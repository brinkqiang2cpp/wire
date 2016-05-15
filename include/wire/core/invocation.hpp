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

#include <wire/errors/not_found.hpp>

#include <wire/util/function_traits.hpp>

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
    void_async
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

template < typename Handler, typename Member,
        typename IndexTuple, typename ... Args >
struct local_invocation;

template < typename Handler, typename Member,
            size_t ... Indexes, typename ... Args >
struct local_invocation< Handler, Member,
            util::indexes_tuple< Indexes ... >, Args ... > {
    using member_type       = Member;
    using member_traits     = util::function_traits< member_type >;
    using interface_type    = typename member_traits::class_type;
    using servant_ptr       = ::std::shared_ptr< interface_type >;

    using response_hander   = Handler;
    using response_traits   = util::function_traits<response_hander>;
    using response_args     = typename response_traits::decayed_args_tuple_type;

    static constexpr bool is_void       = util::is_func_void< member_type >::value;
    static constexpr bool is_sync       = is_sync_dispatch< member_type >::value;
    static constexpr bool void_response = response_traits::arity == 0;

    using invocation_args = typename ::std::conditional<is_sync,
            ::std::tuple<
                  ::std::reference_wrapper<
                       typename ::std::decay< Args >::type const > ... >,
            ::std::tuple< typename ::std::decay< Args >::type ... > // Copy args for the async local invocation
        >::type;
    using exception_handler = functional::exception_callback;
    using sent_handler      = functional::callback< bool >;

    reference const&        ref;
    member_type             member;

    ::std::string const&    op;
    context_type const&     ctx;
    invocation_args         args;

    response_hander         response;
    exception_handler       exception;
    sent_handler            sent;

    void
    operator()(bool run_sync) const
    {
        invocation_sent();
        object_ptr obj = ref.get_local_object();
        if (!obj) {
            invokation_error(
                ::std::make_exception_ptr(errors::no_object{
                    ref.object_id(),
                    ref.facet(),
                    op
            }));
            return;
        }
        servant_ptr srv = ::std::dynamic_pointer_cast< interface_type >(obj);

        current curr{{ref.object_id(), ref.facet(), op}, ctx};
        if (!srv) {
            dispatch(obj, curr, run_sync);
        } else {
            invoke(srv, curr, run_sync, invokation_selector< is_void, is_sync >{});
        }
    }

    void
    invoke( servant_ptr srv, current const& curr, bool run_sync,
            invocation_mode< invokation_type::void_sync > const& ) const
    {
        try {
            ((*srv).*member)(::std::get< Indexes >(args).get() ..., curr);
            response();
        } catch (...) {
            invokation_error(::std::current_exception());
        }
    }

    void
    invoke( servant_ptr srv, current const& curr, bool run_sync,
            invocation_mode< invokation_type::nonvoid_sync > const& ) const
    {
        try {
            response(((*srv).*member)(::std::get< Indexes >(args).get() ..., curr));
        } catch (...) {
            invokation_error(::std::current_exception());
        }
    }

    void
    invoke( servant_ptr srv, current const& curr, bool run_sync,
            invocation_mode< invokation_type::void_async > const&) const
    {
        try {
            ((*srv).*member)(::std::get< Indexes >(args) ...,
                    response, exception, curr);
        } catch (...) {
            invokation_error(::std::current_exception());
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
    invokation_error(::std::exception_ptr ex) const
    {
        if (exception) {
            try {
                exception(ex);
            } catch (...) {}
        }
    }

    void
    write_args(encoding::outgoing& out, ::std::true_type const&) const // write sync invocation args
    {
        encoding::write(::std::back_inserter(out),
                ::std::get< Indexes >(args).get() ...);
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
                if (exc) {
                    try {
                        exc(::std::current_exception());
                    } catch(...) {}
                }
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
                util::invoke(resp, args);
            } catch (...) {
                if (exc) {
                    try {
                        exc(::std::current_exception());
                    } catch (...) {}
                }
            }
        };
    }
    void
    dispatch(object_ptr obj, current const& curr,  bool run_sync) const
    {
        using namespace encoding;
        outgoing out{ ref.get_connector() };
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
                util::indexes_tuple< Indexes ... >, Args ... > {
    using invocation_args =
            ::std::tuple<
                  ::std::reference_wrapper<
                        typename ::std::decay< Args >::type const > ... >;
    using response_hander   = Handler;
    using exception_handler = functional::exception_callback;
    using sent_handler      = functional::callback< bool >;

    reference const&        ref;

    ::std::string const&    op;
    context_type const&     ctx;
    invocation_args         args;

    response_hander         response;
    exception_handler       exception;
    sent_handler            sent;

    void
    operator()(bool run_sync) const
    {
        ref.get_connection()->invoke(ref.object_id(), op, ctx, run_sync,
                response, exception, sent, ::std::get< Indexes >(args).get() ...);
    }
};

}  /* namespace detail */

template < typename Handler, typename Member, typename ... Args>
functional::invocation_function
make_invocation(reference const&        ref,
        ::std::string const&            op,
        context_type const&             ctx,
        Member                          member,
        Handler                         response,
        functional::exception_callback  exception,
        functional::callback< bool >    sent,
        Args const& ...                 args)
{
    using index_type        = typename util::index_builder< sizeof ... (Args) >::type;
    using remote_invocation = detail::remote_invocation< Handler, index_type, Args ... >;
    using remote_args       = typename remote_invocation::invocation_args;
    using local_invocation  = detail::local_invocation< Handler, Member, index_type, Args ... >;
    using local_args        = typename local_invocation::invocation_args;

    if (ref.is_local()) {
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
