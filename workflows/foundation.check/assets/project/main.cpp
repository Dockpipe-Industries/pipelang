// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <foundation_iteration/CompositionFactory.hpp>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <vector>

using Startup = foundation_iteration::composition::Foundation::Composition::Startup;
using pipelang::model::ByteBuffer;

int main() {
    const auto created = Startup::Create();
    assert(created.is_success());
    const auto root = created.value();
    const auto scoped = root->CreateScope();
    assert(scoped.is_success());
    const auto scope = scoped.value();
    const auto resolved = scope->ResolveByteBufferIteration();
    assert(resolved.is_success());
    auto model = resolved.value();
    std::vector<std::uint8_t> values;
    for (unsigned value = 0; value < 256; ++value) {
        values.push_back(static_cast<std::uint8_t>(value));
        const ByteBuffer single(std::vector<std::uint8_t>{static_cast<std::uint8_t>(value)});
        const auto typed = model.Typed(single);
        const auto complement = model.Complement(single);
        assert(typed.is_success() && typed.value() == static_cast<std::int64_t>(value) + 1);
        assert(complement.is_success() && complement.value() == ~static_cast<std::uint32_t>(value));
    }
    const auto sum = model.Sum(ByteBuffer(values));
    assert(sum.is_success() && sum.value() == 32640);
    const ByteBuffer input(std::vector<std::uint8_t>{0, 1, 128, 255, 9});
    const auto control = model.Control(input);
    const auto snapshot = model.Snapshot(input);
    const auto read = model.ReceiverOnce();
    assert(control.is_success() && control.value() == 129);
    assert(snapshot.is_success() && snapshot.value() == 393);
    assert(read.is_success() && read.value() == 438);
    const auto exhausted = model.Sum(ByteBuffer(std::vector<std::uint8_t>(65536, 1)));
    assert(exhausted.is_failure() && exhausted.error() == "source evaluation budget exceeded");
    assert(scope->Close().is_success());
    assert(root->Close().is_success());
}
