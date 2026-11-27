// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>

namespace pipelang::native_app_v19 {

namespace detail {

class HostedSequenceCell {
public:
    using Projection = std::function<void(const std::optional<HostedValue>&)>;

    virtual ~HostedSequenceCell() = default;
    virtual std::int64_t count() const = 0;
    virtual void clear() const = 0;
    virtual void append(const HostedValue& value) const = 0;
    virtual void take(const Projection& project) const = 0;
    virtual std::optional<HostedValue> peek() const = 0;
    virtual std::vector<HostedValue> items() const = 0;
    virtual std::string_view elementType() const noexcept = 0;
};

template<bool IsStack, class Cell, class Item>
class TypedHostedSequenceCell final : public HostedSequenceCell {
public:
    TypedHostedSequenceCell(Cell cell, HostedValueCodec<Item> codec)
        : cell_(std::move(cell)), codec_(std::move(codec))
    {
        cell_.requireCell();
        if (codec_.type.empty() || !codec_.encode || !codec_.decode)
            throw model::ExecutionFailure("hosted concurrent sequence requires a checked element codec");
    }

    std::int64_t count() const override { return cell_.count(); }
    void clear() const override { cell_.clear(); }

    void append(const HostedValue& value) const override
    {
        if constexpr (IsStack)
            cell_.push(codec_.decode(value));
        else
            cell_.enqueue(codec_.decode(value));
    }

    void take(const Projection& project) const override
    {
        const auto projectItem = [&](const model::Optional<Item>& value) {
            project(encode(value));
        };
        if constexpr (IsStack)
            cell_.tryPopProjected(projectItem);
        else
            cell_.tryDequeueProjected(projectItem);
    }

    std::optional<HostedValue> peek() const override
    {
        return encode(cell_.tryPeek());
    }

    std::vector<HostedValue> items() const override
    {
        const auto snapshot = cell_.items();
        std::vector<HostedValue> values;
        values.reserve(static_cast<std::size_t>(snapshot.count()));
        for (const auto& item : snapshot.values())
            values.push_back(codec_.project(item));
        return values;
    }

    std::string_view elementType() const noexcept override { return codec_.type; }

private:
    std::optional<HostedValue> encode(const model::Optional<Item>& item) const
    {
        if (!item.has_value())
            return {};
        return codec_.project(item.value());
    }

    Cell cell_;
    HostedValueCodec<Item> codec_;
};

}

template<class Item>
HostedValue HostedValue::fromConcurrentQueue(model::ConcurrentQueue<Item> queue, HostedValueCodec<Item> codec)
{
    auto type = "ConcurrentQueue<" + codec.type + '>';
    auto cell = std::make_shared<detail::TypedHostedSequenceCell<false, model::ConcurrentQueue<Item>, Item>>(
        std::move(queue), std::move(codec));
    return fromHostedSequence(std::move(type), std::move(cell));
}

template<class Item>
HostedValue HostedValue::fromConcurrentStack(model::ConcurrentStack<Item> stack, HostedValueCodec<Item> codec)
{
    auto type = "ConcurrentStack<" + codec.type + '>';
    auto cell = std::make_shared<detail::TypedHostedSequenceCell<true, model::ConcurrentStack<Item>, Item>>(
        std::move(stack), std::move(codec));
    return fromHostedSequence(std::move(type), std::move(cell));
}

}
