#pragma once

#include <cstdint>

#include <boost/thread/barrier.hpp>
#include <boost/thread/thread.hpp>

#include <asio.hpp>

/// Alias for asyncronous i/o implementation namespace (either boost::asio or pure asio).
namespace io = asio;

namespace testing {

namespace util {

using loop_t = io::io_service;

static const std::uint64_t TIMEOUT = 1000;

/// An OS should select available port for us.
std::uint16_t port();

class server_t {
    loop_t loop;
    std::unique_ptr<loop_t::work> work;
    boost::thread thread;

public:
    std::vector<std::shared_ptr<io::ip::tcp::socket>> sockets;

    server_t(std::uint16_t port, std::function<void(io::ip::tcp::acceptor&, loop_t&)> fn) :
        work(new loop_t::work(loop))
    {
        boost::barrier barrier(2);
        thread = std::move(boost::thread([this, port, fn, &barrier]{
            io::ip::tcp::acceptor acceptor(loop);
            io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);
            acceptor.open(endpoint.protocol());
            acceptor.bind(endpoint);
            acceptor.listen();

            barrier.wait();

            fn(acceptor, loop);
        }));

        barrier.wait();
    }

    ~server_t() {
        work.reset();
        thread.join();
    }

    void stop() {
        work.reset();
    }
};

class client_t {
    loop_t io;
    std::unique_ptr<loop_t::work> work;
    boost::thread thread;

public:
    client_t() :
        work(new loop_t::work(io)),
        thread(std::bind(static_cast<std::size_t(loop_t::*)()>(&loop_t::run), std::ref(io)))
    {}

    ~client_t() {
        work.reset();
        thread.join();
    }

    loop_t& loop() {
        return io;
    }
};

} // namespace util

} // namespace testing
