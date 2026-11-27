// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NativeCellSlots.hpp"
#include "native_app_abi.hpp"

#include <pipelang/model/native_host_value.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pipelang::model {
class EvaluationBudget;
}

namespace pipelang::native_app_v19 {

class NativeAtomicCell;

std::uint64_t callAtomicCell(const NativeAtomicCell& cell, AtomicValueKind kind, AtomicOperation operation,
                             std::uint64_t argument, std::uint64_t comparand, model::EvaluationBudget& budget);

class LocalAtomics {
public:
    static constexpr std::uint64_t handleTag = std::uint64_t{1} << 63;

    LocalAtomics();
    ~LocalAtomics();
    LocalAtomics(const LocalAtomics&) = delete;
    LocalAtomics& operator=(const LocalAtomics&) = delete;

    std::uint64_t initialize(std::size_t site, AtomicValueKind kind, std::uint64_t value,
                             model::EvaluationBudget& budget);
    const NativeAtomicCell& at(std::uint64_t handle) const;

private:
    struct Entry;
    std::vector<Entry> cells_;
};

class InvocationAtomics {
public:
    InvocationAtomics();
    ~InvocationAtomics();
    InvocationAtomics(const InvocationAtomics&) = delete;
    InvocationAtomics& operator=(const InvocationAtomics&) = delete;

    std::uint64_t borrowCell(const InvocationAtomics& parent, std::uint64_t handle);
    std::uint64_t borrowCell(const NativeAtomicCell& cell);
    std::uint64_t borrowCell(const HostedAtomic& cell);
    std::size_t checkpoint() const noexcept;
    void restore(std::size_t checkpoint) noexcept;
    void retire(std::uint64_t handle) noexcept;

    std::uint64_t create(AtomicValueKind kind, std::uint64_t value);
    const NativeAtomicCell& at(std::uint64_t handle) const;

private:
    NativeCellSlots<NativeAtomicCell> cells_;
};

}
