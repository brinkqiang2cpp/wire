/*
 * locator_impl.cpp
 *
 *  Created on: Sep 27, 2016
 *      Author: zmij
 */

#include <wire/locator/locator_impl.hpp>
#include <wire/util/make_unique.hpp>
#include <wire/errors/not_found.hpp>

#include <iostream>
#include <mutex>
#include <tbb/concurrent_hash_map.h>

namespace wire {
namespace svc {

struct callback_accumulator {

    struct data {
        ::wire::core::functional::void_callback         responce;
        ::wire::core::functional::exception_callback    exception_cb;
        ::std::exception_ptr                            exception = nullptr;

        data(::wire::core::functional::void_callback         __resp,
             ::wire::core::functional::exception_callback    __exception)
            : responce(__resp), exception_cb(__exception)
        {
        }
        ~data()
        {
            using core::functional::report_exception;
            if (exception) {
                report_exception(exception_cb, exception);
            } else {
                responce();
            }
        }
    };

    callback_accumulator(
            ::wire::core::functional::void_callback         __resp,
            ::wire::core::functional::exception_callback    __exception)
        : data_{ ::std::make_shared<data>(__resp, __exception ) } {}

    void
    operator()() {}
    void
    operator()(::std::exception_ptr ex)
    {
        if (!data_->exception)
            data_->exception = ex;
    }

    ::std::shared_ptr<data> data_;
};

struct locator::impl {
    using proxy_map         = ::tbb::concurrent_hash_map< core::identity, core::object_prx >;

    core::locator_registry_prx  reg_;

    proxy_map                   objects_;
    proxy_map                   adapters_;

    impl(core::locator_registry_prx reg) : reg_{reg} {}

    core::object_prx
    find_object(core::identity const& id)
    {
        #if DEBUG_OUTPUT >= 1
        ::std::cerr <<::getpid() << " Locator lookup well-known object " << id << "\n";
        #endif
        proxy_map::const_accessor f;
        if (objects_.find(f, id)) {
            return f->second;
        }
        ::std::cerr <<::getpid() << " Locator well-known object " << id << " not found\n";
        throw core::object_not_found{id};
    }

    void
    add_well_known_object(::wire::core::object_prx obj,
            ::wire::core::functional::void_callback __resp,
            ::wire::core::functional::exception_callback __exception)
    {
        using core::functional::report_exception;
        if (!obj)
            report_exception(__exception, core::not_enough_data{});
        auto const& ref = obj->wire_get_reference()->data();
        if (!ref.adapter.is_initialized() && ref.endpoints.empty()) {
            #if DEBUG_OUTPUT >= 1
            ::std::cerr <<::getpid() << " Locator cannot add well-known object " << *obj << "\n";
            #endif
            report_exception(__exception, core::not_enough_data{});
        }
        #if DEBUG_OUTPUT >= 1
        ::std::cerr <<::getpid() << " Locator add well-known object " << *obj << "\n";
        #endif
        auto id = ref.object_id;
        proxy_map::accessor f;
        if (!objects_.find(f, id)) {
            objects_.emplace(id, obj);
        } else {
            if (*f->second != *obj) {
                f->second->wire_ping_async(
                    [__exception]()
                    {
                        report_exception(__exception, core::well_known_object_exists{});
                    },
                    [this, __resp, id, obj](::std::exception_ptr ex)
                    {
                        // Object is not there, replacing
                        proxy_map::accessor f;
                        if (!objects_.find(f, id)) {
                            objects_.emplace(id, obj);
                        } else {
                            f->second = obj;
                        }
                        __resp();
                    });
                return;
            }
        }
        __resp();
    }
    void
    add_well_known_objects(::wire::core::object_seq const& objs,
            ::wire::core::functional::void_callback __resp,
            ::wire::core::functional::exception_callback __exception)
    {
        callback_accumulator cb(__resp, __exception);
        for (auto const& obj: objs) {
            add_well_known_object(obj, cb, cb);
        }
    }
    void
    add_well_known_object(core::object_prx obj)
    {
        if (!obj)
            throw core::not_enough_data{};
        auto const& ref = obj->wire_get_reference()->data();
        if (!ref.adapter.is_initialized() && ref.endpoints.empty()) {
            #if DEBUG_OUTPUT >= 1
            ::std::cerr <<::getpid() << " Locator cannot add well-known object " << *obj << "\n";
            #endif
            throw core::not_enough_data{};
        }
        #if DEBUG_OUTPUT >= 1
        ::std::cerr <<::getpid() << " Locator add well-known object " << *obj << "\n";
        #endif
        auto id = ref.object_id;
        proxy_map::accessor f;
        if (!objects_.find(f, id)) {
            objects_.emplace(id, obj);
        } else {
            if (*f->second != *obj) {
                bool alive = true;
                try {
                    f->second->wire_ping();
                    // Object is alive, throw exception
                } catch (...) {
                    // Object is dead, safe to replace
                    alive = false;
                }
                if (alive) {
                    // TODO Throw an exception
                    throw core::well_known_object_exists{};
                } else {
                    f->second = obj;
                }
            }
        }
    }

    void
    remove_well_known_object(core::object_prx obj)
    {
        if (obj) {
            auto const& id = obj->wire_identity();
            #if DEBUG_OUTPUT >= 1
            ::std::cerr <<::getpid() << " Locator remove well-known object " << id << "\n";
            #endif
            proxy_map::accessor f;
            if (objects_.find(f, id)) {
                objects_.erase(f);
            }
        }
    }

    core::object_prx
    find_adapter(core::identity const& id)
    {
        proxy_map::const_accessor f;
        if (adapters_.find(f, id)) {
            return f->second;
        }
        throw core::adapter_not_found{id};
    }

    void
    add_adapter(core::object_prx adapter)
    {
        if (!adapter)
            throw core::not_enough_data{};
        auto const& ref = adapter->wire_get_reference()->data();
        if (ref.endpoints.empty())
            throw core::not_enough_data{};
        auto id = ref.object_id;

        #if DEBUG_OUTPUT >= 1
        ::std::cerr <<::getpid() << " Locator add adapter " << *adapter << "\n";
        #endif
        proxy_map::accessor f;
        // TODO Devise some sort of policies to throw on adapter exists
        if (!adapters_.find(f, id)) {
            adapters_.emplace(id, adapter);
        } else {
            f->second = adapter;
        }
    }
    void
    add_replicated_adapter(core::object_prx adapter)
    {
        if (!adapter)
            throw core::not_enough_data{};
        auto const& ref = adapter->wire_get_reference()->data();
        if (ref.endpoints.empty())
            throw core::not_enough_data{};
        auto id = ref.object_id;
        if (id.category.empty()) {
            throw core::no_category_for_replica{};
        }
        #if DEBUG_OUTPUT >= 1
        ::std::cerr <<::getpid() << " Locator add replicated adapter " << *adapter << "\n";
        #endif

        if (id.is_wildcard()) {
            proxy_map::accessor f;
            if (!adapters_.find(f, id)) {
                adapters_.emplace(id, adapter);
            } else {
                auto data = f->second->wire_get_reference()->data();
                auto const& new_data = adapter->wire_get_reference()->data();

                auto eps = core::merge_endpoints(data.endpoints, new_data.endpoints);

                data.endpoints.clear();
                data.endpoints.reserve(eps.size());
                ::std::copy(eps.begin(), eps.end(), ::std::back_inserter(data.endpoints));

                auto ref = core::reference::create_reference(reg_->wire_get_connector(), data);
                f->second = ::std::make_shared< core::object_proxy >( ref );
            }
        } else {
            // Register normal adapter
            add_adapter(adapter);
            // Add/merge endpoints to replica
            core::identity replica_id = core::identity::wildcard(id.category);
            auto replica_prx = adapter->wire_with_identity(replica_id);
            add_replicated_adapter(replica_prx);
        }
    }
    void
    remove_adapter(core::object_prx adapter)
    {
        auto id = adapter->wire_identity();
        proxy_map::accessor f;
        if (!adapters_.find(f, id)) {
            throw core::adapter_not_found{id};
        } else {
            #if DEBUG_OUTPUT >= 1
            ::std::cerr <<::getpid() << " Locator remove adapter " << *adapter << "\n";
            #endif
            if (id.is_wildcard()) {
                auto data = f->second->wire_get_reference()->data();
                auto const& new_data = adapter->wire_get_reference()->data();
                core::endpoint_set old_eps{data.endpoints.begin(), data.endpoints.end()};
                core::endpoint_set remove_eps{new_data.endpoints.begin(), new_data.endpoints.end()};

                data.endpoints.clear();
                ::std::set_difference(
                    old_eps.begin(), old_eps.end(),
                    remove_eps.begin(), remove_eps.end(),
                    ::std::back_inserter(data.endpoints));
                if (data.endpoints.empty()) {
                    adapters_.erase(f);
                } else {
                    auto ref = core::reference::create_reference(reg_->wire_get_connector(), data);
                    f->second = ::std::make_shared< core::object_proxy >( ref );
                }
            } else {
                adapters_.erase(f);
                if (!id.category.empty()) {
                    core::identity replica_id = core::identity::wildcard(id.category);
                    auto replica_prx = adapter->wire_with_identity(replica_id);
                    remove_adapter(replica_prx);
                }
            }
        }
    }
};

locator::locator(core::locator_registry_prx registry)
    : pimpl_{util::make_unique<impl>(registry)}
{
}

locator::~locator() = default;

void
locator::find_object(::wire::core::identity&& id,
            find_object_return_callback __resp,
            ::wire::core::functional::exception_callback __exception,
            ::wire::core::current const&) const
{
    try {
        __resp(pimpl_->find_object(id));
    } catch (...) {
        __exception(::std::current_exception());
    }
}

void
locator::find_adapter(::wire::core::identity&& id,
        find_adapter_return_callback __resp,
        ::wire::core::functional::exception_callback __exception,
        ::wire::core::current const&) const
{
    try {
        __resp(pimpl_->find_adapter(id));
    } catch (...) {
        __exception(::std::current_exception());
    }
}

void
locator::get_registry(get_registry_return_callback __resp,
        ::wire::core::functional::exception_callback __exception,
        ::wire::core::current const&) const
{
    try {
        __resp(pimpl_->reg_);
    } catch (...) {
        __exception(::std::current_exception());
    }
}

void
locator::add_well_known_object(::wire::core::object_prx object,
        ::wire::core::functional::void_callback __resp,
        ::wire::core::functional::exception_callback __exception)
{
    pimpl_->add_well_known_object(object, __resp, __exception);
}

void
locator::add_well_known_objects(::wire::core::object_seq const& objs,
        ::wire::core::functional::void_callback __resp,
        ::wire::core::functional::exception_callback __exception)
{
    pimpl_->add_well_known_objects(objs, __resp, __exception);
}


void
locator::add_well_known_object(core::object_prx obj)
{
    pimpl_->add_well_known_object(obj);
}

void
locator::remove_well_known_object(core::object_prx obj)
{
    pimpl_->remove_well_known_object(obj);
}

void
locator::add_adapter(core::object_prx adapter)
{
    pimpl_->add_adapter(adapter);
}

void
locator::add_replicated_adapter(core::object_prx adapter)
{
    pimpl_->add_replicated_adapter(adapter);
}

void
locator::remove_adapter(core::object_prx adapter)
{
    pimpl_->remove_adapter(adapter);
}

//----------------------------------------------------------------------------
//  Registry interface implementation
//----------------------------------------------------------------------------
locator_registry::locator_registry(locator_ptr loc)
    : loc_{loc}
{
}

void
locator_registry::add_well_known_object(::wire::core::object_prx obj,
        ::wire::core::functional::void_callback __resp,
        ::wire::core::functional::exception_callback __exception,
        ::wire::core::current const&)
{
    try {
        loc_->add_well_known_object(obj, __resp, __exception);
    } catch (...) {
        __exception(::std::current_exception());
    }
}

void
locator_registry::add_well_known_objects(::wire::core::object_seq&& objs,
        ::wire::core::functional::void_callback __resp,
        ::wire::core::functional::exception_callback __exception,
        ::wire::core::current const&)
{
    loc_->add_well_known_objects(objs, __resp, __exception);
}

void
locator_registry::remove_well_known_object(::wire::core::object_prx obj,
        ::wire::core::functional::void_callback __resp,
        ::wire::core::functional::exception_callback __exception,
        ::wire::core::current const&)
{
    try {
        loc_->remove_well_known_object(obj);
        __resp();
    } catch (...) {
        __exception(::std::current_exception());
    }
}

void
locator_registry::add_adapter(::wire::core::object_prx adapter,
        ::wire::core::functional::void_callback __resp,
        ::wire::core::functional::exception_callback __exception,
        ::wire::core::current const&)
{
    try {
        loc_->add_adapter(adapter);
        __resp();
    } catch (...) {
        __exception(::std::current_exception());
    }
}

void
locator_registry::add_replicated_adapter(::wire::core::object_prx adapter,
        ::wire::core::functional::void_callback __resp,
        ::wire::core::functional::exception_callback __exception,
        ::wire::core::current const&)
{
    try {
        loc_->add_replicated_adapter(adapter);
        __resp();
    } catch (...) {
        __exception(::std::current_exception());
    }
}

void
locator_registry::remove_adapter(::wire::core::object_prx adapter,
        ::wire::core::functional::void_callback __resp,
        ::wire::core::functional::exception_callback __exception,
        ::wire::core::current const&)
{
    try {
        loc_->remove_adapter(adapter);
        __resp();
    } catch (...) {
        __exception(::std::current_exception());
    }
}

}  /* namespace service */
}  /* namespace wire */

