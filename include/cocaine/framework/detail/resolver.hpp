#pragma once

#include <boost/asio/ip/tcp.hpp>

#include "cocaine/framework/forwards.hpp"

namespace cocaine {

namespace framework {

namespace detail {

/*!
 * \reentrant
 */
class resolver_t {
public:
    typedef boost::asio::ip::tcp::endpoint endpoint_type;

    struct result_t {
        std::vector<endpoint_type> endpoints;
        unsigned int version;
    };

private:
    scheduler_t& scheduler;
    std::vector<endpoint_type> endpoints_;

public:
    /*!
     * \note sets default endpoint to [::]:10053.
     */
    explicit resolver_t(scheduler_t& scheduler);

    ~resolver_t();

    void timeout(std::chrono::milliseconds);

    std::vector<endpoint_type> endpoints() const;
    void endpoints(std::vector<endpoint_type> endpoints);

    // No queue.
    auto resolve(std::string name) -> typename task<result_t>::future_type;
};

} // namespace detail

} // namespace framework

} // namespace cocaine
