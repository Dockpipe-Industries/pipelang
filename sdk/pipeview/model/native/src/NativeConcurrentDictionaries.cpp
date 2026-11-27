// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeConcurrentDictionaries.hpp"

#include "NativeCollectionKey.hpp"
#include "NativeConcurrentValue.hpp"
#include "NativeLists.hpp"
#include "native_host_projection.hpp"

#include <pipelang/model/concurrent_collections.hpp>
#include <pipelang/model/object.hpp>

#include <string>
#include <utility>
#include <variant>

namespace pipelang::native_app_v19 {
namespace {

template<class Key>
using Dictionary = model::ConcurrentDictionary<Key, NativeConcurrentValue>;

using HostedDictionary = std::shared_ptr<const detail::HostedDictionaryCell>;
using Cell = std::variant<Dictionary<std::int64_t>, Dictionary<bool>, Dictionary<std::string>,
                          Dictionary<UnsignedCollectionKey>, HostedDictionary>;

template<class Key>
HostedValue projectKey(const OwnedShape& shape, const Key& key, HostedProjection& projection)
{
    HostedValue result;
    visitCollectionKeyWords(key, [&](const void* value) {
        result = projection.copy(shape, static_cast<const std::uint64_t*>(value));
    });
    return result;
}

}

struct NativeConcurrentDictionaries::Entry {
    Cell cell;
    const OwnedShape* key;
    const OwnedShape* value;
};

NativeConcurrentDictionaries::NativeConcurrentDictionaries() = default;
NativeConcurrentDictionaries::~NativeConcurrentDictionaries() = default;

std::size_t NativeConcurrentDictionaries::checkpoint() const noexcept
{
    return cells_.checkpoint();
}

void NativeConcurrentDictionaries::restore(std::size_t checkpoint) noexcept
{
    cells_.restore(checkpoint);
}

void NativeConcurrentDictionaries::retire(std::uint64_t handle) noexcept
{
    cells_.retire(handle - 1);
}

std::uint64_t NativeConcurrentDictionaries::retain(Entry entry)
{
    return cells_.retain(std::move(entry), "native concurrent dictionary storage limit exceeded") + 1;
}

std::uint64_t NativeConcurrentDictionaries::create(const OwnedShape& key, const OwnedShape& value)
{
    return visitCollectionKey(key, "concurrent dictionary", [&]<class Key>() {
        return retain({Dictionary<Key>::Empty(), &key, &value});
    });
}

std::uint64_t NativeConcurrentDictionaries::borrow(const NativeConcurrentDictionaries& parent, std::uint64_t handle)
{
    return retain(parent.at(handle));
}

std::uint64_t NativeConcurrentDictionaries::borrow(const HostedValue& dictionary)
{
    return retain({dictionary.hostedDictionary(), nullptr, nullptr});
}

const NativeConcurrentDictionaries::Entry& NativeConcurrentDictionaries::at(std::uint64_t handle) const
{
    const auto* cell = cells_.find(handle - 1);
    if (!cell)
        throw model::ExecutionFailure("native concurrent dictionary does not belong to the invocation");
    return *cell;
}

void NativeConcurrentDictionaries::call(std::uint64_t handle, ConcurrentDictionaryOperation operation,
                                        const OwnedShape& keyShape, const OwnedShape& valueShape,
                                        const std::uint64_t* key, const std::uint64_t* input, const std::uint64_t* comparison,
                                        const OwnedShape* result, std::uint64_t* output, NativeLists& lists,
                                        HostedInput& packing) const
{
    using Operation = ConcurrentDictionaryOperation;
    const auto& entry = at(handle);
    const auto* hosted = std::get_if<HostedDictionary>(&entry.cell);
    const bool typesMatch = hosted
        ? keyShape.name && valueShape.name && (*hosted)->keyType() == keyShape.name && (*hosted)->valueType() == valueShape.name
        : entry.key == &keyShape && entry.value == &valueShape;
    if (!typesMatch)
        throw model::ExecutionFailure("native concurrent dictionary types do not match its cell");

    HostedProjection projection(lists);
    const auto write = [&](const HostedValue& value) {
        if (!result || !output)
            throw model::ExecutionFailure("native concurrent dictionary output is missing");
        packing.pack(*result, value, output, &lists);
    };
    const auto writeValue = [&](const NativeConcurrentValue& value) { write(value.value); };
    const auto writeOptional = [&](const HostedValue& selected) {
        if (!result)
            throw model::ExecutionFailure("native concurrent dictionary optional shape is missing");
        write(HostedValue::fromOptional(std::string(result->name), selected));
    };
    const auto project = [&](const model::Optional<NativeConcurrentValue>& value) {
        writeOptional(value.has_value() ? value.value().value : HostedValue{});
    };
    const auto prepare = [&] {
        if (!input)
            throw model::ExecutionFailure("native concurrent dictionary input is missing");
        return NativeConcurrentValue{projection.copy(valueShape, input), lists.ownership(valueShape, input)};
    };
    const auto matches = [&](const HostedValue& current) {
        if (!comparison)
            throw model::ExecutionFailure("native concurrent dictionary comparand is missing");
        std::vector<std::uint64_t> data(valueShape.words);
        packing.pack(valueShape, current, data.data(), &lists);
        return lists.equal(valueShape, reinterpret_cast<const std::byte*>(data.data()),
                           reinterpret_cast<const std::byte*>(comparison));
    };

    if (hosted) {
        const auto& dictionary = **hosted;
        const auto projectItem = [&](const detail::HostedDictionaryCell::Item& item) {
            writeOptional(item.value_or(HostedValue{}));
        };
        const auto writeList = [&](std::vector<HostedValue> values) {
            if (!result)
                throw model::ExecutionFailure("native concurrent dictionary snapshot shape is missing");
            write(HostedValue::fromList(std::string(result->name), std::move(values)));
        };
        if (operation == Operation::Count)
            return write(HostedValue::fromInteger(dictionary.count()));
        if (operation == Operation::Clear)
            return dictionary.clear();
        if (operation == Operation::Keys)
            return writeList(dictionary.keys());
        if (operation == Operation::Values)
            return writeList(dictionary.values());
        if (operation == Operation::Entries) {
            if (!result || !result->fields)
                throw model::ExecutionFailure("native concurrent dictionary snapshot shape is missing");
            const auto& pair = *result->fields[0].shape;
            auto entries = dictionary.entries();
            std::vector<HostedValue> values;
            values.reserve(entries.size());
            for (auto& [entryKey, entryValue] : entries) {
                values.push_back(HostedValue::fromRecord(std::string(pair.name), {
                    {pair.fields[0].name, std::move(entryKey)},
                    {pair.fields[1].name, std::move(entryValue)}
                }));
            }
            return writeList(std::move(values));
        }

        if (!key)
            throw model::ExecutionFailure("native concurrent dictionary key is missing");
        const auto selected = projection.copy(keyShape, key);
        const auto hostedInput = [&] {
            if (!input)
                throw model::ExecutionFailure("native concurrent dictionary input is missing");
            return projection.copy(valueShape, input);
        };
        const bool returnsBoolean = operation == Operation::Contains || operation == Operation::TryAdd ||
            operation == Operation::TryUpdate;
        if (returnsBoolean && !output)
            throw model::ExecutionFailure("native concurrent dictionary Boolean output is missing");
        switch (operation) {
        case Operation::At: return write(dictionary.at(selected));
        case Operation::TryGet: return projectItem(dictionary.tryGet(selected));
        case Operation::Contains:
            *output = dictionary.contains(selected);
            return;
        case Operation::TryAdd:
            *output = dictionary.tryAdd(selected, hostedInput());
            return;
        case Operation::Set: return dictionary.set(selected, hostedInput());
        case Operation::TryRemove: return dictionary.remove(selected, projectItem);
        case Operation::GetOrAdd: return dictionary.getOrAdd(selected, hostedInput(), write);
        case Operation::TryUpdate:
            *output = dictionary.update(selected, hostedInput(), matches);
            return;
        default: throw model::ExecutionFailure("invalid native concurrent dictionary operation");
        }
    }

    visitCollectionKey(keyShape, "concurrent dictionary", [&]<class Key>() {
        const auto& dictionary = std::get<Dictionary<Key>>(entry.cell);
        if (operation == Operation::Count)
            return write(HostedValue::fromInteger(dictionary.count()));
        if (operation == Operation::Clear)
            return dictionary.clear();

        if (operation == Operation::Keys || operation == Operation::Values || operation == Operation::Entries) {
            if (!result || !result->fields)
                throw model::ExecutionFailure("native concurrent dictionary snapshot shape is missing");
            std::vector<HostedValue> values;
            if (operation == Operation::Keys) {
                const auto snapshot = dictionary.keys();
                for (const auto& selected : snapshot.values())
                    values.push_back(projectKey(keyShape, selected, projection));
            } else if (operation == Operation::Values) {
                const auto snapshot = dictionary.mappedValues();
                for (const auto& selected : snapshot.values())
                    values.push_back(selected.value);
            } else {
                const auto snapshot = dictionary.entries();
                const auto& pair = *result->fields[0].shape;
                for (const auto& selected : snapshot.values())
                    values.push_back(HostedValue::fromRecord(std::string(pair.name), {
                        {pair.fields[0].name, projectKey(keyShape, selected.key, projection)},
                        {pair.fields[1].name, selected.value.value}
                    }));
            }
            return write(HostedValue::fromList(std::string(result->name), std::move(values)));
        }

        if (!key)
            throw model::ExecutionFailure("native concurrent dictionary key is missing");
        auto selected = loadCollectionKey<Key>(keyShape, key, "concurrent dictionary");
        switch (operation) {
        case Operation::At: return writeValue(dictionary.at(selected));
        case Operation::TryGet: return project(dictionary.tryGet(selected));
        case Operation::Contains:
            if (!output)
                throw model::ExecutionFailure("native concurrent dictionary Boolean output is missing");
            *output = dictionary.containsKey(selected);
            return;
        case Operation::TryAdd:
            if (!output)
                throw model::ExecutionFailure("native concurrent dictionary Boolean output is missing");
            *output = dictionary.tryAdd(std::move(selected), prepare());
            return;
        case Operation::Set: return dictionary.set(std::move(selected), prepare());
        case Operation::TryRemove: return dictionary.tryRemoveProjected(selected, project);
        case Operation::GetOrAdd: return dictionary.getOrAddProjected(std::move(selected), prepare(), writeValue);
        case Operation::TryUpdate:
            if (!output)
                throw model::ExecutionFailure("native concurrent dictionary Boolean output is missing");
            *output = dictionary.tryUpdateMatching(std::move(selected), prepare(),
                [&](const NativeConcurrentValue& current) { return matches(current.value); });
            return;
        default: throw model::ExecutionFailure("invalid native concurrent dictionary operation");
        }
    });
}

}
