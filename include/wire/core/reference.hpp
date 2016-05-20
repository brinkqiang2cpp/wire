/*
 * reference.hpp
 *
 *  Created on: Apr 11, 2016
 *      Author: zmij
 */

#ifndef WIRE_CORE_REFERENCE_HPP_
#define WIRE_CORE_REFERENCE_HPP_

#include <wire/core/identity.hpp>
#include <wire/core/endpoint.hpp>

#include <wire/core/connection_fwd.hpp>
#include <wire/core/connector_fwd.hpp>
#include <wire/core/reference_fwd.hpp>
#include <wire/core/object_fwd.hpp>
#include <wire/core/functional.hpp>

#include <wire/encoding/detail/optional_io.hpp>

#include <future>

namespace wire {
namespace core {

struct reference_data {
    identity                object_id;
    ::std::string           facet;
    optional_identity       adapter;
    endpoint_list           endpoints;
};

template < typename OutputIterator >
void
wire_write(OutputIterator o, reference_data const& v)
{
    encoding::write(o, v.object_id, v.facet, v.adapter, v.endpoints);
}

template < typename InputIterator >
void
wire_read(InputIterator& begin, InputIterator end, reference_data& v)
{
    encoding::read(begin, end, v.object_id, v.facet, v.adapter, v.endpoints);
}

bool
operator == (reference_data const& lhs, reference_data const& rhs);
bool
operator != (reference_data const& lhs, reference_data const& rhs);
bool
operator < (reference_data const& lhs, reference_data const& rhs);

::std::ostream&
operator << (::std::ostream& os, reference_data const& val);
::std::istream&
operator >> (::std::istream& is, reference_data& val);

/**
 * Class for a reference.
 */
class reference {
public:
    using connection_callback = functional::callback< connection_ptr >;
public:
    reference(connector_ptr cn, reference_data const& ref)
        : connector_{cn}, ref_{ref} {}

    virtual ~reference() = default;

    static reference_ptr
    create_reference(connector_ptr, reference_data const&);

    reference_data const&
    data() const
    { return ref_; }

    bool
    is_local() const;
    object_ptr
    get_local_object() const;

    identity const&
    object_id() const
    { return ref_.object_id; }

    ::std::string const&
    facet() const
    { return ref_.facet; }

    connector_ptr
    get_connector() const
    { return connector_.lock(); }

    connection_ptr
    get_connection() const;

    template < template < typename  > class _Promise = ::std::promise >
    auto
    get_connection_async(bool sync = false) const
        -> decltype(::std::declval< _Promise< connection_ptr > >().get_future() )
    {
        auto promise = ::std::make_shared< _Promise< connection_ptr > >();
        get_connection_async(
            [promise](connection_ptr val)
            {
                promise->set_value(val);
            },
            [promise](::std::exception_ptr ex)
            {
                promise->set_exception(::std::move(ex));
            }, sync
        );

        return promise->get_future();
    }

    virtual void
    get_connection_async( connection_callback on_get,
            functional::exception_callback on_error,
            bool sync = false) const  = 0;
private:
    connector_weak_ptr  connector_;
protected:
    reference_data          ref_;
    object_weak_ptr mutable local_object_cache_;
};

using reference_ptr = ::std::shared_ptr<reference>;

/**
 * Fixed reference
 */
class fixed_reference : public reference {
public:
    fixed_reference(connector_ptr cn, reference_data const& ref);

    void
    get_connection_async( connection_callback on_get,
            functional::exception_callback on_error,
            bool sync = false) const override;
private:
    connection_weak_ptr mutable             connection_;
    endpoint_list::const_iterator mutable   current_;
};

class routed_reference : public reference {
};

}  // namespace core
}  // namespace wire



#endif /* WIRE_CORE_REFERENCE_HPP_ */
