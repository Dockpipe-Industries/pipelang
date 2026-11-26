// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NativeCollectionKey.hpp"

#include <pipelang/model/extended_collections.hpp>

#include <variant>
#include <vector>
#include <unordered_map>

namespace pipelang::native_app_v19 {

class NativeSets {
    using Storage = std::variant<model::HashSet<std::int64_t>, model::HashSet<bool>,
                                 model::HashSet<std::string>, model::HashSet<UnsignedCollectionKey>>;

    struct Version {
        const OwnedShape* key;
        Storage set;
    };

    static constexpr std::size_t maximumArenaBytes = 32 * model::maximumListBytes;

    template<class Key>
    static const model::HashSet<Key>& typed(const Version* version)
    {
        static const auto empty = model::HashSet<Key>::Empty();
        return version ? std::get<model::HashSet<Key>>(version->set) : empty;
    }

    const Version* version(SetView view, const OwnedShape& key) const
    {
        if (view.version == 0 && view.count == 0)
            return nullptr;
        if (view.version == 0 || view.version > versions_.size())
            throw model::ExecutionFailure("invalid native HashSet handle");
        const auto& stored = versions_[view.version - 1];
        if (stored->key != &key || count(*stored) != view.count)
            throw model::ExecutionFailure("native HashSet handle does not match its key or count");
        return stored.get();
    }

    static std::uint64_t count(const Version& value)
    {
        return std::visit([](const auto& set) { return static_cast<std::uint64_t>(set.count()); }, value.set);
    }

    template<class Visitor>
    decltype(auto) visitSet(SetView view, const OwnedShape& key, const Visitor& visit) const
    {
        const auto* stored = version(view, key);
        return visitCollectionKey(key, "HashSet", [&]<class Key>() -> decltype(auto) {
            return visit.template operator()<Key>(typed<Key>(stored));
        });
    }

public:
    using Snapshot = std::shared_ptr<const Version>;

    struct Checkpoint {
        std::size_t versions;
        std::size_t bytes;
    };

    Checkpoint checkpoint() const noexcept { return {versions_.size(), bytes_}; }

    void restore(Checkpoint checkpoint) noexcept
    {
        while (versions_.size() > checkpoint.versions) {
            identities_.erase(versions_.back().get());
            versions_.pop_back();
        }
        bytes_ = checkpoint.bytes;
    }

    struct Change {
        SetView view;
        bool changed;
    };

    bool contains(SetView view, const OwnedShape& key, const void* value) const
    {
        return visitSet(view, key, [&]<class Key>(const model::HashSet<Key>& set) {
            return set.contains(loadCollectionKey<Key>(key, value, "HashSet"));
        });
    }

    Change mutate(SetView view, const OwnedShape& key, const void* value, bool adding)
    {
        return visitSet(view, key, [&]<class Key>(const model::HashSet<Key>& previous) {
            auto candidate = loadCollectionKey<Key>(key, value, "HashSet");
            if (previous.contains(candidate) == adding)
                return Change{view, false};

            auto next = previous;
            if (adding)
                next.add(std::move(candidate));
            else
                next.remove(candidate);
            if (next.count() == 0)
                return Change{{}, true};

            const auto cost = sizeof(Version) + next.ownedByteCost();
            if (cost > maximumArenaBytes - bytes_)
                throw model::ExecutionFailure("native HashSet version storage limit exceeded");
            return Change{retain(std::make_shared<const Version>(Version{&key, std::move(next)}), key), true};
        });
    }

    Snapshot snapshot(SetView view, const OwnedShape& key) const
    {
        if (!version(view, key))
            return {};
        return versions_[view.version - 1];
    }

    template<class Reader>
    SetView import(const OwnedShape& key, std::size_t size, const Reader& read)
    {
        if (size == 0)
            return {};
        return visitCollectionKey(key, "HashSet", [&]<class Key>() {
            auto set = model::HashSet<Key>::Empty();
            std::vector<std::uint64_t> words(key.words);
            for (std::size_t index = 0; index < size; ++index) {
                read(index, words.data());
                if (!set.add(loadCollectionKey<Key>(key, words.data(), "HashSet")))
                    throw model::ExecutionFailure("native hosted input HashSet contains a duplicate key");
            }
            return retain(std::make_shared<const Version>(Version{&key, std::move(set)}), key);
        });
    }

    static std::size_t snapshotBytes(const Snapshot& snapshot) noexcept
    {
        if (!snapshot)
            return 0;
        return std::visit([](const auto& set) { return set.ownedByteCost(); }, snapshot->set);
    }

    SetView retain(Snapshot snapshot, const OwnedShape& key)
    {
        if (!snapshot)
            return {};
        if (snapshot->key != &key)
            throw model::ExecutionFailure("native HashSet snapshot does not match its key");
        const auto size = count(*snapshot);
        const auto* identity = snapshot.get();
        if (const auto found = identities_.find(identity); found != identities_.end())
            return {found->second, size};
        const auto cost = sizeof(Version) + snapshotBytes(snapshot);
        if (cost > maximumArenaBytes - bytes_)
            throw model::ExecutionFailure("native HashSet version storage limit exceeded");
        versions_.push_back(std::move(snapshot));
        try {
            identities_.emplace(identity, versions_.size());
        } catch (...) {
            versions_.pop_back();
            throw;
        }
        bytes_ += cost;
        return {versions_.size(), size};
    }

    bool equal(SetView left, SetView right, const OwnedShape& key) const
    {
        const auto* other = version(right, key);
        return visitSet(left, key, [&]<class Key>(const model::HashSet<Key>& set) {
            return set == typed<Key>(other);
        });
    }

    model::ListOwnership ownership(SetView view, const OwnedShape& key) const
    {
        return visitSet(view, key, []<class Key>(const model::HashSet<Key>& set) {
            return model::ListOwnership{set.ownedByteCost(), set.ownedElementCost()};
        });
    }

    template<class Visitor>
    void visitValues(SetView view, const OwnedShape& key, const Visitor& visit) const
    {
        visitSet(view, key, [&]<class Key>(const model::HashSet<Key>& set) {
            set.visitValues([&](const Key& value) { visitCollectionKeyWords(value, visit); });
        });
    }

private:
    // SDK sets share heap-owned entries. Reallocating the version table keeps
    // those entries and borrowed text keys at their original addresses.
    std::vector<Snapshot> versions_;
    std::unordered_map<const Version*, std::uint64_t> identities_;
    std::size_t bytes_ = 0;
};

}
