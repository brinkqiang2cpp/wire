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
#include <wire/core/current.hpp>
#include <wire/core/object.hpp>

#include <wire/encoding/message.hpp>
#include <wire/errors/user_exception.hpp>

#include <iterator>

namespace wire {
namespace core {

namespace detail {

using tcp_connection_impl           = connection_impl< transport_type::tcp >;
using ssl_connection_impl           = connection_impl< transport_type::ssl >;
using socket_connection_impl        = connection_impl< transport_type::socket >;

using tcp_listen_connection_impl    = listen_connection_impl< transport_type::tcp >;
using ssl_listen_connection_impl    = listen_connection_impl< transport_type::ssl >;
using socket_listen_connection_impl = listen_connection_impl< transport_type::socket >;

class invocation_foolproof_guard {
    functional::void_callback       destroy_;
    bool                            responded_ = false;
public:
    invocation_foolproof_guard(functional::void_callback on_destroy)
        : destroy_{on_destroy}
    {}
    ~invocation_foolproof_guard()
    {
        if (!responded_ && destroy_) {
            destroy_();
        }
    }
    void
    responded()
    {
        responded_ = true;
    }
};

connection_impl_ptr
connection_impl_base::create_connection(adapter_ptr adptr, transport_type _type,
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
            throw errors::runtime_error{ "Adapter gone away" };
        }
        return ::std::make_shared< Session >( server_side{}, a, on_close );
    }, on_close);
}

connection_impl_ptr
connection_impl_base::create_listen_connection(adapter_ptr adptr, transport_type _type,
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
connection_impl_base::set_connection_timer()
{
    if (can_drop_connection()) {
        // FIXME Configurable timeout
        if (connection_timer_.expires_from_now(::std::chrono::seconds{10}) > 0) {
            #ifdef DEBUG_OUTPUT
            ::std::cerr << "Reset timer\n";
            #endif
            connection_timer_.async_wait(::std::bind(&connection_impl_base::on_connection_timeout,
                    shared_from_this(), ::std::placeholders::_1));
        } else {
            #ifdef DEBUG_OUTPUT
            ::std::cerr << "Set timer\n";
            #endif
            connection_timer_.async_wait(::std::bind(&connection_impl_base::on_connection_timeout,
                    shared_from_this(), ::std::placeholders::_1));
        }
    }
}

void
connection_impl_base::on_connection_timeout(asio_config::error_code const& ec)
{
    if (!ec) {
        #ifdef DEBUG_OUTPUT
        ::std::cerr << "Timer expired\n";
        #endif
        if (can_drop_connection())
            process_event(events::close{});
    }
}

bool
connection_impl_base::can_drop_connection() const
{
    return false; // Turn off connection timeout
    // TODO Check if peer endpoint has a routable bidir adapter
    //return pending_replies_.empty() && outstanding_responses_ <= 0;
}

void
connection_impl_base::start_request_timer()
{
    request_timer_.expires_from_now(::std::chrono::milliseconds{500});
    request_timer_.async_wait(::std::bind(&connection_impl_base::on_request_timeout,
                    shared_from_this(), ::std::placeholders::_1));
}

void
connection_impl_base::on_request_timeout(asio_config::error_code const& ec)
{
    {
        lock_guard lock{reply_mutex_};
        auto now = clock_type::now();
        ::std::vector<pending_replies_type::const_iterator> to_erase;
        for (auto r = pending_replies_.begin(); r != pending_replies_.end(); ++r) {
            if (r->second.expires < now) {
                #ifdef DEBUG_OUTPUT
                ::std::cerr << "Request timed out\n";
                #endif
                if (r->second.error) {
                    auto err_handler = r->second.error;
                    io_service_->post([err_handler](){
                        try {
                            err_handler(::std::make_exception_ptr(
                                errors::request_timed_out{ "Request timed out" }
                            ));
                        } catch(...) {}
                    });
                }
                to_erase.push_back(r);
            }
        }
        for (auto r : to_erase) {
            pending_replies_.erase(r);
        }
    }
    start_request_timer();
}

void
connection_impl_base::connect_async(endpoint const& ep,
        functional::void_callback cb, functional::exception_callback eb)
{
    mode_ = client;
    process_event(events::connect{ ep, cb, eb });
}

void
connection_impl_base::start_session()
{
    mode_ = server;
    process_event(events::start{});
}

void
connection_impl_base::listen(endpoint const& ep, bool reuse_port)
{
    mode_ = server;
    do_listen(ep, reuse_port);
}

void
connection_impl_base::handle_connected(asio_config::error_code const& ec)
{
    #ifdef DEBUG_OUTPUT
    ::std::cerr << "Handle connected\n";
    #endif
    if (!ec) {
        process_event(events::connected{});
        auto adp = adapter_.lock();
        if (adp) {
            adp->connection_online(local_endpoint(), remote_endpoint());
        }
        start_request_timer();
    } else {
        process_event(events::connection_failure{
            ::std::make_exception_ptr(errors::connection_failed(ec.message()))
        });
    }
}

void
connection_impl_base::send_validate_message()
{
    encoding::outgoing_ptr out =
        ::std::make_shared<encoding::outgoing>(
                get_connector(),
                encoding::message::validate_flags);
    write_async(out);
}


void
connection_impl_base::close()
{
    process_event(events::close{});
}

void
connection_impl_base::send_close_message()
{
    encoding::outgoing_ptr out =
            ::std::make_shared<encoding::outgoing>(
                    get_connector(),
                    encoding::message::close);
    write_async(out);
}

void
connection_impl_base::handle_close()
{
    #ifdef DEBUG_OUTPUT
    ::std::cerr << "Handle close\n";
    #endif
    connection_timer_.cancel();
    request_timer_.cancel();
    {
        lock_guard lock{reply_mutex_};
    if (!pending_replies_.empty()) {
        auto ex = ::std::make_exception_ptr(errors::connection_failed{ "Conection closed" });
        for (auto const& req : pending_replies_) {
            try {
                req.second.error(ex);
            } catch (...) {}
        }
        pending_replies_.clear();
    }
    }

    if (on_close_)
        on_close_();
}

void
connection_impl_base::write_async(encoding::outgoing_ptr out,
        functional::void_callback cb)
{
    #ifdef DEBUG_OUTPUT
    ::std::cerr << "Send message " << out->type() << " size " << out->size() << "\n";
    #endif
    do_write_async( out,
        ::std::bind(&connection_impl_base::handle_write, shared_from_this(),
                ::std::placeholders::_1, ::std::placeholders::_2, cb, out));
}

void
connection_impl_base::handle_write(asio_config::error_code const& ec, ::std::size_t bytes,
        functional::void_callback cb, encoding::outgoing_ptr out)
{
    if (!ec) {
        if (cb) cb();
        set_connection_timer();
    } else {
        #ifdef DEBUG_OUTPUT
        ::std::cerr << "Write failed " << ec.message() << "\n";
        #endif
        process_event(events::connection_failure{
            ::std::make_exception_ptr(errors::connection_failed(ec.message()))
        });
    }
}

void
connection_impl_base::start_read()
{
    incoming_buffer_ptr buffer = ::std::make_shared< incoming_buffer >();
    read_async(buffer);
}

void
connection_impl_base::read_async(incoming_buffer_ptr buffer)
{
    do_read_async(buffer,
        ::std::bind(&connection_impl_base::handle_read, shared_from_this(),
                ::std::placeholders::_1, ::std::placeholders::_2, buffer));
}

void
connection_impl_base::handle_read(asio_config::error_code const& ec, ::std::size_t bytes,
        incoming_buffer_ptr buffer)
{
    if (!ec) {
        read_incoming_message(buffer, bytes);
        start_read();
        set_connection_timer();
    } else {
        #ifdef DEBUG_OUTPUT
        ::std::cerr << "Read failed " << ec.message() << "\n";
        #endif
        process_event(events::connection_failure{
            ::std::make_exception_ptr(errors::connection_failed(ec.message()))
        });
    }
}

void
connection_impl_base::read_incoming_message(incoming_buffer_ptr buffer, ::std::size_t bytes)
{
    using encoding::message;
    auto b = buffer->begin();
    auto e = b + bytes;
    try {
        while (b != e) {
            if (incoming_) {
                incoming_->insert_back(b, e);
                if (incoming_->complete()) {
                    dispatch_incoming(incoming_);
                    incoming_.reset();
                }
            } else {
                message m;
                read(b, e, m);
                bytes -= b - buffer->begin();

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
                        #ifdef DEBUG_OUTPUT
                        ::std::cerr << "Receive message " << m.type()
                                << " size " << m.size << "\n";
                        #endif

                        encoding::incoming_ptr incoming =
                            ::std::make_shared< encoding::incoming >( get_connector(), m, b, e );
                        if (!incoming->complete()) {
                            incoming_ = incoming;
                        } else {
                            dispatch_incoming(incoming);
                        }
                    }
                }
            }
        }
    } catch (::std::exception const& e) {
        /** TODO Make it a protocol error? Can we handle it? */
        #ifdef DEBUG_OUTPUT
        ::std::cerr << "Protocol read exception: " << e.what() << "\n";
        #endif
        process_event(events::connection_failure{
            ::std::current_exception()
        });
    }
}

void
connection_impl_base::dispatch_incoming(encoding::incoming_ptr incoming)
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
            process_event(events::connection_failure{
                ::std::make_exception_ptr(errors::unmarshal_error{ "Unknown message type ", incoming->type() })
            });
    }
}

void
connection_impl_base::invoke(identity const& id, ::std::string const& op, context_type const& ctx,
        bool run_sync,
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
        encoding::operation_specs{ id, "", op },
        request::normal
    };
    write(::std::back_inserter(*out), r);
    params.close_all_encaps();
    out->insert_encapsulation(::std::move(params));
    functional::void_callback write_cb = sent ? [sent](){sent(true);} : functional::void_callback{};
    // TODO Pass timeout in invokation parameters
    time_point expires = clock_type::now() + expire_duration{5000};
    {
        lock_guard lock{reply_mutex_};
        pending_replies_.insert(::std::make_pair( r.number, pending_reply{ reply, exception, expires } ));
    }
    process_event(events::send_request{ out, write_cb });

    if (run_sync) {
        wait_until([&](){
            return pending_replies_.count(r.number);
        });
    }
}

void
connection_impl_base::dispatch_reply(encoding::incoming_ptr buffer)
{
    using namespace encoding;
    try {
        #ifdef DEBUG_OUTPUT
        ::std::cerr << "Dispatch reply\n";
        #endif
        reply rep;
        incoming::const_iterator b = buffer->begin();
        incoming::const_iterator e = buffer->end();
        read(b, e, rep);
        pending_replies_type::iterator f = pending_replies_.end();
        {
            lock_guard lock{reply_mutex_};
            f = pending_replies_.find(rep.number);
        }
        if (f != pending_replies_.end()) {
            switch (rep.status) {
                case reply::success:{
                    #ifdef DEBUG_OUTPUT
                    ::std::cerr << "Reply status is success\n";
                    #endif
                    if (f->second.reply) {
                        incoming::encaps_guard encaps{buffer->begin_encapsulation(b)};

                        #ifdef DEBUG_OUTPUT
                        version const& ever = encaps.encaps().encoding_version();
                        ::std::cerr << "Reply encaps v" << ever.major << "." << ever.minor
                                << " size " << encaps.size() << "\n";
                        #endif
                        try {
                            f->second.reply(encaps->begin(), encaps->end());
                        } catch (...) {
                            // Ignore handler error
                        }
                    }
                    break;
                }
                case reply::success_no_body: {
                    #ifdef DEBUG_OUTPUT
                    ::std::cerr << "Reply status is success without body\n";
                    #endif
                    if (f->second.reply) {
                        try {
                            f->second.reply( incoming::const_iterator{}, incoming::const_iterator{} );
                        } catch (...) {
                            // Ignore handler error
                        }
                    }
                    break;
                }
                case reply::no_object:
                case reply::no_facet:
                case reply::no_operation: {
                    #ifdef DEBUG_OUTPUT
                    ::std::cerr << "Reply status is not found\n";
                    #endif
                    if (f->second.error) {
                        incoming::encaps_guard encaps{buffer->begin_encapsulation(b)};

                        #ifdef DEBUG_OUTPUT
                        version const& ever = encaps.encaps().encoding_version();
                        ::std::cerr << "Reply encaps v" << ever.major << "." << ever.minor
                                << " size " << encaps.size() << "\n";
                        #endif
                        encoding::operation_specs op;
                        auto b = encaps->begin();
                        read(b, encaps->end(), op);
                        encaps->read_indirection_table(b);
                        try {
                            f->second.error(
                                ::std::make_exception_ptr(
                                    errors::not_found{
                                        static_cast< errors::not_found::subject >(
                                                rep.status - reply::no_object ),
                                        op.identity,
                                        op.facet,
                                        op.name()
                                    } )
                            );
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
                    #ifdef DEBUG_OUTPUT
                    ::std::cerr << "Reply status is an exception\n";
                    #endif
                    if (f->second.error) {
                        incoming::encaps_guard encaps{buffer->begin_encapsulation(b)};

                        #ifdef DEBUG_OUTPUT
                        version const& ever = encaps.encaps().encoding_version();
                        ::std::cerr << "Reply encaps v" << ever.major << "." << ever.minor
                                << " size " << encaps.size() << "\n";
                        #endif
                        errors::user_exception_ptr exc;
                        auto b = encaps->begin();
                        read(b, encaps->end(), exc);
                        encaps->read_indirection_table(b);
                        try {
                            f->second.error(exc->make_exception_ptr());
                        } catch (...) {
                            // Ignore handler error
                        }
                    }
                    break;
                }
                default:
                    if (f->second.error) {
                        try {
                            f->second.error(::std::make_exception_ptr(
                                    errors::unmarshal_error{ "Unhandled reply status" } ));
                        } catch (...) {
                            // Ignore handler error
                        }
                    }
                    break;
            }
            {
                lock_guard lock{reply_mutex_};
            pending_replies_.erase(f);
            }
            #ifdef DEBUG_OUTPUT
            ::std::cerr << "Pending replies: " << pending_replies_.size() << "\n";
            #endif
        } else {
            // else discard the reply (it can be timed out)
            #ifdef DEBUG_OUTPUT
            ::std::cerr << "No waiting callback for reply\n";
            #endif
        }
    } catch (::std::exception const& e) {
        #ifdef DEBUG_OUTPUT
        ::std::cerr << "Exception when reading reply: " << e.what() << "\n";
        #endif
        process_event(events::connection_failure{ ::std::current_exception() });
    } catch (...) {
        #ifdef DEBUG_OUTPUT
        ::std::cerr << "Exception when reading reply\n";
        #endif
        process_event(events::connection_failure{ ::std::current_exception() });
    }
}

void
connection_impl_base::send_not_found(
        uint32_t req_num, errors::not_found::subject subj,
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
    write_async(out);
}

void
connection_impl_base::send_exception(uint32_t req_num, errors::user_exception const& e)
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
    write_async(out);
}

void
connection_impl_base::send_exception(uint32_t req_num, ::std::exception const& e)
{
    using namespace encoding;
    outgoing_ptr out =
            ::std::make_shared<outgoing>(get_connector(), message::reply);
    reply rep { req_num, reply::unknown_user_exception };
    auto o = ::std::back_inserter(*out);
    write(o, rep);

    {
        outgoing::encaps_guard guard{ out->begin_encapsulation() };
        errors::unexpected ue { typeid(e).name(), e.what() };
        ue.__wire_write(o);
    }
    write_async(out);
}

void
connection_impl_base::send_unknown_exception(uint32_t req_num)
{
    using namespace encoding;
    outgoing_ptr out =
            ::std::make_shared<outgoing>(get_connector(), message::reply);
    reply rep { req_num, reply::unknown_exception };
    auto o = ::std::back_inserter(*out);
    write(o, rep);

    {
        outgoing::encaps_guard guard{ out->begin_encapsulation() };
        errors::unexpected ue {};
        ue.__wire_write(o);
    }
    write_async(out);
}

void
connection_impl_base::dispatch_incoming_request(encoding::incoming_ptr buffer)
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
            // TODO Refactor upcall invocation to the adapter
            // TODO Use facet
            #ifdef DEBUG_OUTPUT
            ::std::cerr << "Dispatch request " << req.operation.name()
                    << " to " << req.operation.identity << "\n";
            #endif

            auto disp = adp->find_object(req.operation.identity);
            if (disp) {
                // TODO Read context
                incoming::encaps_guard encaps{ buffer->begin_encapsulation(b) };
                auto const& en = encaps.encaps();
                auto _this = shared_from_this();
                auto fpg = ::std::make_shared< invocation_foolproof_guard >(
                    [_this, req]() mutable {
                        #ifdef DEBUG_OUTPUT
                        ::std::cerr << "Invocation to " << req.operation.identity
                                << " operation " << req.operation.operation
                                << " failed to respond";
                        #endif
                        _this->send_not_found(req.number, errors::not_found::object,
                                req.operation);
                    });
                detail::dispatch_request r{
                    buffer, en.begin(), en.end(), en.size(),
                    [_this, req, fpg](outgoing&& res) mutable {
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
                        _this->write_async(out);
                        fpg->responded();
                    },
                    [_this, req, fpg](::std::exception_ptr ex) mutable {
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
                        fpg->responded();
                    }
                };
                current curr {
                    req.operation,
                    /* FIXME add context */
                    /* FIXME add endpoint */
                };
                disp->__dispatch(r, curr);
                return;
            } else {
                #ifdef DEBUG_OUTPUT
                ::std::cerr << "No object\n";
                #endif
            }
        } else {
            #ifdef DEBUG_OUTPUT
            ::std::cerr << "No adapter\n";
            #endif
        }
        send_not_found(req.number, errors::not_found::object, req.operation);
    } catch (...) {
        process_event(events::connection_failure{ ::std::current_exception() });
    }
}

}  // namespace detail

struct connection::impl {
    connection const*           owner_;
    adapter_weak_ptr            adapter_;
    connector_weak_ptr          connector_;
    asio_config::io_service_ptr io_service_;
    detail::connection_impl_ptr connection_;

    impl(connection const* owner, adapter_ptr adp)
        : owner_{owner},
          adapter_{adp},
          connector_{adp->get_connector()},
          io_service_{adp->io_service()}
    {
    }
    void
    connect_async(endpoint const& ep,
            functional::void_callback       on_connect,
            functional::exception_callback  on_error,
            close_callback                  on_close)
    {
        if (connection_) {
            // Do something with the old connection
            // Or throw exception
        }
        connection const* owner = owner_;
        connection_ = detail::connection_impl_base::create_connection(
            adapter_.lock(), ep.transport(),
            [owner, on_close](){
                #ifdef DEBUG_OUTPUT
                ::std::cerr << "Client connection on close\n";
                #endif
                if (on_close)
                    on_close(owner);
            });
        connection_->connect_async(ep, on_connect, on_error);
    }

    void
    start_accept(endpoint const& ep)
    {
        if (connection_) {
            // Do something with the old connection
            // Or throw exception
        }
        connection_ = detail::connection_impl_base::create_listen_connection(
            adapter_.lock(), ep.transport(),
            [](){
                #ifdef DEBUG_OUTPUT
                ::std::cerr << "Server connection on close\n";
                #endif
            });
        connection_->listen(ep);
    }

    void
    close()
    {
        if (!connection_)
            throw errors::runtime_error{ "Connection is not initialized" };
        connection_->close();
    }

    void
    invoke(identity const& id, ::std::string const& op, context_type const& ctx,
            bool run_sync,
            encoding::outgoing&& params,
            encoding::reply_callback reply,
            functional::exception_callback exception,
            functional::callback< bool > sent)
    {
        if (!connection_)
            throw errors::runtime_error{ "Connection is not initialized" };
        connection_->invoke(id, op, ctx, run_sync,
                ::std::move(params), reply, exception, sent);
    }

    void
    set_adapter(adapter_ptr adp)
    {
        adapter_ = adp;
        if (connection_) {
            connection_->adapter_ = adp;
        }
    }

    endpoint
    local_endpoint()
    {
        if (!connection_)
            throw errors::runtime_error{ "Connection is not initialized" };
        return connection_->local_endpoint();
    }
};

connection::connection(client_side const&, adapter_ptr adp)
    : pimpl_{::std::make_shared<impl>(this, adp)}
{
}

connection::connection(client_side const&, adapter_ptr adp, endpoint const& ep,
        functional::void_callback on_connect,
        functional::exception_callback on_error,
        close_callback on_close)
    : pimpl_{::std::make_shared<impl>(this, adp)}
{
    pimpl_->connect_async(ep, on_connect, on_error, on_close);
}

connection::connection(server_side const&, adapter_ptr adp, endpoint const& ep)
    : pimpl_{::std::make_shared<impl>(this, adp)}
{
    pimpl_->start_accept(ep);
}

connection::connection(connection&& rhs)
    : pimpl_(rhs.pimpl_)
{
    rhs.pimpl_.reset();
}

connection::~connection()
{
    #ifdef DEBUG_OUTPUT
    ::std::cerr << "Destroy connection façade\n";
    #endif
}

connection&
connection::operator =(connection&& rhs)
{
    // TODO Close existing connection if any
    pimpl_ = rhs.pimpl_;
    rhs.pimpl_.reset();
    return *this;
}

connector_ptr
connection::get_connector() const
{
    return pimpl_->connector_.lock();
}

void
connection::connect_async(endpoint const& ep,
        functional::void_callback       on_connect,
        functional::exception_callback  on_error,
        close_callback                  on_close)
{
    pimpl_->connect_async(ep, on_connect, on_error, on_close);
}

void
connection::set_adapter(adapter_ptr adp)
{
    pimpl_->set_adapter(adp);
}

void
connection::close()
{
    pimpl_->close();
}

void
connection::invoke(identity const& id, ::std::string const& op,
        context_type const& ctx,
        bool run_sync,
        encoding::outgoing&& params,
        encoding::reply_callback reply,
        functional::exception_callback exception,
        functional::callback< bool > sent)
{
    pimpl_->invoke(id, op, ctx, run_sync, ::std::move(params), reply, exception, sent);
}

endpoint
connection::local_endpoint() const
{
    return pimpl_->local_endpoint();
}

}  // namespace core
}  // namespace wire
