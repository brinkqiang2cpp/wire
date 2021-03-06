/*
 * base_errors.hpp
 *
 *  Created on: 21 янв. 2016 г.
 *      Author: sergey.fedorov
 */

#ifndef WIRE_ERRORS_EXCEPTIONS_HPP_
#define WIRE_ERRORS_EXCEPTIONS_HPP_

#include <stdexcept>
#include <wire/util/concatenate.hpp>
#include <iosfwd>

namespace wire {
namespace errors {

/**
 * Base class for all wire runtime exceptions
 */
class runtime_error : public ::std::runtime_error {
public:
    runtime_error()
        : ::std::runtime_error{""}, message_{} {}
    runtime_error(std::string const& msg )
        : ::std::runtime_error{msg}, message_{msg} {}
    runtime_error(char const* msg)
        : ::std::runtime_error{msg}, message_{msg} {}

    template < typename ... T >
    runtime_error(T const& ... args)
        : ::std::runtime_error{""}, message_{ util::concatenate(args ...) } {}
    char const*
    what() const noexcept override;
protected:
    virtual void
    stream_message(::std::ostream& os) const;
    ::std::string mutable message_;
};

class connector_destroyed : public runtime_error {
public:
    connector_destroyed(std::string const& msg) : runtime_error{msg} {}
    connector_destroyed(char const* msg) : runtime_error{msg} {}
};

class adapter_destroyed : public runtime_error {
public:
    adapter_destroyed(std::string const& msg) : runtime_error{msg} {}
    adapter_destroyed(char const* msg) : runtime_error{msg} {}
};

class no_locator : public runtime_error {
public:
    no_locator() : runtime_error{"Locator is not configured"} {}
};

/**
 * Connection has been closed, either on timeout, or on error or by user request
 */
class connection_closed : public runtime_error {
public:
    connection_closed(std::string const& msg) : runtime_error{msg} {}
    connection_closed(char const* msg) : runtime_error{msg} {}
    template < typename ... T >
    connection_closed(T const& ... args) : runtime_error(args ...) {}
};

/**
 * Failed to initiate connection
 */
class connection_refused : public connection_closed {
public:
    connection_refused(std::string const& msg) : connection_closed{msg} {}
    connection_refused(char const* msg) : connection_closed{msg} {}
    template < typename ... T >
    connection_refused(T const& ... args) : connection_closed(args ...) {}
};

/**
 * Connection was unexpectedly closed
 */
class connection_failed : public connection_closed {
public:
    connection_failed(std::string const& message) : connection_closed(message) {}
    connection_failed(char const* msg) : connection_closed{msg} {}
    template < typename ... T >
    connection_failed(T const& ... args) : connection_closed(args ...) {}
};


class request_timed_out : public runtime_error {
public:
    request_timed_out(std::string const& msg) : runtime_error{msg} {}
    request_timed_out(char const* msg) : runtime_error{msg} {}
    template < typename ... T >
    request_timed_out(T const& ... args) : runtime_error(args ...) {}
};

class invalid_one_way_invocation : public runtime_error {
public:
    invalid_one_way_invocation(std::string const& msg) : runtime_error{msg} {}
    template < typename ... T >
    invalid_one_way_invocation(T const& ... args) : runtime_error(args ...) {}
};

class invalid_magic_number : public runtime_error {
public:
    invalid_magic_number(std::string const& message) : runtime_error(message) {}
};

class marshal_error : public runtime_error {
public:
    marshal_error(std::string const& message) : runtime_error(message) {}
    marshal_error(char const* message) : runtime_error(message) {}
};

class unmarshal_error : public runtime_error {
public:
    unmarshal_error(std::string const& message) : runtime_error(message) {}
    unmarshal_error(char const* message) : runtime_error(message) {}
    template < typename ... T >
    unmarshal_error(T const& ... args) : runtime_error(args ...) {}
};

class logic_error : public ::std::logic_error {
public:
    logic_error(std::string const& message ) : ::std::logic_error(message) {}
    template < typename ... T >
    logic_error(T const& ... args)
        : ::std::logic_error( util::concatenate(args ...) ) {}
};

}  // namespace errors
}  // namespace wire



#endif /* WIRE_ERRORS_EXCEPTIONS_HPP_ */
