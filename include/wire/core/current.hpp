/*
 * current.hpp
 *
 *  Created on: Feb 3, 2016
 *      Author: zmij
 */

#ifndef WIRE_CORE_CURRENT_HPP_
#define WIRE_CORE_CURRENT_HPP_

#include <wire/encoding/message.hpp>
#include <wire/core/context.hpp>
#include <wire/core/endpoint.hpp>
#include <wire/core/adapter_fwd.hpp>

namespace wire {
namespace core {

struct current {
    encoding::operation_specs   operation;
    context_const_ptr           context;
    endpoint                    peer_endpoint;
    adapter_ptr                 adapter;

    context_type const&
    get_context() const
    { return context ? *context : no_context; }
};

extern const current no_current;

}  // namespace core
}  // namespace wire

#endif /* WIRE_CORE_CURRENT_HPP_ */
