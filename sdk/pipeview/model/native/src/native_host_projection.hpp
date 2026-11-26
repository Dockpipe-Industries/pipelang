// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <pipelang/model/native_host_value.hpp>

#include <list>
#include <thread>
#include <unordered_map>

namespace pipelang::native_app_v19 {

class NativeLists;
class NativeReceivers;
struct NativeReceiverStorage;

struct ExportedReceiverOwner {
    std::thread::id thread;
};

struct ExportedReceiverReference {
    std::weak_ptr<const ExportedReceiverOwner> owner;
    const void* address;

    std::shared_ptr<const ExportedReceiverOwner> requireOwner() const;
};

struct ReceiverReferenceDomain {
    const void* root;
    const NativeReceivers& receivers;
    const ExportedReceiverOwner* exportedOwner = nullptr;

    bool owns(const void* receiver) const;
};

class HostedInput {
public:
    explicit HostedInput(const ReceiverReferenceDomain* domain = nullptr) : domain_(domain) {}

    TextView retainText(std::string_view source);
    ByteView retainBytes(std::span<const std::uint8_t> source, const void* reusableSlot = nullptr);

    // This storage owns any text referenced by the packed words.
    void pack(const OwnedShape& shape, const HostedValue& value, std::uint64_t* output, NativeLists* lists = nullptr);
    void pack(const OwnedShape& shape, const HostedValue& value, std::uint64_t* output, NativeLists* lists,
              const void* reusableByteSlot);

private:
    void pack(const OwnedShape& shape, const HostedValue& value, std::uint64_t* output, NativeLists* lists,
              std::size_t depth, const void* reusableByteSlot = nullptr);
    std::list<std::string> text_;
    std::unordered_map<const void*, std::string> reusableBytes_;
    std::size_t textBytes_ = 0;
    const ReceiverReferenceDomain* domain_;
};

class HostedProjection {
public:
    struct RetainedField {
        const std::uint64_t* address;
        const OwnedShape* shape;
        HostedValue value;
        std::size_t bytes;
    };

    explicit HostedProjection(const NativeLists& lists, const ReceiverReferenceDomain* domain = nullptr,
                              std::span<const RetainedField> retained = {}, NativeReceiverStorage* exporter = nullptr)
        : lists_(lists), domain_(domain), retained_(retained), exporter_(exporter) {}
    HostedValue copy(const OwnedShape& shape, const std::uint64_t* payload);
    std::size_t bytes() const { return bytes_; }
    std::vector<const void*> takeReferences() { return std::exchange(references_, {}); }

private:
    HostedValue copy(const OwnedShape& shape, const std::byte* payload, std::size_t depth);
    void charge(std::size_t bytes);
    void copyText(std::string& destination, std::string_view source);

    const NativeLists& lists_;
    const ReceiverReferenceDomain* domain_;
    std::span<const RetainedField> retained_;
    NativeReceiverStorage* exporter_;
    std::vector<const void*> references_;
    std::size_t bytes_ = 0;
};

}
