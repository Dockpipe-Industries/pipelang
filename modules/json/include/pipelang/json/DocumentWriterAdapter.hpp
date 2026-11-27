// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DocumentWriter.hpp"

#include <pipelang/model/cancellation.hpp>
#include <pipelang/model/result.hpp>

#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pipelang::json {

template<class Contract, class Node>
class DocumentWriterAdapter final : public Contract {
    template<class T> using Result = pipelang::model::Result<T, std::string>;
    using Token = pipelang::model::CancellationToken;

public:
    DocumentWriterAdapter(Reader& reader, RenderOptions options, JsonByteSink sink)
        : owned_(std::in_place, reader, options, std::move(sink)), writer_(*owned_) {}

    explicit DocumentWriterAdapter(DocumentWriter& writer) : writer_(writer) {}

    Result<void> BeginObject() override {
        return invoke<void>([&] { writer_.beginObject(); });
    }

    Result<void> BeginArray() override {
        return invoke<void>([&] { writer_.beginArray(); });
    }

    Result<void> Name(std::string key) override {
        return invoke<void>([&] { writer_.name(key); });
    }

    Result<void> EndObject() override {
        return invoke<void>([&] { writer_.endObject(); });
    }

    Result<void> EndArray() override {
        return invoke<void>([&] { writer_.endArray(); });
    }

    Result<void> WriteText(std::string value) override {
        return invoke<void>([&] { writer_.text(value); });
    }

    Result<void> WriteInteger(std::int64_t value) override {
        return invoke<void>([&] { writer_.integer(value); });
    }

    Result<void> WriteFinite(double value) override {
        return invoke<void>([&] { writer_.finite(value); });
    }

    Result<void> WriteBoolean(bool value) override {
        return invoke<void>([&] { writer_.boolean(value); });
    }

    Result<void> WriteNull() override {
        return invoke<void>([&] { writer_.nullValue(); });
    }

    Result<void> WriteValue(Node value, Token token) override {
        return invoke<void>([&] {
            writer_.borrowed({value.document, value.node}, [&] { return token.is_cancelled(); });
        });
    }

    Result<std::int64_t> Finish() override {
        return invoke<std::int64_t>([&] { return static_cast<std::int64_t>(writer_.finish()); });
    }

private:
    std::optional<DocumentWriter> owned_;
    DocumentWriter& writer_;

    template<class T, class Operation>
    static Result<T> invoke(Operation&& operation) {
        try {
            if constexpr (std::is_void_v<T>) {
                std::forward<Operation>(operation)();
                return Result<void>::Success();
            } else {
                return Result<T>::Success(std::forward<Operation>(operation)());
            }
        } catch (const ReadFailure& error) {
            if (std::string_view(error.what()) == "json.cancelled")
                return Result<T>::Cancelled(error.what());

            return Result<T>::Failure(error.what());
        } catch (const std::bad_alloc&) {
            return Result<T>::Failure("json.memory_limit");
        }
    }
};

} // namespace pipelang::json
