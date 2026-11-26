// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>

namespace pipelang::native_app_v19 {
namespace detail {

class HostedDictionaryCell {
public:
    using Item = std::optional<HostedValue>;
    using ProjectItem = std::function<void(const Item&)>;
    using ProjectValue = std::function<void(const HostedValue&)>;
    using Matches = std::function<bool(const HostedValue&)>;

    virtual ~HostedDictionaryCell() = default;
    virtual std::int64_t count() const = 0;
    virtual void clear() const = 0;
    virtual HostedValue at(const HostedValue& key) const = 0;
    virtual Item tryGet(const HostedValue& key) const = 0;
    virtual bool contains(const HostedValue& key) const = 0;
    virtual bool tryAdd(const HostedValue& key, const HostedValue& value) const = 0;
    virtual void set(const HostedValue& key, const HostedValue& value) const = 0;
    virtual void remove(const HostedValue& key, const ProjectItem& project) const = 0;
    virtual void getOrAdd(const HostedValue& key, const HostedValue& value, const ProjectValue& project) const = 0;
    virtual bool update(const HostedValue& key, const HostedValue& value, const Matches& matches) const = 0;
    virtual std::vector<HostedValue> keys() const = 0;
    virtual std::vector<HostedValue> values() const = 0;
    virtual std::vector<std::pair<HostedValue, HostedValue>> entries() const = 0;
    virtual std::string_view keyType() const noexcept = 0;
    virtual std::string_view valueType() const noexcept = 0;
};

template<class Key, class Value>
class TypedHostedDictionaryCell final : public HostedDictionaryCell {
public:
    TypedHostedDictionaryCell(model::ConcurrentDictionary<Key, Value> cell, HostedValueCodec<Key> key,
                              HostedValueCodec<Value> value)
        : cell_(std::move(cell)), key_(std::move(key)), value_(std::move(value))
    {
        cell_.requireCell();
        if (key_.type.empty() || !key_.encode || !key_.decode ||
            value_.type.empty() || !value_.encode || !value_.decode)
            throw model::ExecutionFailure("hosted concurrent dictionary requires checked key and value codecs");
    }

    std::int64_t count() const override { return cell_.count(); }
    void clear() const override { cell_.clear(); }

    HostedValue at(const HostedValue& key) const override
    {
        return value_.project(cell_.at(key_.decode(key)));
    }

    Item tryGet(const HostedValue& key) const override
    {
        return encode(cell_.tryGet(key_.decode(key)));
    }

    bool contains(const HostedValue& key) const override
    {
        return cell_.containsKey(key_.decode(key));
    }

    bool tryAdd(const HostedValue& key, const HostedValue& value) const override
    {
        return cell_.tryAdd(key_.decode(key), value_.decode(value));
    }

    void set(const HostedValue& key, const HostedValue& value) const override
    {
        cell_.set(key_.decode(key), value_.decode(value));
    }

    void remove(const HostedValue& key, const ProjectItem& project) const override
    {
        cell_.tryRemoveProjected(key_.decode(key), [&](const auto& selected) { project(encode(selected)); });
    }

    void getOrAdd(const HostedValue& key, const HostedValue& value, const ProjectValue& project) const override
    {
        cell_.getOrAddProjected(key_.decode(key), value_.decode(value),
            [&](const Value& selected) { project(value_.project(selected)); });
    }

    bool update(const HostedValue& key, const HostedValue& value, const Matches& matches) const override
    {
        return cell_.tryUpdateMatching(key_.decode(key), value_.decode(value),
            [&](const Value& existing) { return matches(value_.project(existing)); });
    }

    std::vector<HostedValue> keys() const override
    {
        return snapshot(cell_.keys(), key_);
    }

    std::vector<HostedValue> values() const override
    {
        return snapshot(cell_.mappedValues(), value_);
    }

    std::vector<std::pair<HostedValue, HostedValue>> entries() const override
    {
        const auto snapshot = cell_.entries();
        std::vector<std::pair<HostedValue, HostedValue>> values;
        values.reserve(static_cast<std::size_t>(snapshot.count()));
        for (const auto& item : snapshot.values())
            values.emplace_back(key_.project(item.key), value_.project(item.value));
        return values;
    }

    std::string_view keyType() const noexcept override { return key_.type; }
    std::string_view valueType() const noexcept override { return value_.type; }

private:
    template<class Item>
    static std::vector<HostedValue> snapshot(const model::List<Item>& items, const HostedValueCodec<Item>& codec)
    {
        std::vector<HostedValue> values;
        values.reserve(static_cast<std::size_t>(items.count()));
        for (const auto& item : items.values())
            values.push_back(codec.project(item));
        return values;
    }

    Item encode(const model::Optional<Value>& item) const
    {
        if (!item.has_value())
            return {};
        return value_.project(item.value());
    }

    model::ConcurrentDictionary<Key, Value> cell_;
    HostedValueCodec<Key> key_;
    HostedValueCodec<Value> value_;
};

}

template<class Key, class Value>
HostedValue HostedValue::fromConcurrentDictionary(model::ConcurrentDictionary<Key, Value> dictionary,
    HostedValueCodec<Key> key, HostedValueCodec<Value> value)
{
    auto type = "ConcurrentDictionary<" + key.type + ',' + value.type + '>';
    auto cell = std::make_shared<detail::TypedHostedDictionaryCell<Key, Value>>(
        std::move(dictionary), std::move(key), std::move(value));
    return fromHostedDictionary(std::move(type), std::move(cell));
}

}
