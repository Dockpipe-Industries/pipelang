// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <pipelang/model/task.hpp>
#include <span>
#include <vector>

namespace pipelang::native_app_v19 {

class NativeLists;
struct NativeReceiverStorage;
class HostedService;
class HostedValue;
class InvocationSemaphores;
class InvocationLocks;
class InvocationAtomics;
class NativeConcurrentSequences;
class NativeConcurrentDictionaries;
struct ScopedServiceUse {};

struct HostServiceBinding {
    std::shared_ptr<HostedService> service;
    std::uint32_t generation = 0;
    std::shared_ptr<const ScopedServiceUse> use;
};
using HostServiceBindings = std::vector<HostServiceBinding>;

class BoundServices {
public:
    BoundServices() = default;
    BoundServices(const HostServiceBindings& bindings) : bindings_(&bindings) {}
    BoundServices(std::span<const HostServiceBinding> bindings) : fixed_(bindings) {}

    std::span<const HostServiceBinding> current() const
    {
        if (bindings_)
            return *bindings_;
        return fixed_;
    }

private:
    const HostServiceBindings* bindings_ = nullptr;
    std::span<const HostServiceBinding> fixed_;
};
using ReturnProjection = void (*)(void*, const std::uint64_t*, const NativeLists&);

class SourceTrace;
HostedValue projectReceiverSnapshot(const NativeReceiverStorage& storage, const std::uint64_t* receiver,
                                   const OwnedShape& shape);

struct HostedSharedCells {
    InvocationSemaphores& semaphores;
    InvocationLocks& locks;
    InvocationAtomics& atomics;
    NativeConcurrentSequences& sequences;
    NativeConcurrentDictionaries& dictionaries;
};

struct HostedSharedArgument {
    ReceiverSynchronizationKind kind;
    std::uint64_t handle;
};

HostedSharedArgument borrowHostedArgument(const OwnedShape& shape, const HostedValue& value, HostedSharedCells cells);

struct InvocationValue {
    const OwnedShape* shape;
    const HostedValue* value;
    bool indirect;
};

model::Coroutine<model::Result<void>> runNative(
    model::TaskScope& scope, const AsyncFunction& entry, void* receiver, const Argument* arguments,
    model::CancellationToken cancellation, std::uint64_t& waits, ReturnProjection project, void* output,
    BoundServices services = {}, NativeReceiverStorage* receiverStorage = nullptr,
    std::span<const InvocationValue> invocationValues = {}, SourceTrace* trace = nullptr);

}
