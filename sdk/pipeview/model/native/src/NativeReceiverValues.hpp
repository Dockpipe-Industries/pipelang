// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pipelang/model/native_host_value.hpp>
#include <pipelang/model/collection.hpp>

#include <map>
#include <memory>

namespace pipelang::native_app_v19 {

class NativeReceiverValues {
public:
    struct Snapshot {
        HostedValue value;
        std::size_t bytes;
        std::vector<const void*> references;
    };

    using Value = std::shared_ptr<const Snapshot>;

    class Field {
    public:
        const Value& current() const { return current_; }

    private:
        friend class NativeReceiverValues;
        Value current_;
    };

    Field& field(const void* receiver, const std::uint64_t* address)
    {
        return receivers_[receiver][address];
    }

    const Field* find(const void* receiver, const std::uint64_t* address) const
    {
        const auto fields = receivers_.find(receiver);
        if (fields == receivers_.end())
            return nullptr;
        const auto found = fields->second.find(address);
        return found == fields->second.end() ? nullptr : &found->second;
    }

    Value read(const void* receiver, const std::uint64_t* address) const
    {
        const auto* field = find(receiver, address);
        return field ? field->current() : Value{};
    }

    template<class Visit>
    void visitReferences(const void* receiver, const Visit& visit) const
    {
        const auto fields = receivers_.find(receiver);
        if (fields == receivers_.end())
            return;
        for (const auto& [address, field] : fields->second) {
            if (!field.current())
                continue;
            for (const auto* reference : field.current()->references)
                visit(reference);
        }
    }

    void discard(const void* receiver) noexcept
    {
        const auto fields = receivers_.find(receiver);
        if (fields == receivers_.end())
            return;
        for (auto& [address, field] : fields->second)
            replace(field, {});
        receivers_.erase(fields);
    }

    void admitReplacement(std::size_t previousBytes, std::size_t newBytes) const
    {
        if (newBytes > maximumBytes - (bytes_ - previousBytes))
            throw model::EvaluationLimitFailure("native receiver value storage limit exceeded");
    }

    void replace(Field& field, Value value) noexcept
    {
        if (field.current_)
            bytes_ -= field.current_->bytes;
        if (value)
            bytes_ += value->bytes;
        field.current_ = std::move(value);
    }

    static constexpr auto maximumBytes = 32 * model::maximumListBytes;

private:
    std::map<const void*, std::map<const std::uint64_t*, Field>> receivers_;
    std::size_t bytes_ = 0;
};

}
