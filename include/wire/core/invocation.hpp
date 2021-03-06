/*
 * invokation.hpp
 *
 *  Created on: 11 мая 2016 г.
 *      Author: sergey.fedorov
 */

#ifndef WIRE_CORE_INVOCATION_HPP_
#define WIRE_CORE_INVOCATION_HPP_

#include <wire/core/connection.hpp>
#include <wire/core/reference.hpp>
#include <wire/core/detail/dispatch_request.hpp>
#include <wire/core/current.hpp>
#include <wire/core/object.hpp>
#include <wire/encoding/message.hpp>

#include <wire/errors/not_found.hpp>

#include <pushkin/meta/function_traits.hpp>

namespace wire {
namespace core {

namespace detail {

template < typename ... Args >
struct invokation_args_type {
    enum { arity = sizeof ... (Args) };
    using args_tuple_type   = ::std::tuple< Args ... >;

    template < ::std::size_t N >
    struct arg {
        using type          = typename ::std::tuple_element< N, args_tuple_type >::type;
    };

    using last_arg          = typename arg< arity - 1 >::type;
    using before_last_arg   = typename arg< arity - 2 >::type;
};

template < bool enoughArgs, typename ... Args >
struct is_sync_dispatch_impl : ::std::true_type {};

template < typename ... Args >
struct is_sync_dispatch_impl<true, Args ...>
    : ::std::conditional<
            ::std::is_same<
                 typename invokation_args_type< Args... >::before_last_arg,
                 functional::exception_callback >::value &&
            ::std::is_same<
                 typename invokation_args_type< Args... >::last_arg,
                 current const& >::value,
            ::std::false_type,
            ::std::true_type
         >::type {};


template < typename ... T >
struct is_sync_dispatch;

template < typename Interface, typename Return, typename ... Args >
struct is_sync_dispatch< Return(Interface::*)(Args ...) >
    : is_sync_dispatch_impl< sizeof ... (Args) >= 3, Args ... > {};

template < typename Interface, typename Return, typename ... Args >
struct is_sync_dispatch< Return(Interface::*)(Args ...) const>
    : is_sync_dispatch_impl< sizeof ... (Args) >= 3, Args ... > {};

enum class invokation_type {
    void_sync,
    nonvoid_sync,
    void_async,
    nonvoid_async
};

template < bool isVoid, bool isSync >
struct invokation_selector;

template < invokation_type T >
using invocation_mode = ::std::integral_constant< invokation_type, T >;

template <>
struct invokation_selector<true, true> :
    invocation_mode< invokation_type::void_sync >{};
template <>
struct invokation_selector<false, true> :
    invocation_mode< invokation_type::nonvoid_sync >{};
template <>
struct invokation_selector<true, false> :
    invocation_mode< invokation_type::void_async >{};
template <>
struct invokation_selector<false, false> :
    invocation_mode< invokation_type::nonvoid_async >{};

template < typename Handler, typename Member,
        typename IndexTuple, typename ... Args >
struct local_invocation;

template < typename Handler, typename Member,
            size_t ... Indexes, typename ... Args >
struct local_invocation< Handler, Member,
            ::psst::meta::indexes_tuple< Indexes ... >, Args ... > {
    using member_type       = Member;
    using member_traits     = ::psst::meta::function_traits< member_type >;
    using interface_type    = typename member_traits::class_type;
    using servant_ptr       = ::std::shared_ptr< interface_type >;

    using response_hander   = Handler;
    using response_traits   = ::psst::meta::function_traits<response_hander>;
    using response_args     = typename response_traits::decayed_args_tuple_type;

    static constexpr bool is_void       = ::psst::meta::is_func_void< member_type >::value;
    static constexpr bool is_sync       = is_sync_dispatch< member_type >::value;
    static constexpr bool void_response = response_traits::arity == 0;

    using invocation_args = ::std::tuple< typename ::std::decay< Args >::type ... >;
    using exception_handler = functional::exception_callback;
    using sent_handler      = functional::callback< bool >;

    reference_const_ptr                             ref;
    member_type                                     member;

    encoding::operation_specs::operation_id const&  op;
    context_type const&                             ctx;
    invocation_args                                 args;

    response_hander                                 response;
    exception_handler                               exception;
    sent_handler                                    sent;

    void
    operator()(invocation_options const& opts) const
    {
        invocation_sent();
        auto srvnt = ref->get_local_object();
        object_ptr obj = srvnt.first;
        if (!obj) {
            functional::report_exception(exception,
                    errors::no_object{
                        ref->object_id(),
                        ref->facet(),
                        op });
            return;
        }
        servant_ptr srv = ::std::dynamic_pointer_cast< interface_type >(obj);
        auto ctx_ptr = ::std::make_shared<context_type>(ctx);
        current curr{{{ref->object_id(), ref->facet()}, op},
            ctx_ptr,
            endpoint{},
            srvnt.second};
        if (!srv) {
            dispatch(obj, curr, opts);
        } else {
            invoke(srv, curr, opts, invokation_selector< is_void && void_response, is_sync >{});
        }
    }

    void
    invoke( servant_ptr srv, current const& curr, invocation_options const&,
            invocation_mode< invokation_type::void_sync > const& ) const
    {
        try {
            invocation_args tmp = args;
            ((*srv).*member)(::std::move(::std::get< Indexes >(tmp)) ..., curr);
            response();
        } catch (...) {
            functional::report_exception(exception, ::std::current_exception());
        }
    }

    void
    invoke( servant_ptr srv, current const& curr, invocation_options const&,
            invocation_mode< invokation_type::nonvoid_sync > const& ) const
    {
        try {
            invocation_args tmp = args;
            response_args res = ((*srv).*member)(::std::move(::std::get< Indexes >(tmp)) ..., curr);
            response(::std::move(res));
        } catch (...) {
            functional::report_exception(exception, ::std::current_exception());
        }
    }

    void
    invoke( servant_ptr srv, current const& curr, invocation_options const&,
            invocation_mode< invokation_type::void_async > const&) const
    {
        try {
            invocation_args tmp = args;
            ((*srv).*member)(::std::move(::std::get< Indexes >(tmp)) ...,
                    response, exception, curr);
        } catch (...) {
            functional::report_exception(exception, ::std::current_exception());
        }
    }

    void
    invoke( servant_ptr srv, current const& curr, invocation_options const&,
            invocation_mode< invokation_type::nonvoid_async > const&) const
    {
        try {
            invocation_args tmp = args;
            auto resp = response;
            ((*srv).*member)(::std::move(::std::get< Indexes >(tmp)) ...,
                    [resp](response_args const& res)
                    {
                        auto tmp = res;
                        resp(::std::move(tmp));
                    }, exception, curr);
        } catch (...) {
            functional::report_exception(exception, ::std::current_exception());
        }
    }

    void
    invocation_sent() const
    {
        if (sent) {
            try {
                sent(true);
            } catch (...) {}
        }
    }

    void
    write_args(encoding::outgoing& out, ::std::true_type const&) const // write sync invocation args
    {
        encoding::write(::std::back_inserter(out),
                ::std::get< Indexes >(args) ...);
    }
    void
    write_args(encoding::outgoing& out, ::std::false_type const&) const // write async invocation args
    {
        encoding::write(::std::back_inserter(out),
                ::std::get< Indexes >(args) ...);
    }
    encoding::request_result_callback
    create_callback( ::std::true_type const& ) const // callback for void dispatch
    {
        response_hander resp    = response;
        exception_handler exc   = exception;
        return [resp, exc]( encoding::outgoing&& out )
        {
            try {
                if (resp) resp();
            } catch (...) {
                functional::report_exception(exc, ::std::current_exception());
            }
        };
    }
    encoding::request_result_callback
    create_callback( ::std::false_type const& ) const // callback for non-void dispatch
    {
        using namespace encoding;
        response_hander resp = response;
        exception_handler exc   = exception;
        return [resp, exc]( outgoing&& out )
        {
            try {
                // Unmarshal params
                incoming in{ message{}, ::std::move(out) };
                auto encaps = in.current_encapsulation();
                response_args args;
                auto begin = encaps.begin();
                auto end = encaps.end();
                read(begin, end, args);
                encaps.read_indirection_table(begin);
                ::psst::meta::invoke(resp, ::std::move(args));
            } catch (...) {
                functional::report_exception(exc, ::std::current_exception());
            }
        };
    }
    void
    dispatch(object_ptr obj, current const& curr,  invocation_options const&) const
    {
        using namespace encoding;
        outgoing out{ ref->get_connector() };
        write_args(out, ::std::integral_constant<bool, is_sync>{});
        out.close_all_encaps();
        incoming_ptr in = ::std::make_shared< incoming >(message{}, ::std::move(out));
        auto encaps = in->current_encapsulation();

        dispatch_request dr {
            in, encaps.begin(), encaps.end(), encaps.size(),
                    create_callback(::std::integral_constant<bool, void_response>{}),
                    exception
        };
        obj->__dispatch(dr, curr);
    }
};

template < typename Handler, typename IndexTuple, typename ... Args >
struct remote_invocation;

template < typename Handler, size_t ... Indexes, typename ... Args >
struct remote_invocation< Handler,
        ::psst::meta::indexes_tuple< Indexes ... >, Args ... > {
    using invocation_args =
            ::std::tuple<
                  ::std::reference_wrapper<
                        typename ::std::decay< Args >::type const > ... >;
    using response_hanlder  = Handler;
    using response_traits   = ::psst::meta::function_traits<response_hanlder>;
    using is_void           = ::std::integral_constant< bool, (response_traits::arity == 0) >;
    using exception_handler = functional::exception_callback;
    using sent_handler      = functional::callback< bool >;

    struct invocation_data {
        encoding::invocation_target             target;
        encoding::operation_specs::operation_id op;
        context_type                            ctx;
        encoding::outgoing                      out;

        response_hanlder                        response;
        exception_handler                       exception;
        sent_handler                            sent;
    };

    remote_invocation( reference_const_ptr r,
            encoding::operation_specs::operation_id const& o,
            context_type const& c, invocation_args&& args,
            response_hanlder resp, exception_handler exc, sent_handler snt)
        : ref(r),
          data{ new invocation_data{ {ref->object_id(), ref->facet()}, o, c,
              encoding::outgoing{ ref->get_connector() }, resp, exc, snt } }
    {
        encoding::write(::std::back_inserter(data->out),
                ::std::get< Indexes >(args).get() ...);
    }

    /**
     * Make a connection response callback for a non-void function invocation
     * @param response
     * @param exception
     * @param
     * @return
     */
    template <typename ReplyHandler>
    typename ::std::enable_if< (::psst::meta::function_traits< ReplyHandler >::arity > 0),
        encoding::reply_callback >::type
    make_callback(ReplyHandler response, exception_handler exception,
            invocation_options const& opts, ::std::false_type const&) const
    {
        using encoding::incoming;
        using reply_traits = ::psst::meta::function_traits<ReplyHandler>;
        using reply_tuple  = typename reply_traits::decayed_args_tuple_type;
        if (response) {
            if (opts.is_one_way()) {
                ::std::ostringstream os;
                os << "Cannot invoke a non-void function "
                    << data->op << " on a one-way proxy";
                throw errors::invalid_one_way_invocation{os.str()};
            }
            return [response, exception](incoming::const_iterator begin, incoming::const_iterator end) {
                try {
                    auto encaps = begin.incoming_encapsulation();
                    reply_tuple args;
                    encoding::read(begin, end, args);
                    encaps.read_indirection_table(begin);
                    ::psst::meta::invoke(response, ::std::move(args));
                } catch(...) {
                    functional::report_exception(exception, ::std::current_exception());
                }
            };
        }
        return [](incoming::const_iterator, incoming::const_iterator) {};
    }

    /**
     * Make a connection response callback for a void function invocation
     * @param response
     * @param exception
     * @param
     * @return
     */
    encoding::reply_callback
    make_callback(response_hanlder response, exception_handler exception,
            invocation_options const& opts, ::std::true_type const&) const
    {
        using encoding::incoming;
        if (response && !opts.is_one_way())
            return [response, exception](incoming::const_iterator, incoming::const_iterator) {
                try {
                    response();
                } catch(...) {
                    // Don't report exception here, as the exception is the
                    // pair for the response and it will lead to setting shared
                    // state of a promise
                    //functional::report_exception(exception, ::std::current_exception());
                }
            };
        return [](incoming::const_iterator, incoming::const_iterator) {};
    }

    void
    operator()(invocation_options const& opts) const
    {
        auto reply = make_callback(data->response, data->exception, opts, is_void{});
        if (opts.is_sync()) {
            try {
                ref->get_connection(opts)->invoke(data->target, data->op, data->ctx, opts,
                        ::std::move(data->out), reply, data->exception, data->sent);
            } catch (...) {
                functional::report_exception(data->exception, ::std::current_exception());
            }
        } else {
            auto d = data;
            ref->get_connection_async(
            [d, reply, opts](connection_ptr conn) {
                conn->invoke(d->target, d->op, d->ctx, opts,
                    ::std::move(d->out), reply, d->exception, d->sent);
            },
            [d](::std::exception_ptr ex) {
                functional::report_exception(d->exception, ex);
            }, opts);
        }
    }

    reference_const_ptr                     ref;
    ::std::shared_ptr< invocation_data >    data;
};

}  /* namespace detail */

template < typename Handler, typename Member, typename ... Args>
functional::invocation_function
make_invocation(reference_const_ptr                     ref,
        encoding::operation_specs::operation_id const&  op,
        context_type const&                             ctx,
        Member                                          member,
        Handler                                         response,
        functional::exception_callback                  exception,
        functional::callback< bool >                    sent,
        Args const& ...                                 args)
{
    using index_type        = typename ::psst::meta::index_builder< sizeof ... (Args) >::type;
    using remote_invocation = detail::remote_invocation< Handler, index_type, Args ... >;
    using remote_args       = typename remote_invocation::invocation_args;
    using local_invocation  = detail::local_invocation< Handler, Member, index_type, Args ... >;
    using local_args        = typename local_invocation::invocation_args;

    if (!ref)
        throw errors::runtime_error{"Empty reference"};

    if (ref->is_local()) {
        return local_invocation {
            ref, member, op, ctx,
            local_args{ args ... },
            response, exception, sent
        };
    } else {
        return remote_invocation {
            ref, op, ctx,
            remote_args{ args ... },
            response, exception, sent
        };
    }
}

}  /* namespace core */
}  /* namespace wire */



#endif /* WIRE_CORE_INVOCATION_HPP_ */
