// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>
#include <pipelang/model/byte_buffer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace pipelang::native_app_v19;
using pipelang::model::Result;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_hosted_byte_streaming;

namespace {

std::size_t chunkSize(std::int64_t index, bool large)
{
    if (large)
        return pipelang::model::maximumByteBufferBytes / (index % 2 == 0 ? 1 : 2);
    return 21 + static_cast<std::size_t>(index);
}

class ChunkPort final : public HostedService {
public:
    using Response = Result<HostedServiceValue>;

    std::string_view interfaceName() const noexcept override
    {
        return "NativeProof.HostedByteStreaming.ChunkPort";
    }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        const auto index = std::get<std::int64_t>(arguments[0]);
        if (index < 0 || index >= 84)
            return Response::Failure("fixture.chunk_index");
        const auto marker = static_cast<std::uint8_t>(index + 1);
        if (signature == "Read(int,bool)") {
            if (index == 17)
                return Response::Failure("chunk.failed");
            if (index == 18)
                return Response::Cancelled("chunk.cancelled");
            const auto size = chunkSize(index, std::get<bool>(arguments[1]));
            const std::vector<std::uint8_t> bytes(size, marker);
            return Response::Success(HostedValue::fromBytes(bytes));
        }
        if (signature == "ReadTyped(int)") {
            using pipelang::model::Outcome;
            constexpr auto shape = "Result<ByteBuffer,NativeProof.HostedByteStreaming.ChunkError>";
            if (index == 17 || index == 18) {
                const std::array<std::uint8_t, 2> details{static_cast<std::uint8_t>(index), 0};
                const auto error = HostedValue::fromRecord("NativeProof.HostedByteStreaming.ChunkError", {
                    {"code", HostedValue::fromText(index == 17 ? "chunk.failed" : "chunk.cancelled")},
                    {"details", HostedValue::fromBytes(details)}
                });
                return Response::Success(HostedValue::fromResult(shape,
                    index == 17 ? Outcome::Failure : Outcome::Cancelled, error));
            }
            const std::vector<std::uint8_t> bytes(chunkSize(index, true), marker);
            return Response::Success(HostedValue::fromResult(shape, Outcome::Success, HostedValue::fromBytes(bytes)));
        }
        if (signature == "Consume(int,ByteBuffer)") {
            const auto bytes = std::get<HostedValue>(arguments[1]).bytes();
            if (bytes.size() != chunkSize(index, true) ||
                !std::ranges::all_of(bytes, [marker](std::uint8_t byte) { return byte == marker; }))
                return Response::Failure("fixture.chunk_changed");

            ++consumed;
            return Response::Success(HostedValue{});
        }
        return Response::Failure("fixture.unknown_operation");
    }

    std::size_t consumed = 0;
};

}

int main()
{
    const auto port = std::make_shared<ChunkPort>();
    const std::array<HostedConstructorArgument, 1> constructor{port};
    const auto session = HostedInstance::bind(proof_hosted_byte_streaming, constructor);
    if (!session.is_success()) {
        std::cerr << session.error() << '\n';
        return 1;
    }

    std::size_t expectedBytes = 0;
    for (std::int64_t index = 0; index < 84; ++index) {
        if (index != 17 && index != 18)
            expectedBytes += chunkSize(index, true);
    }

    TaskScope scope;
    for (const auto scenario : {0, 1, 2, 3}) {
        const std::array<HostedArgument, 1> arguments{std::int64_t{scenario}};
        const auto task = session.value().start(scope, arguments);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!task.completed()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "hosted stream timed out\n";
                return 2;
            }
            scope.poll();
            if (!task.completed())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto evaluated = task.result();
        if (!evaluated.is_success()) {
            std::cerr << evaluated.error() << '\n';
            return 2;
        }
        const auto expected = scenario == 2 ? 42 : static_cast<std::int64_t>(expectedBytes);
        if (evaluated.value() != expected)
            return 3;
    }
    if (port->consumed != 246)
        return 4;

    std::cout << "Hosted streams preserve changing chunks, typed outcomes and escaped aliases\n";
}
