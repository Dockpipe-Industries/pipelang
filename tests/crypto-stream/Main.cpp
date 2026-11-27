// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"
#include <pipelang/crypto/StreamAdapter.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace api = pipelang_crypto_tests::source::PipeLang::Crypto;
namespace tests = pipelang_crypto_tests::source::PipeLang::Crypto::Tests;
using Adapter = pipelang::crypto::StreamAdapter<api::Sha256Streams, api::CryptoFailure, api::Sha256Digest>;
using Token = pipelang::model::CancellationToken;
using Bytes = pipelang::model::ByteBuffer;
template<class Value>
using FixtureResult = pipelang::model::Result<Value, std::string>;

namespace {

struct ProviderObservations {
    std::int64_t updates = 0;
    std::int64_t finishes = 0;
    const pipelang::model::CancellationSource* cancelAfterUpdate = nullptr;
    const pipelang::model::CancellationSource* cancelAfterFinish = nullptr;
};

// Selected by this one synchronous fixture. Wrappers unconditionally delegate
// exactly once to the actual linked maintained provider; they change only real
// cancellation state and mechanical call counts after that provider returns.
ProviderObservations* observations = nullptr;

void cancelAfter(const pipelang::model::CancellationSource*& source) {
    if (source == nullptr)
        return;

    source->cancel();
    source = nullptr;
}

class Peer final : public api::Sha256Streams {
public:
    Peer() : current_(std::make_unique<Adapter>()) {}

    void recreate() {
        // Release the real owner's 64 contexts before constructing its successor.
        current_.reset();
        current_ = std::make_unique<Adapter>();
    }

    pipelang::model::Result<std::int64_t, api::CryptoFailure> BeginSha256(std::int64_t maximumBytes) override {
        return current_->BeginSha256(maximumBytes);
    }

    pipelang::model::Result<void, api::CryptoFailure> UpdateSha256(std::int64_t hash, Bytes bytes, Token token) override {
        return current_->UpdateSha256(hash, std::move(bytes), std::move(token));
    }

    pipelang::model::Result<api::Sha256Digest, api::CryptoFailure> FinishSha256(std::int64_t hash, Token token) override {
        return current_->FinishSha256(hash, std::move(token));
    }

    pipelang::model::Result<void, api::CryptoFailure> CloseSha256(std::int64_t hash) override {
        return current_->CloseSha256(hash);
    }

private:
    std::unique_ptr<Adapter> current_;
};

class Inputs final : public tests::CryptoInputs {
public:
    explicit Inputs(std::shared_ptr<Peer> peer) : peer_(std::move(peer)) {
        observations = &observed_;
    }

    ~Inputs() override {
        observations = nullptr;
    }

    Token probe(std::size_t scope) const {
        return cancellation_.at(scope).token();
    }

    FixtureResult<Bytes> Slice(std::int64_t fixture, std::int64_t offset, std::int64_t count) override {
        const auto extent = length(fixture);
        if (extent < 0 || offset < 0 || count < 0 || offset > extent || count > extent - offset ||
                static_cast<std::uint64_t>(count) > pipelang::model::maximumByteBufferBytes)
            return FixtureResult<Bytes>::Failure("fixture.range");

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
        const auto literal = text(fixture);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const auto position = static_cast<std::uint64_t>(offset) + index;
            if (fixture == 3 || fixture == 6) {
                bytes[index] = 'a';
            } else if (fixture == 4 || fixture == 7) {
                bytes[index] = static_cast<std::uint8_t>(position & 255);
            } else {
                bytes[index] = static_cast<std::uint8_t>(literal[static_cast<std::size_t>(position)]);
            }
        }

        return FixtureResult<Bytes>::Success(Bytes(std::move(bytes)));
    }

    FixtureResult<void> CancelProbe(std::int64_t scope) override {
        if (!validScope(scope))
            return FixtureResult<void>::Failure("fixture.scope");

        cancellation_[static_cast<std::size_t>(scope)].cancel();
        return FixtureResult<void>::Success();
    }

    FixtureResult<void> CancelAfterProviderCall(std::int64_t scope, bool finish) override {
        if (!validScope(scope) || observed_.cancelAfterUpdate != nullptr || observed_.cancelAfterFinish != nullptr)
            return FixtureResult<void>::Failure("fixture.scope");

        const auto* source = &cancellation_[static_cast<std::size_t>(scope)];
        if (finish)
            observed_.cancelAfterFinish = source;
        else
            observed_.cancelAfterUpdate = source;

        return FixtureResult<void>::Success();
    }

    FixtureResult<std::int64_t> UpdateCalls() override {
        return FixtureResult<std::int64_t>::Success(observed_.updates);
    }

    FixtureResult<std::int64_t> FinishCalls() override {
        return FixtureResult<std::int64_t>::Success(observed_.finishes);
    }

    FixtureResult<void> RecreatePeer() override {
        peer_->recreate();
        return FixtureResult<void>::Success();
    }

private:
    std::shared_ptr<Peer> peer_;
    std::array<pipelang::model::CancellationSource, 4> cancellation_;
    ProviderObservations observed_;

    bool validScope(std::int64_t scope) const {
        return scope >= 0 && static_cast<std::uint64_t>(scope) < cancellation_.size();
    }

    static std::int64_t length(std::int64_t fixture) {
        switch (fixture) {
        case 0:
            return 0;
        case 1:
            return 3;
        case 2:
            return 56;
        case 3:
            return 1000000;
        case 4:
            return 256;
        case 5:
            return 112;
        case 6:
            return 536870912;
        case 7:
            return 1048576;
        default:
            return -1;
        }
    }

    static std::string_view text(std::int64_t fixture) {
        switch (fixture) {
        case 1:
            return "abc";
        case 2:
            return "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        case 5:
            return "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                   "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        default:
            return {};
        }
    }
};

std::string json(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char byte : text) {
        if (byte == '"' || byte == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(byte));
        } else if (byte < 0x20) {
            result += "\\u00";
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 15]);
        } else {
            result.push_back(static_cast<char>(byte));
        }
    }

    result.push_back('"');
    return result;
}

template<class OptionalFailure>
void writeFailure(const OptionalFailure& failure) {
    if (!failure.has_value()) {
        std::cout << "null";
        return;
    }

    std::cout << "{\"code\":" << json(failure.value().code)
              << ",\"message\":" << json(failure.value().message) << '}';
}

} // namespace

extern "C" std::int32_t __real_pipelang_crypto_stream_v2_update(std::uint64_t owner,
    std::uint64_t context, const unsigned char* input, std::size_t length);
extern "C" std::int32_t __real_pipelang_crypto_stream_v2_finish(std::uint64_t owner,
    std::uint64_t context, unsigned char* output, std::size_t length, std::uint64_t* count);

extern "C" std::int32_t __wrap_pipelang_crypto_stream_v2_update(std::uint64_t owner,
    std::uint64_t context, const unsigned char* input, std::size_t length) {
    const auto status = __real_pipelang_crypto_stream_v2_update(owner, context, input, length);
    if (observations != nullptr) {
        ++observations->updates;
        cancelAfter(observations->cancelAfterUpdate);
    }

    return status;
}

extern "C" std::int32_t __wrap_pipelang_crypto_stream_v2_finish(std::uint64_t owner,
    std::uint64_t context, unsigned char* output, std::size_t length, std::uint64_t* count) {
    const auto status = __real_pipelang_crypto_stream_v2_finish(owner, context, output, length, count);
    if (observations != nullptr) {
        ++observations->finishes;
        cancelAfter(observations->cancelAfterFinish);
    }

    return status;
}

int main() {
    try {
        auto primary = std::make_shared<Adapter>();
        auto peer = std::make_shared<Peer>();
        auto inputs = std::make_shared<Inputs>(peer);
        // Explicit existing predicate profile for this finite whole-suite fixture.
        // Source cases remain one invocation; initialization/cleanup stay 65536.
        pipelang::model::TaskOptions options;
        options.evaluationStepsPerSegment = 33554432;
        tests::CryptoSuite suite(primary, peer, inputs, options);
        const auto evaluated = suite.Run({}, inputs->probe(0), inputs->probe(1), inputs->probe(2), inputs->probe(3));
        if (!evaluated.is_success())
            throw std::runtime_error(evaluated.error());

        const auto& report = evaluated.value();
        const pipelang::model::TaskOptions defaults;
        std::cout << "{\"schema\":\"pipelang.crypto-source-contracts/v1\",\"allPassed\":"
                  << (report.allPassed ? "true" : "false")
                  << ",\"profile\":{\"evaluationStepsPerSegment\":" << options.evaluationStepsPerSegment
                  << ",\"initializationSteps\":" << options.initializationSteps
                  << ",\"defaultEvaluationStepsPerSegment\":" << defaults.evaluationStepsPerSegment
                  << ",\"cleanupSteps\":" << pipelang::model::EvaluationBudget::maximumSteps
                  << "},\"results\":[";
        bool first = true;
        for (const auto& result : report.results.values()) {
            if (!first)
                std::cout << ',';

            first = false;
            std::cout << "{\"id\":" << json(result.id)
                      << ",\"name\":" << json(result.name)
                      << ",\"outcome\":" << static_cast<std::int64_t>(result.outcome)
                      << ",\"primaryOutcome\":" << static_cast<std::int64_t>(result.primaryOutcome)
                      << ",\"primaryPhase\":" << static_cast<std::int64_t>(result.primaryPhase)
                      << ",\"primaryFailure\":";
            writeFailure(result.primaryFailure);
            std::cout << ",\"cleanupOutcome\":" << static_cast<std::int64_t>(result.cleanupOutcome)
                      << ",\"cleanupFailure\":";
            writeFailure(result.cleanupFailure);
            std::cout << ",\"bodyRan\":" << (result.bodyRan ? "true" : "false")
                      << ",\"cleanupRan\":" << (result.cleanupRan ? "true" : "false") << '}';
        }

        std::cout << "]}\n";

        return report.allPassed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Crypto source consumer infrastructure: " << error.what() << '\n';
        return 2;
    }
}
