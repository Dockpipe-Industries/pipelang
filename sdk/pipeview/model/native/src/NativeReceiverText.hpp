// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NativeReceivers.hpp"

#include <pipelang/model/result.hpp>

#include <map>
#include <set>

namespace pipelang::native_app_v19 {

class NativeReceiverText {
public:
    using Value = std::shared_ptr<const std::string>;

    class Field {
    public:
        const Value& current() const { return current_; }

    private:
        friend class NativeReceiverText;
        Value current_;
    };

    NativeReceiverText(const void* root, const NativeReceivers& children) : root_(root), children_(children) {}

    bool owns(const void* receiver) const { return receiver == root_ || children_.owns(receiver); }

    Field& field(const std::uint64_t* address) { return fields_[address]; }

    const Field* find(const std::uint64_t* address) const
    {
        const auto found = fields_.find(address);
        return found == fields_.end() ? nullptr : &found->second;
    }

    void admit(const Field& field, std::size_t size) const
    {
        admitReplacement(byteSize(field.current_), size);
    }

    void admitReplacement(std::size_t previousBytes, std::size_t newBytes) const
    {
        if (newBytes > maximumBytes - (bytes_ - previousBytes))
            throw model::EvaluationLimitFailure("native receiver text storage limit exceeded");
    }

    void replace(Field& field, Value value) noexcept
    {
        bytes_ -= byteSize(field.current_);
        bytes_ += byteSize(value);
        field.current_ = std::move(value);
    }

    static constexpr auto maximumBytes = 32 * model::maximumStringBytes;

    void discard(const std::uint64_t* begin, const std::uint64_t* end) noexcept
    {
        for (auto field = fields_.lower_bound(begin); field != fields_.end() && fields_.key_comp()(field->first, end);) {
            replace(field->second, {});
            field = fields_.erase(field);
        }
    }

private:
    static std::size_t byteSize(const Value& value) { return value ? value->size() : 0; }

    const void* root_;
    const NativeReceivers& children_;
    std::map<const std::uint64_t*, Field> fields_;
    std::size_t bytes_ = 0;
};

class ReceiverTextBorrows {
public:
    void retain(const NativeReceiverText::Value& text)
    {
        if (!text || values_.contains(text))
            return;
        if (values_.size() >= maximumFrameWords || text->size() > NativeReceiverText::maximumBytes - bytes_)
            throw model::EvaluationLimitFailure("native invocation receiver text storage limit exceeded");
        values_.insert(text);
        bytes_ += text->size();
    }

private:
    // A local may borrow an older field after the segment commits its replacement.
    std::set<NativeReceiverText::Value> values_;
    std::size_t bytes_ = 0;
};

}
