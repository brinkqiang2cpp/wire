/*
 * reference.cpp
 *
 *  Created on: Apr 12, 2016
 *      Author: zmij
 */

#include <wire/core/reference.hpp>

#include <iostream>
#include <sstream>

#include <wire/core/connector.hpp>
#include <wire/core/locator.hpp>

namespace wire {
namespace core {

bool
operator == (reference_data const& lhs, reference_data const& rhs)
{
    return lhs.object_id == rhs.object_id &&
            lhs.facet == rhs.facet &&
            lhs.adapter == rhs.adapter &&
            lhs.endpoints == rhs.endpoints;
}
bool
operator != (reference_data const& lhs, reference_data const& rhs)
{
    return !(lhs == rhs);
}
bool
operator < (reference_data const& lhs, reference_data const& rhs)
{
    return lhs.object_id < rhs.object_id &&
            lhs.facet < rhs.facet &&
            lhs.adapter < rhs.adapter &&
            lhs.endpoints < rhs.endpoints;
}

reference_data
operator "" _wire_ref(char const* str, ::std::size_t sz)
{
    ::std::string literal{str, sz};
    ::std::istringstream is{literal};
    reference_data ref;
    if (!(is >> ref))
        throw ::std::runtime_error{"Invalid ::wire::core::reference_data literal " + literal};
    return ref;
}

::std::size_t
id_facet_hash(reference_data const& ref)
{
    auto h = hash(ref.object_id);
    return (h << 1) ^ hash(ref.facet);
}

::std::size_t
hash(reference_data const& ref)
{
    auto h = id_facet_hash(ref);
    if (ref.adapter.is_initialized())
        h = (h << 1) ^ hash(ref.adapter.get());
    h = (h << 1) ^ hash(ref.endpoints);
    return h;
}

//----------------------------------------------------------------------------
//      Base reference implementation
//----------------------------------------------------------------------------
reference_ptr
reference::create_reference(connector_ptr cnctr, reference_data const& ref_data)
{
    if (!ref_data.endpoints.empty()) {
        // Find a connection or create a new one
        return
            ::std::make_shared< fixed_reference >(cnctr, ref_data);
    }
    return ::std::make_shared< floating_reference >(cnctr, ref_data);
}

bool
reference::is_local() const
{
    if (local_object_cache_.lock())
        return true;
    return get_connector()->is_local(*this);
}

reference::local_servant
reference::get_local_object() const
{
    auto obj = local_object_cache_.lock();
    if (!obj) {
        auto loc_srv = get_connector()->find_local_servant(*this);
        obj = loc_srv.first;
        local_object_cache_ = obj;
        adapter_cache_      = loc_srv.second;
    }
    return {obj, adapter_cache_.lock()};
}

connection_ptr
reference::get_connection() const
{
    auto future = get_connection_async(true);
    return future.get();
}

connector_ptr
reference::get_connector() const
{
    auto cnctr = connector_.lock();
    if (!cnctr)
        throw errors::runtime_error{ "Connector already destroyed" };
    return cnctr;
}

asio_config::io_service_ptr
reference::io_service() const
{
    return get_connector()->io_service();
}

//----------------------------------------------------------------------------
//      Fixed reference implementation
//----------------------------------------------------------------------------
fixed_reference::fixed_reference(connector_ptr cn, reference_data const& ref)
    : reference{cn, ref}, current_{ref_.endpoints.end()}
{
    if (ref_.endpoints.empty())
        throw errors::runtime_error{ "Reference endpoint list is empty" };
}

fixed_reference::fixed_reference(fixed_reference const& rhs)
    : reference{rhs},
      connection_{rhs.connection_.lock()},
      current_{ref_.endpoints.end()}
{
}

fixed_reference::fixed_reference(fixed_reference&& rhs)
    : reference{::std::move(rhs)},
      connection_{rhs.connection_.lock()},
      current_{ref_.endpoints.end()}
{
}

void
fixed_reference::get_connection_async( connection_callback on_get,
        functional::exception_callback on_error,
        bool sync) const
{
    connection_ptr conn = connection_.lock();
    if (!conn) {
        lock_guard lock{mutex_};
        conn = connection_.lock();
        if (conn) {
            try {
                on_get(conn);
            } catch (...) {}
            return;
        }
        connector_ptr cntr = get_connector();
        if (current_ == ref_.endpoints.end()) {
            current_ = ref_.endpoints.begin();
        }
        auto _this = shared_this<fixed_reference>();
        cntr->get_outgoing_connection_async(
            *current_++,
            [_this, on_get, on_error](connection_ptr c)
            {
                auto conn = _this->connection_.lock();
                if (!conn || conn != c) {
                    _this->connection_ = c;
                    conn = c;
                }
                try {
                    on_get(conn);
                } catch(...) {
                    try {
                        on_error(::std::current_exception());
                    } catch(...) {}
                }
            },
            [on_error](::std::exception_ptr ex)
            {
                try {
                    on_error(ex);
                } catch(...) {}
            },
            sync);
    } else {
        try {
            on_get(conn);
        } catch(...) {}
    }
}

//----------------------------------------------------------------------------
//      Floating reference implementation
//----------------------------------------------------------------------------

floating_reference::floating_reference(connector_ptr cn, reference_data const& ref)
    : reference{ cn, ref }
{
}

floating_reference::floating_reference(floating_reference const& rhs)
    : reference{rhs}
{
}

floating_reference::floating_reference(floating_reference&& rhs)
    : reference{ ::std::move(rhs) }
{
}

void
floating_reference::get_connection_async( connection_callback on_get,
        functional::exception_callback exception,
        bool sync ) const
{
    connector_ptr cnctr = get_connector();
    cnctr->resolve_connection_async(ref_, on_get, exception, sync);
}


}  // namespace core
}  // namespace wire
