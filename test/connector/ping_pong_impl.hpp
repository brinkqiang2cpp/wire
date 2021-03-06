/*
 * ping_pong_impl.hpp
 *
 *  Created on: May 6, 2016
 *      Author: zmij
 */

#ifndef CONNECTOR_PING_PONG_IMPL_HPP_
#define CONNECTOR_PING_PONG_IMPL_HPP_

#include <test/ping_pong.hpp>

namespace wire {
namespace test {

class ping_pong_server : public ::test::ping_pong {
public:
    using stop_callback = ::std::function<void()>;
public:
    ping_pong_server(stop_callback on_stop) : on_stop_{on_stop} {}
    ::std::int32_t
    test_int(::std::int32_t val,
            ::wire::core::current const& = ::wire::core::no_current) const override;

    void
    test_string(::std::string&& val,
            test_string_return_callback __resp,
            ::wire::core::functional::exception_callback __exception,
            ::wire::core::current const& = ::wire::core::no_current) override;

    void
    test_struct(::test::data&& val,
            test_struct_return_callback __resp,
            ::wire::core::functional::exception_callback __exception,
            ::wire::core::current const& = ::wire::core::no_current) const override;

    void
    test_callback(::test::ping_pong::callback_prx cb,
            test_callback_return_callback __resp,
            ::wire::core::functional::exception_callback __exception,
            ::wire::core::current const& = ::wire::core::no_current) override;

    void
    test_ball(::test::ball_ptr b,
            test_ball_return_callback __resp,
            ::wire::core::functional::exception_callback __exception,
            ::wire::core::current const& = ::wire::core::no_current) override;

    void
    error(::std::string&& message, ::wire::core::current const& = ::wire::core::no_current) override;

    void
    async_error(::std::string&& message,
            ::wire::core::functional::void_callback __resp,
            ::wire::core::functional::exception_callback __exception,
            ::wire::core::current const& = ::wire::core::no_current) const override;

    void
    throw_unexpected(::std::string&& message, ::wire::core::current const& = ::wire::core::no_current) override;

    void
    stop(::wire::core::functional::void_callback __resp,
            ::wire::core::functional::exception_callback __exception,
            ::wire::core::current const& = ::wire::core::no_current) override;
private:
    stop_callback   on_stop_;
};

}  /* namespace test */
}  /* namespace wire */

#endif /* CONNECTOR_PING_PONG_IMPL_HPP_ */
