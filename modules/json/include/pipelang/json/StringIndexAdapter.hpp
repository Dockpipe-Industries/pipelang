// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Reader.hpp"
#include <pipelang/model/cancellation.hpp>
#include <pipelang/model/result.hpp>
#include <type_traits>
#include <utility>

namespace pipelang::json {

// Optional generated host interface. The Reader owns documents and index arenas;
// this adapter borrows it and must not outlive it. Existing JsonReader hosts are
// unchanged. Use ReaderAdapter::nativeReader() to share the same document owner.
template<class Contract, class Limits, class Index, class Node>
class StringIndexAdapter final : public Contract {
    template<class T> using Result = pipelang::model::Result<T, std::string>;
    using Token = pipelang::model::CancellationToken;

public:
    explicit StringIndexAdapter(Reader& reader) : reader_(reader) {}

    Result<Index> BeginStringIndex(std::int64_t document, Limits limits) override {
        return invoke<Index>([&] {
            const auto handle = reader_.beginStringIndex(document, {limits.entries, limits.retainedBytes});
            Index result;
            result.document = handle.document;
            result.generation = handle.generation;
            return result;
        });
    }

    Result<bool> AddString(Index index, Node node, Token token) override {
        const auto result = invoke<bool>([&] {
            return reader_.addString(handle(index), {node.document, node.node},
                                     [&] { return token.is_cancelled(); });
        });

        if (result.is_failure() && result.error() == "json.cancelled")
            return Result<bool>::Cancelled(result.error());

        return result;
    }

    // Earlier generated contracts omit this operation. When present, its
    // signature overrides the generated virtual method.
    Result<bool> ContainsString(Index index, Node node, Token token) {
        const auto result = invoke<bool>([&] {
            return reader_.containsString(handle(index), {node.document, node.node},
                                          [&] { return token.is_cancelled(); });
        });

        if (result.is_failure() && result.error() == "json.cancelled")
            return Result<bool>::Cancelled(result.error());

        return result;
    }

    Result<void> CloseStringIndex(Index index) override {
        return invoke<void>([&] { reader_.closeStringIndex(handle(index)); });
    }

private:
    Reader& reader_;

    static StringIndexHandle handle(Index index) {
        return {index.document, index.generation};
    }

    template<class T, class Function>
    static Result<T> invoke(Function&& function) {
        try {
            if constexpr (std::is_void_v<T>) {
                std::forward<Function>(function)();
                return Result<void>::Success();
            } else {
                return Result<T>::Success(std::forward<Function>(function)());
            }
        } catch (const ReadFailure& error) {
            return Result<T>::Failure(error.what());
        } catch (const std::bad_alloc&) {
            return Result<T>::Failure("json.memory_limit");
        }
    }
};
} // namespace pipelang::json
