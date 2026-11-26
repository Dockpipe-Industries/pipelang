// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ByteBufferIteration.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <vector>

using pipelang::model::ByteBuffer;

int main() {
    byte_buffer_iteration::ByteBufferIteration model;
    assert(model.Sum(ByteBuffer::Empty()) == 0);
    assert(model.Typed(ByteBuffer::Empty()) == 0);
    std::vector<std::uint8_t> allBytes;
    for (unsigned value = 0; value < 256; ++value) {
        allBytes.push_back(static_cast<std::uint8_t>(value));
        const ByteBuffer single(std::vector<std::uint8_t>{static_cast<std::uint8_t>(value)});
        assert(model.Sum(single) == static_cast<std::int64_t>(value));
        assert(model.Typed(single) == static_cast<std::int64_t>(value) + 1);
        assert(model.Complement(single) == ~static_cast<std::uint32_t>(value));
    }
    const ByteBuffer corpus(allBytes);
    assert(model.Sum(corpus) == 32640);
    const ByteBuffer bytes(std::vector<std::uint8_t>{0, 1, 128, 255, 9});
    assert(model.Control(bytes) == 129);
    const auto changed = model.Snapshot(bytes);
    assert(changed.is_success() && changed.value() == 393);
    assert(bytes.length() == 5);
    const auto snapshot = model.snapshot();
    assert(snapshot.stored.length() == 10);
    assert(snapshot.stored.slice(0, 5) == bytes);
    assert(snapshot.stored.slice(5, 5).values() == std::vector<std::uint8_t>({9, 9, 9, 9, 9}));
    const auto read = model.ReceiverOnce();
    assert(read.is_success() && read.value() == 438);
    assert(model.snapshot().reads == 1);
    assert(model.Nested(ByteBuffer(std::vector<std::uint8_t>{1, 2}),
        ByteBuffer(std::vector<std::uint8_t>{3, 4})) == 20);
    bool exhausted = false;
    try {
        (void)model.Sum(ByteBuffer(std::vector<std::uint8_t>(65536, 1)));
    } catch (const pipelang::model::EvaluationLimitFailure&) {
        exhausted = true;
    }
    assert(exhausted);
    assert(model.snapshot().stored == snapshot.stored);
}
