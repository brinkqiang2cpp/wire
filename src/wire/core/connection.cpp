/*
 * connection.cpp
 *
 *  Created on: Jan 28, 2016
 *      Author: zmij
 */

#include <wire/core/connection.hpp>
#include <wire/core/adapter.hpp>
#include <wire/core/detail/connection_impl.hpp>
#include <wire/core/detail/dispatch_request.hpp>
#include <wire/core/detail/configuration_options.hpp>
#include <wire/core/current.hpp>
#include <wire/core/object.hpp>
#include <wire/encoding/message.hpp>
#include <wire/errors/user_exception.hpp>

#include <cxxabi.h>
#include <iterator>

namespace wire {
namespace core {

constexpr invocation_options::timeout_type invocation_options::default_timout;
constexpr invocation_options::retry_count_type invocation_options::no_retries;
constexpr invocation_options::retry_count_type invocation_options::infinite_retries;
constexpr invocation_options::timeout_type invocation_options::default_retry_timout;

const invocation_options invocation_options::unspecified{ invocation_flags::unspecified };

namespace detail {

using tcp_connection_impl           = connection_impl< transport_type::tcp >;
using ssl_connection_impl           = connection_impl< transport_type::ssl >;
using socket_connection_impl        = connection_impl< transport_type::socket >;

using tcp_listen_connection_impl    = listen_connection_impl< transport_type::tcp >;
using ssl_listen_connection_impl    = listen_connection_impl< transport_type::ssl >;
using socket_listen_connection_impl = listen_connection_impl< transport_type::socket >;

const encoding::request_result_callback dispatch_request::ignore_result
    = [](encoding::outgoing&&){};
const functional::exception_callback dispatch_request::ignore_exception
    = [](::std::exception_ptr){};

namespace {

::std::string
demangle(char const* type_name)
{
    int status {0};
    char* ret = abi::__cxa_demangle( type_name, nullptr, nullptr, &status );
    ::std::string res{ret};
    if(ret) free(ret);
    return res;
}

} /* namespace  */


class invocation_foolproof_guard {
    using atomic_flag               = ::std::atomic_flag;
    functional::void_callback       destroy_;
    atomic_flag                     responded_{false};
public:
    invocation_foolproof_guard(functional::void_callback on_destroy)
        : destroy_{on_destroy}
    {}
    ~invocation_foolproof_guard()
    {
        if (respond() && destroy_) {
            destroy_();
        }
    }
    bool
    respond()
    {
        return !responded_.test_and_set();
    }
};

connection_impl_ptr
connection_implementation::create_connection(adapter_ptr adptr, transport_type _type,
        functional::void_callback on_close)
{
    switch (_type) {
        case transport_type::tcp :
            return ::std::make_shared< tcp_connection_impl >( client_side{}, adptr, on_close );
        case transport_type::ssl:
            return ::std::make_shared< ssl_connection_impl >( client_side{}, adptr, on_close );
        case transport_type::socket :
            return ::std::make_shared< socket_connection_impl >( client_side{}, adptr, on_close );
        default:
            break;
    }
    throw errors::logic_error(_type, " connection is not implemented yet");
}

template < typename ListenConnection, typename Session >
::std::shared_ptr< ListenConnection >
create_listen_connection_impl(adapter_ptr adptr, functional::void_callback on_close)
{
    adapter_weak_ptr adptr_weak = adptr;
    return ::std::make_shared< ListenConnection >(
    adptr,
    [adptr_weak, on_close]( asio_config::io_service_ptr svc) {
        adapter_ptr a = adptr_weak.lock();
        // TODO Throw an error if adapter gone away
        if (!a) {
            throw errors::adapter_destroyed{ "Adapter gone away" };
        }
        return ::std::make_shared< Session >( server_side{}, a, on_close );
    }, on_close);
}

connection_impl_ptr
connection_implementation::create_listen_connection(adapter_ptr adptr, transport_type _type,
        functional::void_callback on_close)
{
    adapter_weak_ptr adptr_weak = adptr;
    switch (_type) {
        case transport_type::tcp:
            return create_listen_connection_impl<
                    tcp_listen_connection_impl,
                    tcp_connection_impl >(adptr, on_close);
        case transport_type::ssl:
            return create_listen_connection_impl<
                    ssl_listen_connection_impl,
                    ssl_connection_impl >(adptr, on_close);
        case transport_type::socket:
            return create_listen_connection_impl<
                    socket_listen_connection_impl,
                    socket_connection_impl >(adptr, on_close);
        default:
            break;
    }
    throw errors::logic_error(_type, " listen connection is not implemented yet");
}

void
connection_implementation::set_connect_timer()
{
    auto _this = shared_from_this();
    connection_timer_.expires_from_now(::std::chrono::seconds{1});
    connection_timer_.async_wait(
        [_this](asio_config::error_code const& ec)
        {
            _this->on_connect_timeout(ec);
        });
}

void
connection_implementation::cancel_connect_timer()
{
    connection_timer_.cancel();
}

void
connection_implementation::on_connect_timeout(asio_config::error_code const& ec)
{
    if (!ec) {
        connection_failure(
            ::std::make_exception_ptr(errors::connection_failed("Connection timed out")));
    }
}

void
connection_implementation::set_idle_timer()
{
    if (can_drop_connection()) {
        auto const& opts = get_connector()->options();
        #if DEBUG_OUTPUT >= 5
        auto cancelled_events =
        #endif
        connection_timer_.expires_from_now(::std::chrono::milliseconds{opts.connection_idle_timeout});
        #if DEBUG_OUTPUT >= 5
        if (cancelled_events) {
            ::std::ostringstream os;
            tag(os) << " Reset timer\n";
            ::std::cerr << os.str();
        } else {
            ::std::ostringstream os;
            tag(os) << " Set timer\n";
            ::std::cerr << os.str();
        }
        #endif
        auto _this = shared_from_this();
        connection_timer_.async_wait(
                [_this](asio_config::error_code const& ec)
                {
                    _this->on_idle_timeout(ec);
                });
    }
}

void
connection_implementation::on_idle_timeout(asio_config::error_code const& ec)
{
    if (!ec) {
        #if DEBUG_OUTPUT >= 5
        ::std::ostringstream os;
        tag(os) << " Timer expired\n";
        ::std::cerr << os.str();
        #endif
        if (can_drop_connection())
            process_event(events::close{});
    }
}

bool
connection_implementation::can_drop_connection() const
{
    if (get_connector()->options().enable_connection_timeouts) {
        // TODO Check if peer endpoint has a routable bidir adapter
        return pending_replies_.empty() && outstanding_responses_ <= 0;
    }
    return false; // Turn off connection timeout
}

void
connection_implementation::start_request_timer()
{
    request_timer_.expires_from_now(::std::chrono::milliseconds{500});
    request_timer_.async_wait(::std::bind(&connection_implementation::on_request_timeout,
                    shared_from_this(), ::std::placeholders::_1));
}

void
connection_implementation::request_error(request_number r_no,
        ::std::exception_ptr ex)
{
    pending_replies_type::accessor acc;
    if (pending_replies_.find(acc, r_no)) {
        DEBUG_LOG_TAG(3, tag, "Request #" << r_no << " connection error");
        auto const& p_rep = acc->second;
        auto elapsed = clock_type::now() - p_rep.start;
        observer_.invocation_error(r_no, p_rep.target, p_rep.operation,
                remote_endpoint(), p_rep.sent, ex, elapsed);
        if (p_rep.error) {
            auto err_handler = p_rep.error;
            io_service_->post(
            [err_handler, ex]()
            {
                try {
                    err_handler(ex);
                } catch(...) {}
            });
        }
        pending_replies_.erase(acc);
    }
}

void
connection_implementation::on_request_timeout(asio_config::error_code const& ec)
{
    static errors::request_timed_out err{ "Request timed out" };

    auto now = clock_type::now();
    reply_expiration r_exp;
    while (expiration_queue_.try_pop(r_exp)) {
        if (r_exp.expires <= now) {
            DEBUG_LOG_TAG(3, tag, "Request #" << r_exp.number << " timed out");
            request_error(r_exp.number, ::std::make_exception_ptr(err));
        } else {
            expiration_queue_.push(r_exp);
            break;
        }
    }
    start_request_timer();
}

void
connection_implementation::connect_async(endpoint const& ep,
        functional::void_callback cb, functional::exception_callback eb)
{
    mode_ = client;
    process_event(events::connect{ ep, cb, eb });
}

void
connection_implementation::start_session()
{
    mode_ = server;
    #if DEBUG_OUTPUT >= 1
    ::std::ostringstream os;
    tag(os) << " Start server session\n";
    ::std::cerr << os.str();
    #endif
    process_event(events::start{});
}

void
connection_implementation::start_server_session()
{
    do_start_session(
        ::std::bind(&connection_implementation::handle_started,
                shared_from_this(), ::std::placeholders::_1));
}

void
connection_implementation::handle_started(asio_config::error_code const& ec)
{
    #if DEBUG_OUTPUT >= 1
    ::std::ostringstream os;
    tag(os) << " Handle started\n";
    ::std::cerr << os.str();
    #endif
    if (!ec) {
        process_event(events::started{});
        auto adp = adapter_.lock();
        if (adp) {
            adp->connection_online(local_endpoint(), remote_endpoint());
        }
        observer_.connect(remote_endpoint());
    } else {
        connection_failure(
            ::std::make_exception_ptr(errors::connection_failed(ec.message())));
    }
}

void
connection_implementation::listen(endpoint const& ep, bool reuse_port)
{
    mode_ = server;
    do_listen(ep, reuse_port);
}

void
connection_implementation::handle_connected(asio_config::error_code const& ec)
{
    #if DEBUG_OUTPUT >= 1
    ::std::ostringstream os;
    tag(os) << " Handle connected\n";
    ::std::cerr << os.str();
    #endif
    if (!ec) {
        process_event(events::connected{});
        auto adp = adapter_.lock();
        if (adp) {
            adp->connection_online(local_endpoint(), remote_endpoint());
        }
        observer_.connect(remote_endpoint());
        start_request_timer();
    } else {
        connection_failure(
            ::std::make_exception_ptr(errors::connection_refused(ec.message())));
    }
}

void
connection_implementation::send_validate_message()
{
    encoding::outgoing_ptr out =
        ::std::make_shared<encoding::outgoing>(
                get_connector(),
                encoding::message::validate_flags);
    auto _this = shared_from_this();
    write_async(out,
    [_this]()
    {
        DEBUG_LOG_TAG(3, _this->tag, "Validate message sent");
        _this->process_event(events::validate_sent{});
    });
}


void
connection_implementation::close()
{
    process_event(events::close{});
}

void
connection_implementation::send_close_message()
{
    encoding::outgoing_ptr out =
            ::std::make_shared<encoding::outgoing>(
                    get_connector(),
                    encoding::message::close);
    write_async(out);
}

void
connection_implementation::handle_close()
{
    DEBUG_LOG_TAG(1, tag, "Handle close");

    errors::connection_closed err{ "Connection closed" };
    auto ex = ::std::make_exception_ptr(err);

    cancel_connect_timer();
    request_timer_.cancel();

    reply_expiration r_exp;
    while (expiration_queue_.try_pop(r_exp)) {
        request_error(r_exp.number, ex);
    }

    if (on_close_)
        on_close_();
    observer_.disconnect(remote_endpoint());
}

void
connection_implementation::write_async(encoding::outgoing_ptr out,
        functional::void_callback cb)
{
    DEBUG_LOG_TAG(3, tag, "Send " << out->type() << " size " << out->size());
    if (!is_terminated() && is_open()) {
        do_write_async( out,
            ::std::bind(&connection_implementation::handle_write, shared_from_this(),
                ::std::placeholders::_1, ::std::placeholders::_2, cb, out));
    }
}

void
connection_implementation::handle_write(asio_config::error_code const& ec, ::std::size_t bytes,
        functional::void_callback cb, encoding::outgoing_ptr out)
{
    if (!ec) {
        DEBUG_LOG_TAG(3, tag, "Write operation finished. Bytes written: " << bytes)
        observer_.send_bytes(bytes, remote_endpoint());
        set_idle_timer();
        process_event(events::write_done{});
        if (cb) cb();
    } else {
        DEBUG_LOG_TAG(2, tag, "Write failed " << ec.message());
        connection_failure(
            ::std::make_exception_ptr(errors::connection_failed(ec.message())));
    }
}

void
connection_implementation::start_read()
{
    DEBUG_LOG_TAG(4, tag, "Start read");
    if (!is_terminated() && is_open()) {
        incoming_buffer_ptr buffer = ::std::make_shared< incoming_buffer >();
        read_async(buffer);
    }
}

void
connection_implementation::read_async(incoming_buffer_ptr buffer)
{
    do_read_async(buffer,
        ::std::bind(&connection_implementation::handle_read, shared_from_this(),
                ::std::placeholders::_1, ::std::placeholders::_2, buffer));
}

void
connection_implementation::handle_read(asio_config::error_code const& ec, ::std::size_t bytes,
        incoming_buffer_ptr buffer)
{
    if (!ec) {
        DEBUG_LOG_TAG(4, tag, "Received " << bytes << " bytes");
        observer_.receive_bytes(bytes, remote_endpoint());
        process_event(events::receive_data{buffer, bytes});
        start_read();
        set_idle_timer();
    } else {
        DEBUG_LOG_TAG(2, tag, "Read failed " << ec.message());
        connection_failure(
            ::std::make_exception_ptr(errors::connection_failed(ec.message())));
    }
}

void
connection_implementation::process_message(encoding::message m,
            incoming_buffer::iterator& b, incoming_buffer::iterator e)
{
    using encoding::message;
    switch(m.type()) {
        case message::validate: {
            if (m.size > 0) {
                throw errors::connection_failed("Invalid validate message");
            }
            process_event(events::receive_validate{});
            break;
        }
        case message::close : {
            if (m.size > 0) {
                throw errors::connection_failed("Invalid close message");
            }
            process_event(events::receive_close{});
            break;
        }
        default: {
            if (m.size == 0) {
                throw errors::connection_failed(
                        "Zero sized ", m.type(), " message");
            }
            DEBUG_LOG_TAG(5, tag, "Receive " << m.type()
                    << " size " << m.size << " buffer remains: "
                    << (e - b));

            encoding::incoming_ptr incoming =
                ::std::make_shared< encoding::incoming >( get_connector(), m, b, e );
            if (!incoming->complete()) {
                DEBUG_LOG_TAG(5, tag, "Wait for more data from peer");
                incoming_ = incoming;
            } else {
                DEBUG_LOG_TAG(5, tag, "Dispatch message");
                dispatch_incoming(incoming);
            }
        }
    }
}

void
connection_implementation::read_incoming_message(incoming_buffer_ptr buffer, ::std::size_t bytes)
{
    using encoding::message;
    auto b = buffer->begin();
    auto e = b + bytes;
    try {
        while (b != e) {
            DEBUG_LOG_TAG(5, tag, "Buffer size " << e - b );
            if (!carry_.empty()) {
                DEBUG_LOG_TAG(5, tag, "Read message with carry. Carry size " << carry_.size() );
                auto need_bytes = message::max_header_size - carry_.size();
                for (auto i = 0U; i < need_bytes && b != e; ++i) {
                    carry_.push_back(*b++);
                }
                auto cb = carry_.begin();
                auto ce = carry_.end();
                message m;
                if (try_read(cb, ce, m)) {
                    // Rewind b
                    b -= ce - cb;
                    carry_.clear();

                    bytes -= b - buffer->begin();
                    process_message(m, b, e);
                }
                // If we fail to read the message with carry
                // it means we exhausted the buffer and moved it to the carry.
                // b == e, carry is filled with needed bytes
            } else if (incoming_) {
                DEBUG_LOG_TAG(3, tag,
                    "Incomplete message is pending. Message current size: "
                    << incoming_->size()
                    << " expected " << incoming_->header().size);

                incoming_->insert_back(b, e);
                if (incoming_->complete()) {
                    DEBUG_LOG_TAG(3, tag,
                        "Pending message complete size: " << incoming_->size()
                        << " (expected " << incoming_->header().size << ")");
                    dispatch_incoming(incoming_);
                    incoming_.reset();
                #if DEBUG_OUTPUT >= 3
                } else {
                    DEBUG_LOG_TAG(3, tag, "Pending message size: " << incoming_->size()
                        << " (expected " << incoming_->header().size << ")");
                #endif
                }
            } else {
                DEBUG_LOG_TAG(3, tag, "Read message. Buffer size " << e - b );
                message m;

                if (try_read(b, e, m)) {
                    bytes -= b - buffer->begin();
                    process_message(m, b, e);
                } else {
                    // The buffer was not enough to read the message size.
                    // b != e, need to carry this.
                    break;
                }
            }
        }
        #if DEBUG_OUTPUT >= 3
        if (b != e) {
            DEBUG_LOG_TAG(3, tag, "Add " << e - b << " bytes to the carry");
        }
        #endif
        carry_.insert(carry_.end(), b, e);
    } catch (::std::exception const& e) {
        /** TODO Make it a protocol error? Can we handle it? */
        DEBUG_LOG_TAG(2, tag, "Protocol read exception: " << e.what());
        connection_failure(::std::current_exception());
    }
}

void
connection_implementation::dispatch_incoming(encoding::incoming_ptr incoming)
{
    using encoding::message;
    switch (incoming->type()) {
        case message::request:
            process_event(events::receive_request{ incoming });
            break;
        case message::reply:
            process_event(events::receive_reply{ incoming });
            break;
        default:
            connection_failure(
                ::std::make_exception_ptr(errors::unmarshal_error{ "Unknown message type ", incoming->type() }));
            break;
    }
}

void
connection_implementation::request_sent(request_number r_no,
        functional::callback< bool > sent, bool one_way)
{
    DEBUG_LOG_TAG(3, tag, "Invocation #" << r_no << " has been sent");

    pending_replies_type::accessor acc;
    if (pending_replies_.find(acc, r_no)) {
        if (one_way) {
            pending_replies_.erase(acc);
        } else {
            acc->second.sent = true;
        }
    }
    if (sent)
        sent(true);
}

void
connection_implementation::invoke(encoding::invocation_target const& target,
        encoding::operation_specs::operation_id const& op,
        context_type const& ctx,
        invocation_options const& opts,
        encoding::outgoing&& params,
        encoding::reply_callback reply,
        functional::exception_callback exception,
        functional::callback< bool > sent)
{
    using encoding::request;
    encoding::outgoing_ptr out = ::std::make_shared<encoding::outgoing>(
            get_connector(),
            encoding::message::request);
    request r{
        ++request_no_,
        encoding::operation_specs{ target, op },
        request::normal
    };
    observer_.invoke_remote(r.number, target, op, remote_endpoint());
    if (ctx.empty())
        r.mode |= request::no_context;
    if (opts.is_one_way())
        r.mode |= request::one_way;

    DEBUG_LOG_TAG(3, tag, "Invoke request " << target.identity << "::" << op
            << " #" << r.number);

    write(::std::back_inserter(*out), r);
    if (!ctx.empty())
        write(::std::back_inserter(*out), ctx);
    params.close_all_encaps();
    out->insert_encapsulation(::std::move(params));
    time_point expires = clock_type::now() + expire_duration{opts.timeout};
    pending_replies_.insert(::std::make_pair( r.number,
            pending_reply{ target, op, reply, exception, clock_type::now(), false } ));
    expiration_queue_.push(reply_expiration{ r.number, expires });
    auto _this = shared_from_this();
    auto r_no = r.number;
    bool one_way = opts.is_one_way();
    functional::void_callback write_cb = [_this, r_no, sent, one_way]()
        {
            _this->request_sent(r_no, sent, one_way);
        };
    process_event(events::send_request{ out, write_cb });

    if (opts.is_sync()) {
        // TODO Decide what to do in case of one way invocation
        util::run_while(io_service_, [_this, r_no](){
            pending_replies_type::const_accessor acc;
            return _this->pending_replies_.find(acc, r_no);
        });
    }
}

void
connection_implementation::send(encoding::multiple_targets const& targets,
        encoding::operation_specs::operation_id const& op,
        context_type const& ctx,
        invocation_options const& opts,
        encoding::outgoing&& params,
        functional::exception_callback exception,
        functional::callback< bool > sent)
{
    if (targets.empty()) {
        throw errors::runtime_error{"Empty target list"};
    } else if (targets.size() == 1) {
        DEBUG_LOG_TAG(1, tag, "Send single invocation via invoke");
        invoke(*targets.begin(), op, ctx,
                opts | invocation_flags::one_way,
                ::std::move(params), nullptr, exception, sent);
    } else {
        using encoding::request;
        encoding::outgoing_ptr out = ::std::make_shared<encoding::outgoing>(
                get_connector(),
                encoding::message::request);
        request r{
            ++request_no_,
            encoding::operation_specs{ encoding::invocation_target{}, op },
            request::multi_target | request::one_way
        };
        if (ctx.empty())
            r.mode |= request::no_context;

        write(::std::back_inserter(*out), r);
        if (!ctx.empty())
            write(::std::back_inserter(*out), ctx);
        write(::std::back_inserter(*out), targets);

        params.close_all_encaps();
        out->insert_encapsulation(::std::move(params));

        auto _this = shared_from_this();
        auto r_no = r.number;
        functional::void_callback write_cb = [_this, r_no, sent]()
            {
                _this->request_sent(r_no, sent, true);
            };
        process_event(events::send_request{ out, write_cb });
    }
}

void
connection_implementation::forward(encoding::multiple_targets const& targets,
        encoding::operation_specs::operation_id const& op,
        context_type const& ctx,
        invocation_options const& opts,
        detail::dispatch_request const& req,
        encoding::reply_callback reply,
        functional::exception_callback exception,
        functional::callback< bool > sent)
{
    using namespace encoding;
    if (targets.empty()) {
        throw errors::runtime_error{"Empty invocation target list"};
    } else {
        request::request_mode mode{};
        if (targets.size() > 1) {
            mode = request::multi_target | request::one_way;
        }
        if (opts.is_one_way()) {
            mode |= request::one_way;
        }
        outgoing_ptr out = ::std::make_shared<outgoing>(
                        get_connector(),
                        message::request);
        invocation_target tgt = targets.size() == 1 ?
                *targets.begin() : invocation_target{};
        request r{
            ++request_no_,
            encoding::operation_specs{ tgt, op },
            mode
        };
        if (ctx.empty())
            r.mode |= request::no_context;

        write(::std::back_inserter(*out), r);
        if (!ctx.empty())
            write(::std::back_inserter(*out), ctx);
        if (targets.size() > 1)
            write(::std::back_inserter(*out), targets);
        auto encaps = out->begin_encapsulation();
        ::std::copy(req.encaps_start, req.encaps_end,
                ::std::back_inserter(*out));
        encaps.end_encaps();

        if (!(r.mode & request::one_way)) {
            time_point expires = clock_type::now() + expire_duration{opts.timeout};
            pending_replies_.insert(::std::make_pair( r.number,
                    pending_reply{ encoding::invocation_target{}, op,
                        reply, exception, clock_type::now(), false } ));
            //}
            expiration_queue_.push(reply_expiration{ r.number, expires });
        }

        auto _this = shared_from_this();
        auto r_no = r.number;
        bool one_way = r.mode & request::one_way;
        functional::void_callback write_cb = [_this, r_no, sent, one_way]()
            {
                _this->request_sent(r_no, sent, one_way);
            };
        process_event(events::send_request{ out, write_cb });
    }
}

void
connection_implementation::dispatch_reply(encoding::incoming_ptr buffer)
{
    using namespace encoding;
    try {
        reply rep;
        incoming::const_iterator b = buffer->begin();
        incoming::const_iterator e = buffer->end();
        read(b, e, rep);
        DEBUG_LOG_TAG(3, tag, "Dispatch reply #" << rep.number);
        auto peer_ep = remote_endpoint();
        pending_replies_type::accessor acc;
        if (pending_replies_.find(acc, rep.number)) {
            auto const& p_rep = acc->second;
            auto elapsed = clock_type::now() - p_rep.start;
            switch (rep.status) {
                case reply::success:{
                    DEBUG_LOG_TAG(3, tag, "Reply #" << rep.number << " status is success");
                    if (p_rep.reply) {
                        incoming::encaps_guard encaps{buffer->begin_encapsulation(b)};

                        #if DEBUG_OUTPUT >= 3
                        version const& ever = encaps.encaps().encoding_version();
                        DEBUG_LOG_TAG(3, tag, "Reply encaps v" << ever.major << "." << ever.minor
                                << " size " << encaps.size());
                        #endif
                        observer_.invocation_ok(rep.number,
                                p_rep.target, p_rep.operation,
                                peer_ep, elapsed);
                        try {
                            p_rep.reply(encaps->begin(), encaps->end());
                        } catch (...) {
                            // Ignore handler error
                        }
                    }
                    break;
                }
                case reply::success_no_body: {
                    DEBUG_LOG_TAG(3, tag, "Reply #" << rep.number
                            << " status is success without body");
                    observer_.invocation_ok(rep.number,
                            p_rep.target, p_rep.operation,
                            peer_ep, elapsed);
                    if (p_rep.reply) {
                        try {
                            p_rep.reply( incoming::const_iterator{}, incoming::const_iterator{} );
                        } catch (...) {
                            // Ignore handler error
                        }
                    }
                    break;
                }
                case reply::no_object:
                case reply::no_facet:
                case reply::no_operation: {
                    DEBUG_LOG_TAG(3, tag, "Reply #" << rep.number << " status is not found");
                    if (p_rep.error) {
                        incoming::encaps_guard encaps{buffer->begin_encapsulation(b)};

                        #if DEBUG_OUTPUT >= 3
                        version const& ever = encaps.encaps().encoding_version();
                        DEBUG_LOG_TAG(3, tag, "Reply encaps v" << ever.major << "." << ever.minor
                                << " size " << encaps.size());
                        #endif
                        encoding::operation_specs op;
                        auto b = encaps->begin();
                        read(b, encaps->end(), op);
                        encaps->read_indirection_table(b);
                        auto ex = ::std::make_exception_ptr(
                                    errors::not_found{
                                        static_cast< errors::not_found::subject >(
                                                rep.status - reply::no_object ),
                                        op.target.identity,
                                        op.target.facet,
                                    op.operation
                                });
                        observer_.invocation_error(rep.number,
                                p_rep.target, p_rep.operation,
                                peer_ep, p_rep.sent, ex, elapsed);
                        try {
                            p_rep.error(ex);
                        } catch (...) {
                            // Ignore handler error
                        }
                    }
                    break;
                }
                case reply::user_exception:
                case reply::unknown_wire_exception:
                case reply::unknown_user_exception:
                case reply::unknown_exception: {
                    DEBUG_LOG_TAG(3, tag, "Reply #" << rep.number << " status is an exception");
                    if (p_rep.error) {
                        incoming::encaps_guard encaps{buffer->begin_encapsulation(b)};

                        #if DEBUG_OUTPUT >= 3
                        version const& ever = encaps.encaps().encoding_version();
                        DEBUG_LOG_TAG(3, tag, "Reply encaps v" << ever.major << "." << ever.minor
                                << " size " << encaps.size());
                        #endif
                        errors::user_exception_ptr exc;
                        auto b = encaps->begin();
                        read(b, encaps->end(), exc);
                        encaps->read_indirection_table(b);
                        auto ex = exc->make_exception_ptr();
                        observer_.invocation_error(rep.number,
                                p_rep.target, p_rep.operation,
                                peer_ep, p_rep.sent, ex, elapsed);
                        try {
                            p_rep.error(ex);
                        } catch (...) {
                            // Ignore handler error
                        }
                    }
                    break;
                }
                default:
                    if (p_rep.error) {
                        auto ex = ::std::make_exception_ptr(
                                errors::unmarshal_error{ "Unhandled reply status" } );
                        observer_.invocation_error(rep.number,
                                p_rep.target, p_rep.operation,
                                peer_ep, p_rep.sent, ex, elapsed);
                        try {
                            p_rep.error(ex);
                        } catch (...) {
                            // Ignore handler error
                        }
                    }
                    break;
            }
            pending_replies_.erase(acc);
            DEBUG_LOG_TAG(3, tag, "Pending replies: " << pending_replies_.size());
        } else {
            // else discard the reply (it can be timed out)
            DEBUG_LOG_TAG(4, tag, "No waiting callback for reply #" << rep.number);
        }
    } catch (::std::exception const& e) {
        DEBUG_LOG_TAG(2, tag, " Exception when reading reply: " << e.what());
        connection_failure(::std::current_exception());
    } catch (...) {
        DEBUG_LOG_TAG(2, tag, "Exception when reading reply");
        connection_failure(::std::current_exception());
    }
}

void
connection_implementation::send_not_found(
        request_number req_num, errors::not_found::subject subj,
        encoding::operation_specs const& op)
{
    using namespace encoding;
    outgoing_ptr out =
            ::std::make_shared<outgoing>(get_connector(), message::reply);
    reply::reply_status status =
            static_cast<reply::reply_status>(reply::no_object + subj);
    reply rep { req_num, status };
    write(::std::back_inserter(*out), rep);
    {
        outgoing::encaps_guard guard{ out->begin_encapsulation() };
        write(::std::back_inserter(*out), op);
    }
    process_event(events::send_reply{out});
}

void
connection_implementation::send_exception(request_number req_num, ::std::exception_ptr ex,
        encoding::operation_specs const& op)
{
    try {
        ::std::rethrow_exception(ex);
    } catch (errors::not_found const& e) {
        send_not_found(req_num, e.subj(), op);
    } catch (errors::user_exception const& e) {
        send_exception(req_num, e);
    } catch (::std::exception const& e) {
        send_exception(req_num, e);
    } catch(...) {
        send_unknown_exception(req_num);
    }
}

void
connection_implementation::send_exception(request_number req_num, errors::user_exception const& e)
{
    using namespace encoding;
    outgoing_ptr out =
            ::std::make_shared<outgoing>(get_connector(), message::reply);
    reply rep { req_num, reply::user_exception };
    auto o = ::std::back_inserter(*out);
    write(o, rep);
    {
        outgoing::encaps_guard guard{ out->begin_encapsulation() };
        e.__wire_write(o);
    }
    process_event(events::send_reply{out});
}

void
connection_implementation::send_exception(request_number req_num, ::std::exception const& e)
{
    using namespace encoding;
    outgoing_ptr out =
            ::std::make_shared<outgoing>(get_connector(), message::reply);
    reply rep { req_num, reply::unknown_user_exception };
    auto o = ::std::back_inserter(*out);
    write(o, rep);

    {
        outgoing::encaps_guard guard{ out->begin_encapsulation() };
        errors::unexpected ue { demangle(typeid(e).name()), ::std::string{e.what()} };
        ue.__wire_write(o);
    }
    process_event(events::send_reply{out});
}

void
connection_implementation::send_unknown_exception(request_number req_num)
{
    using namespace encoding;
    using namespace ::std::string_literals;

    outgoing_ptr out =
            ::std::make_shared<outgoing>(get_connector(), message::reply);
    reply rep { req_num, reply::unknown_exception };
    auto o = ::std::back_inserter(*out);
    write(o, rep);

    {
        outgoing::encaps_guard guard{ out->begin_encapsulation() };
        errors::unexpected ue {"Unknown type"s,
            "Unexpected exception not deriving from std::exception"s};
        ue.__wire_write(o);
    }
    process_event(events::send_reply{out});
}

void
connection_implementation::dispatch_incoming_request(encoding::incoming_ptr buffer)
{
    using namespace encoding;
    try {
        request req;
        incoming::const_iterator b = buffer->begin();
        incoming::const_iterator e = buffer->end();
        read(b, e, req);
        //Find invocation by req.operation.identity
        adapter_ptr adp = adapter_.lock();
        if (adp) {
            #if DEBUG_OUTPUT >= 3
            ::std::ostringstream os;
            tag(os) << " Dispatch request #" << req.number << " "
                    << req.operation.operation
                    << " to ";
            if (req.mode & request::multi_target)
                os << "multiple targets\n";
            else
                os << req.operation.target.identity << "\n";
            ::std::cerr << os.str();
            #endif
            time_point req_start = clock_type::now();
            auto peer_ep = remote_endpoint();
            observer_.receive_request(req.number,
                    req.operation.target, req.operation.operation, peer_ep);
            current curr {
                req.operation,
                {},
                peer_ep,
                adp
            };
            if (!(req.mode & request::no_context)) {
                auto ctx_ptr = ::std::make_shared<context_type>();
                read(b, e, *ctx_ptr);
                curr.context = ctx_ptr;
            }

            ::std::shared_ptr<encoding::multiple_targets> targets;
            if (req.mode & request::multi_target) {
                targets = ::std::make_shared< encoding::multiple_targets >();
                read(b, e, *targets);
            }

            incoming::encaps_guard encaps{ buffer->begin_encapsulation(b) };
            auto const& en = encaps.encaps();
            detail::dispatch_request r;
            if (req.mode & request::one_way) {
                r = detail::dispatch_request{
                        buffer, en.begin(), en.end(), en.size(),
                        detail::dispatch_request::ignore_result,
                        detail::dispatch_request::ignore_exception
                    };
            } else {
                auto _this = shared_from_this();
                auto fpg = ::std::make_shared< invocation_foolproof_guard >(
                    [_this, req, req_start]() mutable {
                        #if DEBUG_OUTPUT >= 3
                        ::std::ostringstream os;
                        _this->tag(os) << " Invocation #" << req.number
                                << " to " << req.operation.target.identity
                                << " operation " << req.operation.operation
                                << " failed to respond\n";
                        ::std::cerr << os.str();
                        #endif
                        _this->observer_.request_no_response(req.number,
                                req.operation.target, req.operation.operation,
                                _this->remote_endpoint(),
                                clock_type::now() - req_start);
                        _this->send_not_found(req.number, errors::not_found::object,
                                req.operation);
                    });
                r = detail::dispatch_request{
                        buffer, en.begin(), en.end(), en.size(),
                        [_this, req, fpg, req_start](outgoing&& res) mutable {
                            if (fpg->respond()) {
                                DEBUG_LOG_TAG(3, _this->tag,
                                        "Request #" << req.number << " success responce");
                                _this->observer_.request_ok(req.number,
                                    req.operation.target, req.operation.operation,
                                    _this->remote_endpoint(),
                                    clock_type::now() - req_start);
                                outgoing_ptr out =
                                        ::std::make_shared<outgoing>(_this->get_connector(), message::reply);
                                reply rep {
                                    req.number,
                                    res.empty() ? reply::success_no_body : reply::success
                                };
                                write(::std::back_inserter(*out), rep);
                                if (!res.empty()) {
                                    res.close_all_encaps();
                                    out->insert_encapsulation(::std::move(res));
                                }
                                _this->process_event(events::send_reply{out});
                            } else {
                                _this->observer_.request_double_response(
                                    req.number,
                                    req.operation.target,
                                    req.operation.operation,
                                    _this->remote_endpoint());
                            }
                        },
                        [_this, req, fpg, req_start](::std::exception_ptr ex) mutable {
                            if (fpg->respond()) {
                                #if DEBUG_OUTPUT >= 3
                                ::std::ostringstream os;
                                _this->tag(os) << " Request #" << req.number
                                        << " exception responce\n";
                                ::std::cerr << os.str();
                                #endif
                                _this->observer_.request_error(req.number,
                                    req.operation.target, req.operation.operation,
                                    _this->remote_endpoint(), ex,
                                    clock_type::now() - req_start);
                                try {
                                    ::std::rethrow_exception(ex);
                                } catch (errors::not_found const& e) {
                                    _this->send_not_found(req.number, e.subj(), req.operation);
                                } catch (errors::user_exception const& e) {
                                    _this->send_exception(req.number, e);
                                } catch (::std::exception const& e) {
                                    _this->send_exception(req.number, e);
                                } catch (...) {
                                    _this->send_unknown_exception(req.number);
                                }
                            } else {
                                _this->observer_.request_double_response(
                                    req.number,
                                    req.operation.target,
                                    req.operation.operation,
                                    _this->remote_endpoint());
                            }
                        }
                };
            }
            if (targets) {
                for (auto const& tgt : *targets) {
                    curr.operation.target = tgt;
                    adp->dispatch(r, curr);
                }
                return;
            } else if (adp->dispatch(r, curr)) {
                return;
            } else {
                #if DEBUG_OUTPUT >= 3
                ::std::ostringstream os;
                tag(os) << " No object\n";
                ::std::cerr << os.str();
                #endif
            }
        } else {
            #if DEBUG_OUTPUT >= 3
            ::std::ostringstream os;
            tag(os) << " No adapter\n";
            ::std::cerr << os.str();
            #endif
        }
        // FIXME If the adapter::dispatch returns false, a no response error occurs
        send_not_found(req.number, errors::not_found::object, req.operation);
    } catch (...) {
        connection_failure( ::std::current_exception() );
    }
}

void
connection_implementation::connection_failure(::std::exception_ptr ex)
{
    process_event(events::connection_failure{ ex });
    observer_.connection_failure(remote_endpoint(), ex);
}

}  // namespace detail

connection::connection(client_side const&, adapter_ptr adp, transport_type tt,
        close_callback on_close)
    : pimpl_{}
{
    create_client_connection(adp, tt, on_close);
}

connection::connection(client_side const&, adapter_ptr adp, endpoint const& ep,
        functional::void_callback on_connect,
        functional::exception_callback on_error,
        close_callback on_close)
    : pimpl_{}
{
    if (!ep)
        throw errors::runtime_error{ "Endpoint is invalid", to_string(ep) };
    create_client_connection(adp, ep.transport(), on_close);
    connect_async(ep, on_connect, on_error);
}

connection::connection(server_side const&, adapter_ptr adp, endpoint const& ep)
    : pimpl_{}
{
    pimpl_ = detail::connection_implementation::create_listen_connection(
        adp, ep.transport(),
        [](){
            #if DEBUG_OUTPUT >= 1
            ::std::ostringstream os;
            os << ::getpid() << " Server connection on close\n";
            ::std::cerr << os.str();
            #endif
        });
    pimpl_->listen(ep);
}

connection::~connection()
{
    #if DEBUG_OUTPUT >= 1
    ::std::ostringstream os;
    os << ::getpid() << " Destroy connection façade\n";
    ::std::cerr << os.str();
    #endif
}

void
connection::create_client_connection(adapter_ptr adp, transport_type tt,
        close_callback on_close)
{
    pimpl_ = detail::connection_implementation::create_connection(
            adp, tt,
            [this, on_close](){
                #if DEBUG_OUTPUT >= 1
                ::std::ostringstream os;
                os << ::getpid() << " Client connection on close\n";
                ::std::cerr << os.str();
                #endif
                if (on_close)
                    on_close(this);
            });
}

connector_ptr
connection::get_connector() const
{
    assert(pimpl_.get() && "Connection implementation is not set");
    return pimpl_->get_connector();
}

void
connection::add_observer(connection_observer_ptr observer)
{
    assert(pimpl_.get() && "Connection implementation is not set");
    pimpl_->add_observer(observer);
}

void
connection::remove_observer(connection_observer_ptr observer)
{
    assert(pimpl_.get() && "Connection implementation is not set");
    pimpl_->remove_observer(observer);
}

void
connection::connect_async(endpoint const& ep,
        functional::void_callback       on_connect,
        functional::exception_callback  on_error)
{
    assert(pimpl_.get() && "Connection implementation is not set");
    pimpl_->connect_async(ep, on_connect, on_error);
}

void
connection::set_adapter(adapter_ptr adp)
{
    assert(pimpl_.get() && "Connection implementation is not set");
    pimpl_->adapter_= adp;
}

void
connection::close()
{
    assert(pimpl_.get() && "Connection implementation is not set");
    pimpl_->close();
}

void
connection::invoke(encoding::invocation_target const& target,
        encoding::operation_specs::operation_id const& op,
        context_type const& ctx,
        invocation_options const& opts,
        encoding::outgoing&& params,
        encoding::reply_callback reply,
        functional::exception_callback exception,
        functional::callback< bool > sent)
{
    assert(pimpl_.get() && "Connection implementation is not set");
    pimpl_->invoke(target, op, ctx, opts, ::std::move(params), reply, exception, sent);
}

void
connection::send(encoding::multiple_targets const& targets,
            encoding::operation_specs::operation_id const& op,
            context_type const& ctx,
            invocation_options const& opts,
            encoding::outgoing&& params,
            functional::exception_callback exception,
            functional::callback< bool > sent)
{
    assert(pimpl_.get() && "Connection implementation is not set");
    pimpl_->send(targets, op, ctx, opts, ::std::move(params), exception, sent);
}

void
connection::forward(encoding::multiple_targets const& targets,
        encoding::operation_specs::operation_id const& op,
        context_type const& ctx,
        invocation_options const& opts,
        detail::dispatch_request const& req,
        encoding::reply_callback reply,
        functional::exception_callback exception,
        functional::callback< bool > sent)
{
    assert(pimpl_.get() && "Connection implementation is not set");
    pimpl_->forward(targets, op, ctx, opts, req, reply, exception, sent);
}


endpoint
connection::local_endpoint() const
{
    assert(pimpl_.get() && "Connection implementation is not set");
    return pimpl_->local_endpoint();
}

endpoint
connection::remote_endpoint() const
{
    assert(pimpl_.get() && "Connection implementation is not set");
    return pimpl_->remote_endpoint();
}

}  // namespace core
}  // namespace wire
