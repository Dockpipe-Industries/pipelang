// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ByteBuffers.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

using pipelang::model::ByteBuffer;
using pipelang::model::CancellationToken;
using pipelang::model::ExecutionFailure;
using pipelang::model::Result;

namespace {
class Host final : public byte_buffers::ByteBufferHost {
public:
    Result<byte_buffers::ByteBufferPacket> Exchange(byte_buffers::ByteBufferPacket packet) override {
        if (packet.bytes.length() < static_cast<std::int64_t>(pipelang::model::maximumByteBufferBytes))
            packet.bytes.add(254);
        return Result<byte_buffers::ByteBufferPacket>::Success(std::move(packet));
    }

    Result<ByteBuffer> Read(ByteBuffer input, CancellationToken cancellation) override {
        if (cancellation.is_cancelled())
            return Result<ByteBuffer>::Cancelled();
        if (input.length() < static_cast<std::int64_t>(pipelang::model::maximumByteBufferBytes))
            input.add(255);
        return Result<ByteBuffer>::Success(std::move(input));
    }
};

template<class Callback>
void expectFailure(Callback callback) {
    bool failed = false;
    try {
        callback();
    } catch (const ExecutionFailure&) {
        failed = true;
    }
    assert(failed);
}

void valueSemantics(byte_buffers::ByteBuffers& model) {
    const ByteBuffer bytes(std::vector<std::uint8_t>{0, 255, 195, 169, 128});
    assert(model.Length(bytes) == 5);
    assert(model.At(bytes, 1) == 255);
    assert(model.Slice(bytes, 2, 1).values() == std::vector<std::uint8_t>{195});
    assert(model.Slice(bytes, 5, 0).length() == 0);
    assert(model.IndexOf(bytes, ByteBuffer(std::vector<std::uint8_t>{255, 195}), 0) == 1);
    assert(model.IndexOf(bytes, ByteBuffer::Empty(), 5) == 5);
    assert(model.IndexOf(bytes, bytes, 1) == -1);
    assert(model.Equal(bytes, bytes));
    assert(!model.Equal(bytes, ByteBuffer::Empty()));
    assert(model.Decode(model.Encode("café")) == "café");
    expectFailure([&] { (void)model.Decode(bytes); });
    expectFailure([&] { (void)model.Decode(model.Slice(model.Encode("é"), 0, 1)); });
    expectFailure([&] { (void)model.At(bytes, -1); });
    expectFailure([&] { (void)model.At(bytes, 5); });
    expectFailure([&] { (void)model.Slice(bytes, 0, INT64_MAX); });
    expectFailure([&] { (void)model.Slice(bytes, 0, -1); });
    expectFailure([&] { (void)model.IndexOf(bytes, bytes, 6); });
    const auto built = model.Build();
    assert(built.length() == 6 && built.at(0) == 0 && built.at(1) == 255);
    assert(model.Store(bytes).is_success());
    auto snapshot = model.snapshot();
    snapshot.stored.add(9);
    assert(model.snapshot().stored == bytes);
    assert(model.AppendStored(bytes).is_success());
    assert(model.snapshot().stored.length() == 10);
    const auto beforeFailure = model.snapshot().stored;
    assert(model.InvalidAdd(256).is_failure());
    assert(model.snapshot().stored == beforeFailure);
    assert(model.InvalidDecode().is_failure());
    assert(model.snapshot().stored == beforeFailure);
    assert(model.Ordered(bytes).value() == bytes.slice(1, 2));
    assert(model.snapshot().sequence == 123);
    ByteBuffer self = model.Encode("abc");
    self.append(self);
    assert(self.decodeUtf8() == "abcabc");
    ByteBuffer body(std::vector<std::uint8_t>(16384, 255));
    assert(model.Length(body) == 16384);
    ByteBuffer maximum(std::vector<std::uint8_t>(pipelang::model::maximumByteBufferBytes, 0));
    assert(model.Length(maximum) == 1048576);
    assert(pipelang::model::ownedBytes(maximum) == sizeof(ByteBuffer) + 1048576);
    expectFailure([&] { maximum.add(1); });
    expectFailure([&] { maximum.append(body); });
    expectFailure([&] { ByteBuffer invalid(std::vector<std::uint8_t>(1048577, 0)); });
    pipelang::model::List<ByteBuffer> collection;
    collection.add(body);
    assert(collection.ownedElementCost() == 1);
    expectFailure([&] { collection.add(maximum); });
}

void hostTransport(byte_buffers::ByteBuffers& model) {
    const ByteBuffer bytes(std::vector<std::uint8_t>{255, 0, 128});
    const auto exchanged = model.Exchange({bytes, 7});
    assert(exchanged.is_success());
    assert(exchanged.value().bytes.length() == 4 && exchanged.value().bytes.at(3) == 254);
    assert(bytes.length() == 3);
    auto task = model.Read(bytes, {});
    for (int attempt = 0; !task.completed() && attempt < 1000; ++attempt) {
        model.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(task.completed() && task.result().is_success());
    assert(task.result().value().length() == 4);
    assert(model.snapshot().stored == task.result().value());
    const auto beforeCancellation = model.snapshot().stored;
    pipelang::model::CancellationSource cancellation;
    cancellation.cancel();
    auto cancelled = model.Read(bytes, cancellation.token());
    for (int attempt = 0; !cancelled.completed() && attempt < 1000; ++attempt) {
        model.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(cancelled.completed() && cancelled.result().is_cancelled());
    assert(model.snapshot().stored == beforeCancellation);
    const ByteBuffer maximum(std::vector<std::uint8_t>(1048576, 255));
    const auto largePacket = model.Exchange({maximum, 9});
    assert(largePacket.is_success() && largePacket.value().bytes == maximum);
    auto largeResult = model.Read(maximum, {});
    for (int attempt = 0; !largeResult.completed() && attempt < 1000; ++attempt) {
        model.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(largeResult.completed() && largeResult.result().is_success());
    assert(largeResult.result().value() == maximum);
}
}

int main() {
    auto host = std::make_shared<Host>();
    byte_buffers::ByteBuffers model(host);
    valueSemantics(model);
    hostTransport(model);
}
