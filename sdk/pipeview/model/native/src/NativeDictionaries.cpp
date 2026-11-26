// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeDictionaries.hpp"

#include "NativeCollectionKey.hpp"
#include "NativeLists.hpp"

#include <cstring>
#include <variant>
#include <vector>

namespace pipelang::native_app_v19 {

struct NativeDictionaryValue {
    const NativeLists* lists;
    const OwnedShape* shape;
    std::shared_ptr<const std::vector<std::uint64_t>> words;
    model::ListOwnership ownership;
    std::size_t pairBytes;

    bool operator==(const NativeDictionaryValue& other) const
    {
        return shape == other.shape && lists->equal(*shape, reinterpret_cast<const std::byte*>(words->data()),
                                                    reinterpret_cast<const std::byte*>(other.words->data()));
    }
};

// Dictionary's admission logic measures the reference representation rather
// than the temporary native words and their shared backing pointer.
template<class Key>
std::size_t ownedBytes(const model::KeyValuePair<Key, NativeDictionaryValue>& pair)
{
    const auto bytes = model::addOwnedBytes(pair.value.pairBytes, model::ownedBytes(pair.key));
    return model::addOwnedBytes(bytes, pair.value.ownership.bytes);
}

template<class Key>
std::size_t ownedElements(const model::KeyValuePair<Key, NativeDictionaryValue>& pair)
{
    return pair.value.ownership.elements;
}

void validate(const NativeDictionaryValue& value)
{
    if (!value.words || value.words->size() != value.shape->words)
        throw model::ExecutionFailure("invalid native Dictionary value");
}

class NativeDictionaries::Versions {
    template<class Key>
    using Dictionary = model::Dictionary<Key, NativeDictionaryValue>;

    using Storage = std::variant<Dictionary<std::int64_t>, Dictionary<bool>, Dictionary<std::string>,
                                 Dictionary<UnsignedCollectionKey>>;

    struct Version {
        const OwnedShape* shape;
        Storage dictionary;
    };

    const Version* find(DictionaryView view, const OwnedShape& shape) const
    {
        if (view.version == 0 && view.count == 0)
            return nullptr;
        if (view.version == 0 || view.version > versions_.size())
            throw model::ExecutionFailure("invalid native Dictionary handle");
        const auto& version = versions_[view.version - 1];
        if (version.shape != &shape || std::visit([](const auto& dictionary) {
                return static_cast<std::uint64_t>(dictionary.count());
            }, version.dictionary) != view.count)
            throw model::ExecutionFailure("native Dictionary handle does not match its shape or count");
        return &version;
    }

public:
    Checkpoint checkpoint() const noexcept { return {versions_.size(), bytes_}; }

    void restore(Checkpoint checkpoint) noexcept
    {
        while (versions_.size() > checkpoint.versions)
            versions_.pop_back();
        bytes_ = checkpoint.bytes;
    }

    template<class Visitor>
    static decltype(auto) visit(const Versions* arena, DictionaryView view, const OwnedShape& shape,
                               const Visitor& operation)
    {
        if (!arena && (view.version != 0 || view.count != 0))
            throw model::ExecutionFailure("invalid native Dictionary handle");
        const auto* version = arena ? arena->find(view, shape) : nullptr;
        return visitCollectionKey(*shape.fields[0].shape, "Dictionary", [&]<class Key>() -> decltype(auto) {
            static const auto empty = Dictionary<Key>::Empty();
            return operation.template operator()<Key>(version ? std::get<Dictionary<Key>>(version->dictionary) : empty);
        });
    }

    template<class Key>
    DictionaryView save(const OwnedShape& shape, Dictionary<Key> dictionary, std::size_t newPayloadBytes)
    {
        const auto count = static_cast<std::size_t>(dictionary.count());
        if (count == 0)
            return {};
        constexpr auto maximumArenaBytes = 32 * model::maximumListBytes;
        const auto cost = sizeof(Version) + dictionary.ownedByteCost() + newPayloadBytes +
            count * sizeof(std::shared_ptr<const typename Dictionary<Key>::Entry>);
        if (cost > maximumArenaBytes - bytes_)
            throw model::ExecutionFailure("native Dictionary version storage limit exceeded");
        versions_.push_back({&shape, std::move(dictionary)});
        bytes_ += cost;
        return {versions_.size(), count};
    }

    static bool equal(const Versions* arena, DictionaryView left, DictionaryView right, const OwnedShape& shape)
    {
        return visit(arena, left, shape, [&]<class Key>(const Dictionary<Key>& first) {
            if (!arena)
                return right.version == 0 && right.count == 0;
            const auto* second = arena->find(right, shape);
            if (!second)
                return first.count() == 0;
            return first == std::get<Dictionary<Key>>(second->dictionary);
        });
    }

private:
    std::vector<Version> versions_;
    std::size_t bytes_ = 0;
};

NativeDictionaries::NativeDictionaries() = default;
NativeDictionaries::~NativeDictionaries() = default;

NativeDictionaries::Checkpoint NativeDictionaries::checkpoint() const noexcept
{
    return versions_ ? versions_->checkpoint() : Checkpoint{};
}

void NativeDictionaries::restore(Checkpoint checkpoint) noexcept
{
    if (versions_)
        versions_->restore(checkpoint);
}

bool NativeDictionaries::equal(DictionaryView left, DictionaryView right, const OwnedShape& shape) const
{
    return Versions::equal(versions_.get(), left, right, shape);
}

model::ListOwnership NativeDictionaries::ownership(DictionaryView view, const OwnedShape& shape) const
{
    return Versions::visit(versions_.get(), view, shape, []<class Key>(const auto& dictionary) {
        return model::ListOwnership{dictionary.ownedByteCost(), dictionary.ownedElementCost()};
    });
}

static void copyPair(const void* key, std::size_t keyWords, const NativeDictionaryValue& value, void* target)
{
    auto* output = static_cast<std::uint64_t*>(target);
    std::memcpy(output, key, keyWords * sizeof(std::uint64_t));
    std::copy(value.words->begin(), value.words->end(), output + keyWords);
}

NativeDictionaryValue NativeDictionaries::capture(NativeLists& lists, const OwnedShape& shape, const void* value)
{
    const auto& key = *shape.fields[0].shape;
    const auto& mapped = *shape.fields[1].shape;
    const auto* words = static_cast<const std::uint64_t*>(value);
    NativeLists::ObjectLayout pair{0, 1};
    pair.append(NativeLists::referenceLayout(key));
    pair.append(NativeLists::referenceLayout(mapped));
    return {&lists, &mapped, std::make_shared<const std::vector<std::uint64_t>>(words, words + mapped.words),
            lists.cost(mapped, reinterpret_cast<const std::byte*>(words)), pair.finish().size};
}

DictionaryView NativeDictionaries::import(NativeLists& lists, const OwnedShape& shape, std::size_t count,
    const std::function<void(std::size_t, std::span<std::uint64_t>)>& read)
{
    if (count == 0)
        return {};
    const auto& key = *shape.fields[0].shape;
    const auto valueWords = shape.fields[1].shape->words;
    return visitCollectionKey(key, "Dictionary", [&]<class Key>() {
        model::Dictionary<Key, NativeDictionaryValue> dictionary;
        std::vector<std::uint64_t> words(key.words + valueWords);
        for (std::size_t index = 0; index < count; ++index) {
            std::fill(words.begin(), words.end(), 0);
            read(index, words);
            dictionary.add(loadCollectionKey<Key>(key, words.data(), "Dictionary"),
                           capture(lists, shape, words.data() + key.words));
        }
        if (!versions_)
            versions_ = std::make_unique<Versions>();
        return versions_->save(shape, std::move(dictionary), count * valueWords * sizeof(std::uint64_t));
    });
}

void NativeDictionaries::copyEntry(DictionaryView view, const OwnedShape& shape, std::int64_t index,
                                   void* output) const
{
    Versions::visit(versions_.get(), view, shape, [&]<class Key>(const auto& dictionary) {
        dictionary.visitEntry(index, [&](const Key& key, const NativeDictionaryValue& value) {
            visitCollectionKeyWords(key, [&](const void* words) {
                copyPair(words, shape.fields[0].shape->words, value, output);
            });
        });
    });
}

void NativeDictionaries::operate(NativeLists& lists, DictionaryOperation operation, const OwnedShape& shape,
                                 DictionaryView& view, const void* key, const void* value, const OwnedShape* projection,
                                 std::int64_t index, void* output)
{
    const auto& keyShape = *shape.fields[0].shape;
    const auto& valueShape = *shape.fields[1].shape;
    if (operation == DictionaryOperation::Iterate) {
        copyEntry(view, shape, index, output);
        return;
    }
    Versions::visit(versions_.get(), view, shape, [&]<class Key>(const auto& current) {
        if (projection) {
            ListView result{};
            std::vector<std::uint64_t> entry;
            if (operation == DictionaryOperation::Entries)
                entry.resize(projection->words);
            for (std::int64_t item = 0; item < current.count(); ++item) {
                current.visitEntry(item, [&](const Key& entryKey, const NativeDictionaryValue& mapped) {
                    if (operation == DictionaryOperation::Values) {
                        result = lists.append(result, *projection, mapped.words->data());
                        return;
                    }
                    visitCollectionKeyWords(entryKey, [&](const void* words) {
                        if (operation == DictionaryOperation::Keys) {
                            result = lists.append(result, *projection, words);
                        } else {
                            copyPair(words, keyShape.words, mapped, entry.data());
                            result = lists.append(result, *projection, entry.data());
                        }
                    });
                });
            }
            std::memcpy(output, &result, sizeof(result));
            return;
        }

        auto decoded = loadCollectionKey<Key>(keyShape, key, "Dictionary");
        auto* result = static_cast<std::uint64_t*>(output);
        switch (operation) {
        case DictionaryOperation::At: {
            const auto found = current.at(decoded);
            std::copy(found.words->begin(), found.words->end(), result);
            return;
        }
        case DictionaryOperation::TryGet: {
            const auto found = current.tryGet(decoded);
            std::fill_n(result, valueShape.words + 1, 0);
            if (found.has_value()) {
                result[0] = 1;
                const auto& mapped = found.value();
                std::copy(mapped.words->begin(), mapped.words->end(), result + 1);
            }
            return;
        }
        case DictionaryOperation::Contains:
            *result = current.containsKey(decoded);
            return;
        case DictionaryOperation::Remove: {
            *result = current.containsKey(decoded);
            if (*result == 0)
                return;
            auto updated = current;
            updated.remove(decoded);
            view = versions_->save(shape, std::move(updated), 0);
            return;
        }
        case DictionaryOperation::Add:
        case DictionaryOperation::Set: {
            auto mapped = capture(lists, shape, value);
            auto updated = current;
            if (operation == DictionaryOperation::Add)
                updated.add(std::move(decoded), std::move(mapped));
            else
                updated.set(std::move(decoded), std::move(mapped));
            if (!versions_)
                versions_ = std::make_unique<Versions>();
            view = versions_->save(shape, std::move(updated), valueShape.words * sizeof(std::uint64_t));
            return;
        }
        default:
            throw model::ExecutionFailure("invalid native Dictionary operation");
        }
    });
}

}
