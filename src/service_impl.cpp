#include <cocaine/framework/service.hpp>

#include <cocaine/asio/resolver.hpp>

using namespace cocaine::framework;

namespace {
    template<class... Args>
    struct emptyf {
        static
        void
        call(Args...){
            // pass
        }
    };
}

service_connection_t::service_connection_t(const std::string& name,
                                           std::shared_ptr<service_manager_t> manager,
                                           unsigned int version) :
    m_name(name),
    m_version(version),
    m_manager(manager),
    m_connection_status(service_status::disconnected),
    m_use_default_executor(true),
    m_session_counter(1)
{
    m_channel.reset(new iochannel_t);
}

service_connection_t::service_connection_t(const endpoint_t& endpoint,
                                           std::shared_ptr<service_manager_t> manager,
                                           unsigned int version) :
    m_endpoint(endpoint),
    m_version(version),
    m_manager(manager),
    m_connection_status(service_status::disconnected),
    m_use_default_executor(true),
    m_session_counter(1)
{
    m_channel.reset(new iochannel_t);
}

std::shared_ptr<service_manager_t>
service_connection_t::manager() {
    auto m = m_manager.lock();
    if (m) {
        return m;
    } else {
        throw service_error_t(service_errc::broken_manager);
    }
}

void
service_connection_t::soft_destroy() {
    std::unique_lock<std::recursive_mutex> lock(m_handlers_lock);

    if (m_connection_status == service_status::connecting) {
        throw service_error_t(service_errc::wait_for_connection);
    } else if (m_connection_status != service_status::waiting_for_sessions) {
        m_connection_status = service_status::waiting_for_sessions;

        if (m_handlers.empty()) {
            auto m = m_manager.lock();
            if (m) {
                m->release_connection(shared_from_this());
            }
        }
    }
}

service_connection_t::session_id_t
service_connection_t::create_session(
    std::shared_ptr<detail::service::service_handler_concept_t> handler,
    std::shared_ptr<iochannel_t>& channel
)
{
    std::unique_lock<std::recursive_mutex> lock(m_handlers_lock);
    if (m_connection_status == service_status::disconnected) {
        throw service_error_t(service_errc::not_connected);
    } else if (m_connection_status == service_status::waiting_for_sessions) {
        throw service_error_t(service_errc::wait_for_connection);
    } else {
        session_id_t current_session = m_session_counter++;
        m_handlers[current_session] = handler;
        channel = m_channel;
        return current_session;
    }
}

future<std::shared_ptr<service_connection_t>>
service_connection_t::reconnect() {
    std::unique_lock<std::recursive_mutex> lock(m_handlers_lock);

    if (m_connection_status == service_status::connecting) {
        throw service_error_t(service_errc::wait_for_connection);
    }

    m_connection_status = service_status::disconnected;
    std::shared_ptr<iochannel_t> channel(std::move(m_channel));
    m_channel.reset(new iochannel_t);
    reset_sessions();
    manager()->m_ioservice.post(std::bind(&emptyf<std::shared_ptr<iochannel_t>>::call, channel));

    return connect(lock);
}

future<std::shared_ptr<service_connection_t>>
service_connection_t::connect(std::unique_lock<std::recursive_mutex>& lock) {
    try {
        if (m_name) {
            auto m = manager();
            m_connection_status = service_status::connecting;
            lock.unlock();
            return m->resolve(*m_name).then(std::bind(&service_connection_t::on_resolved,
                                                      this,
                                                      std::placeholders::_1));
        } else {
            m_connection_status = service_status::connecting;
            lock.unlock();
            connect_to_endpoint();
            return make_ready_future<std::shared_ptr<service_connection_t>>::make(shared_from_this());
        }
    } catch (...) {
        return make_ready_future<std::shared_ptr<service_connection_t>>::error(std::current_exception());
    }
}

void
service_connection_t::connect_to_endpoint() {
    try {
        auto m = manager();

        auto socket = std::make_shared<cocaine::io::socket<cocaine::io::tcp>>(m_endpoint);

        m_channel->attach(m->m_ioservice, socket);
        m_channel->rd->bind(std::bind(&service_connection_t::on_message,
                                      this,
                                      std::placeholders::_1),
                            std::bind(&service_connection_t::on_error,
                                      this,
                                      std::placeholders::_1));
        m_channel->wr->bind(std::bind(&service_connection_t::on_error,
                                      this,
                                      std::placeholders::_1));
        m->m_ioservice.post(&emptyf<>::call); // wake up event-loop

        m_connection_status = service_status::connected;
    } catch (...) {
        std::unique_lock<std::recursive_mutex> lock(m_handlers_lock);
        m_connection_status = service_status::disconnected;
        reset_sessions();
        throw;
    }
}

void
service_connection_t::reset_sessions() {
    handlers_map_t handlers;
    handlers.swap(m_handlers);

    service_error_t err(service_errc::not_connected);

    for (auto it = handlers.begin(); it != handlers.end(); ++it) {
        try {
            it->second->error(cocaine::framework::make_exception_ptr(err));
        } catch (...) {
            // optimize it
        }
    }
}

std::shared_ptr<service_connection_t>
service_connection_t::on_resolved(service_traits<cocaine::io::locator::resolve>::future_type& f) {
    try {
        auto service_info = f.next();
        std::string hostname;
        uint16_t port;

        std::tie(hostname, port) = std::get<0>(service_info);

        m_endpoint = cocaine::io::resolver<cocaine::io::tcp>::query(hostname, port);

        if (m_version != std::get<1>(service_info)) {
            throw service_error_t(service_errc::bad_version);
        }
    } catch (...) {
        std::unique_lock<std::recursive_mutex> lock(m_handlers_lock);
        m_connection_status = service_status::disconnected;
        reset_sessions();
        throw;
    }

    connect_to_endpoint();

    return shared_from_this();
}

void
service_connection_t::on_error(const std::error_code& /* code */) {
    reconnect();
}

void
service_connection_t::on_message(const cocaine::io::message_t& message) {
    std::unique_lock<std::recursive_mutex> lock(m_handlers_lock);
    handlers_map_t::iterator it = m_handlers.find(message.band());

    if (it == m_handlers.end()) {
        auto m = m_manager.lock();
        if (m && m->get_system_logger()) {
            COCAINE_LOG_DEBUG(
                m->get_system_logger(),
                "Message with unknown session id has been received from service %s",
                name()
            );
        }
    } else {
        try {
            auto h = it->second;
            if (message.id() == io::event_traits<io::rpc::choke>::id) {
                m_handlers.erase(it);
            }
            if (m_connection_status == service_status::waiting_for_sessions && m_handlers.empty()) {
                auto m = m_manager.lock();
                if (m) {
                    m->release_connection(shared_from_this());
                }
            }
            lock.unlock();
            h->handle_message(message);
        } catch (const std::exception& e) {
            auto m = m_manager.lock();
            if (m && m->get_system_logger()) {
                COCAINE_LOG_WARNING(
                    m->get_system_logger(),
                    "Following error has occurred while handling message from service %s: %s",
                    name(),
                    e.what()
                );
            }
        }
    }
}
