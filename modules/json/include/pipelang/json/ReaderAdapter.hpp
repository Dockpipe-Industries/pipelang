// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Reader.hpp"
#include <pipelang/model/byte_buffer.hpp>
#include <pipelang/model/cancellation.hpp>
#include <pipelang/model/result.hpp>
#include <type_traits>
#include <utility>

namespace pipelang::json {

// Instantiate with the compiler-generated interface/records/enum. No dependency
// on an application namespace or handwritten substitute for generated host ABI.
template<class Contract, class Limits, class Node, class NodeKind>
class ReaderAdapter final : public Contract {
    template<class T> using Result = pipelang::model::Result<T, std::string>;
    using Buffer = pipelang::model::ByteBuffer;
    using Token = pipelang::model::CancellationToken;
    using OptionalNode = pipelang::model::Optional<Node>;

public:
    explicit ReaderAdapter(ReaderOptions options = {}) : reader_(options) {}

    Result<std::int64_t> BeginDocument(Limits limits) override {
        return invoke<std::int64_t>([&] {
            ReadLimits selected{limits.inputBytes, limits.nodes, limits.depth,
                                limits.decodedBytes, limits.nameBytes, limits.retainedBytes};
            if constexpr (requires { limits.detectUnicodeEncoding; })
                selected.detectUnicodeEncoding = limits.detectUnicodeEncoding;

            return reader_.begin(selected);
        });
    }

    Result<void> AppendDocument(std::int64_t document, Buffer bytes) override {
        return invoke<void>([&] { reader_.append(document, bytes.values()); });
    }

    Result<Node> CompleteDocument(std::int64_t document, Token token) override {
        const auto result = invoke<Node>([&] {
            return project(reader_.complete(document, [&] { return token.is_cancelled(); }));
        });
        if (result.is_failure() && result.error() == "json.cancelled")
            return Result<Node>::Cancelled(result.error());
        return result;
    }

    Result<void> CloseDocument(std::int64_t document) override {
        return invoke<void>([&] { reader_.close(document); });
    }

    Result<NodeKind> Kind(Node node) override {
        return invoke<NodeKind>([&] { return static_cast<NodeKind>(reader_.kind(handle(node))); });
    }

    Result<std::int64_t> Count(Node node) override {
        return invoke<std::int64_t>([&] { return reader_.count(handle(node)); });
    }

    Result<Node> At(Node node, std::int64_t index) override {
        return invoke<Node>([&] { return project(reader_.at(handle(node), index)); });
    }

    Result<OptionalNode> FindMember(Node node, std::string name) override {
        return invoke<OptionalNode>([&] {
            const auto member = reader_.find(handle(node), name);
            if (member)
                return OptionalNode::Some(project(*member));
            return OptionalNode::None();
        });
    }

    Result<bool> BoolValue(Node node) override {
        return invoke<bool>([&] { return reader_.boolean(handle(node)); });
    }

    Result<std::int64_t> Int64Value(Node node) override {
        return invoke<std::int64_t>([&] { return reader_.integer(handle(node)); });
    }

    Result<double> FiniteFloatValue(Node node) override {
        return invoke<double>([&] { return reader_.finiteFloat(handle(node)); });
    }

    Result<std::int64_t> ScalarLength(Node node) override {
        return invoke<std::int64_t>([&] { return reader_.scalarLength(handle(node)); });
    }

    Result<Buffer> ScalarSlice(Node node, std::int64_t start, std::int64_t count) override {
        return invoke<Buffer>([&] { return Buffer(reader_.scalarSlice(handle(node), start, count)); });
    }

    Result<std::int64_t> NameLength(Node node) override {
        return invoke<std::int64_t>([&] { return reader_.nameLength(handle(node)); });
    }

    Result<Buffer> NameSlice(Node node, std::int64_t start, std::int64_t count) override {
        return invoke<Buffer>([&] { return Buffer(reader_.nameSlice(handle(node), start, count)); });
    }

    Result<std::int64_t> InputLength(std::int64_t document) override {
        return invoke<std::int64_t>([&] { return reader_.inputLength(document); });
    }

    Result<Buffer> InputSlice(std::int64_t document, std::int64_t start, std::int64_t count) override {
        return invoke<Buffer>([&] { return Buffer(reader_.inputSlice(document, start, count)); });
    }

    Reader& nativeReader() noexcept { return reader_; }

private:
    Reader reader_;

    static NodeHandle handle(Node node) { return {node.document, node.node}; }

    static Node project(NodeHandle handle) {
        Node result;
        result.document = handle.document;
        result.node = handle.node;
        return result;
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
