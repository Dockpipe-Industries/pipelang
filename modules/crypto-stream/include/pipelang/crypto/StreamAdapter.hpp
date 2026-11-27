// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pipelang_crypto_stream.h>
#include <pipelang/model/byte_buffer.hpp>
#include <pipelang/model/cancellation.hpp>
#include <pipelang/model/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace pipelang::crypto {

// Generated Contract/Failure/Digest types keep this optional package independent
// of one consumer's generated namespace. One adapter owns one private Rust owner.
// Its fixed local ledger validates custody before any source handle reaches FFI.
template<class Contract, class Failure, class Digest>
class StreamAdapter final : public Contract {
    template<class Value>
    using Result = pipelang::model::Result<Value, Failure>;
    using Token = pipelang::model::CancellationToken;
    using Bytes = pipelang::model::ByteBuffer;

    enum class State { Empty, Active, Sealed, Closed };

    struct Slot {
        std::uint64_t context = 0;
        State state = State::Empty;
    };

public:
    StreamAdapter() {
        if (pipelang_crypto_stream_abi_version() != 2) {
            startupStatus_ = -1;
            return;
        }

        startupStatus_ = pipelang_crypto_stream_v2_owner_create(&owner_);
    }

    ~StreamAdapter() override {
        if (owner_ != 0)
            (void)pipelang_crypto_stream_v2_owner_close(owner_);
    }

    StreamAdapter(const StreamAdapter&) = delete;
    StreamAdapter& operator=(const StreamAdapter&) = delete;
    StreamAdapter(StreamAdapter&&) = delete;
    StreamAdapter& operator=(StreamAdapter&&) = delete;

    Result<std::int64_t> BeginSha256(std::int64_t maximumBytes) override {
        std::lock_guard guard(mutex_);
        constexpr auto maximumMessageBytes = std::numeric_limits<std::uint64_t>::max() / 8;
        if (maximumBytes < 0 || static_cast<std::uint64_t>(maximumBytes) > maximumMessageBytes)
            return Result<std::int64_t>::Failure(error("crypto.invalid_limit"));

        if (owner_ == 0)
            return Result<std::int64_t>::Failure(statusError(startupStatus_));

        Slot* selected = nullptr;
        for (auto& slot : slots_) {
            if (slot.state == State::Empty || slot.state == State::Closed) {
                selected = &slot;
                break;
            }
        }

        if (selected == nullptr)
            return Result<std::int64_t>::Failure(error("crypto.context_limit"));

        std::uint64_t context = 0;
        const auto status = pipelang_crypto_stream_v2_begin(owner_, static_cast<std::uint64_t>(maximumBytes), &context);
        if (status != 0) {
            observeProviderFailure(status);
            return Result<std::int64_t>::Failure(statusError(status));
        }

        if (context == 0 || context > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            (void)pipelang_crypto_stream_v2_close(owner_, context);
            return Result<std::int64_t>::Failure(error("crypto.invalid_provider_output"));
        }

        selected->context = context;
        selected->state = State::Active;
        return Result<std::int64_t>::Success(static_cast<std::int64_t>(context));
    }

    Result<void> UpdateSha256(std::int64_t hash, Bytes bytes, Token token) override {
        std::lock_guard guard(mutex_);
        auto* slot = find(hash);
        if (slot == nullptr || slot->state != State::Active)
            return Result<void>::Failure(error("crypto.invalid_context"));

        if (token.is_cancelled()) {
            discard(*slot);
            return Result<void>::Cancelled(error("crypto.cancelled"));
        }

        const auto& values = bytes.values();
        if (values.size() > pipelang::model::maximumByteBufferBytes) {
            discard(*slot);
            return Result<void>::Failure(error("crypto.chunk_limit"));
        }

        const auto status = pipelang_crypto_stream_v2_update(owner_, slot->context, values.data(), values.size());
        if (status != 0) {
            discard(*slot);
            observeProviderFailure(status);
            return Result<void>::Failure(statusError(status));
        }

        // The real provider may already have consumed every byte. Cancellation
        // after this bounded call must release state before publishing Cancelled.
        if (token.is_cancelled()) {
            discard(*slot);
            return Result<void>::Cancelled(error("crypto.cancelled"));
        }

        return Result<void>::Success();
    }

    Result<Digest> FinishSha256(std::int64_t hash, Token token) override {
        std::lock_guard guard(mutex_);
        auto* slot = find(hash);
        if (slot == nullptr || slot->state != State::Active)
            return Result<Digest>::Failure(error("crypto.invalid_context"));

        if (token.is_cancelled()) {
            discard(*slot);
            return Result<Digest>::Cancelled(error("crypto.cancelled"));
        }

        std::array<unsigned char, 32> output{};
        std::uint64_t byteCount = 0;
        const auto status = pipelang_crypto_stream_v2_finish(owner_, slot->context,
            output.data(), output.size(), &byteCount);
        if (status != 0) {
            discard(*slot);
            observeProviderFailure(status);
            return Result<Digest>::Failure(statusError(status));
        }

        if (token.is_cancelled()) {
            discard(*slot);
            return Result<Digest>::Cancelled(error("crypto.cancelled"));
        }

        if (byteCount > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            discard(*slot);
            return Result<Digest>::Failure(error("crypto.invalid_provider_output"));
        }

        try {
            constexpr char digits[] = "0123456789abcdef";
            Digest digest;
            digest.byteCount = static_cast<std::int64_t>(byteCount);
            digest.lowercaseHex.resize(64);
            for (std::size_t index = 0; index < output.size(); ++index) {
                digest.lowercaseHex[index * 2] = digits[output[index] >> 4];
                digest.lowercaseHex[index * 2 + 1] = digits[output[index] & 15];
            }

            if (token.is_cancelled()) {
                discard(*slot);
                return Result<Digest>::Cancelled(error("crypto.cancelled"));
            }

            slot->state = State::Sealed;
            return Result<Digest>::Success(std::move(digest));
        } catch (const std::bad_alloc&) {
            discard(*slot);
            return Result<Digest>::Failure(error("crypto.memory_limit"));
        }
    }

    Result<void> CloseSha256(std::int64_t hash) override {
        std::lock_guard guard(mutex_);
        auto* slot = find(hash);
        if (slot == nullptr)
            return Result<void>::Failure(error("crypto.invalid_context"));

        if (slot->state == State::Closed)
            return Result<void>::Success();

        const auto status = pipelang_crypto_stream_v2_close(owner_, slot->context);
        slot->state = State::Closed;
        if (status != 0) {
            observeProviderFailure(status);
            return Result<void>::Failure(statusError(status));
        }

        return Result<void>::Success();
    }

private:
    std::mutex mutex_;
    std::uint64_t owner_ = 0;
    std::int32_t startupStatus_ = 6;
    std::array<Slot, 64> slots_{};

    static Failure error(const char* code) {
        Failure failure;
        failure.code = code;
        return failure;
    }

    static Failure statusError(std::int32_t status) {
        switch (status) {
        case -1:
            return error("crypto.abi_version");
        case 1:
            return error("crypto.invalid_argument");
        case 2:
            return error("crypto.chunk_limit");
        case 3:
            return error("crypto.provider_unavailable");
        case 4:
            return error("crypto.owner_limit");
        case 5:
            return error("crypto.context_limit");
        case 6:
            return error("crypto.invalid_owner");
        case 7:
            return error("crypto.invalid_context");
        case 8:
            return error("crypto.invalid_state");
        case 9:
            return error("crypto.message_limit");
        case 10:
            return error("crypto.identifier_limit");
        default:
            return error("crypto.provider_failure");
        }
    }

    Slot* find(std::int64_t hash) {
        if (hash <= 0)
            return nullptr;

        for (auto& slot : slots_) {
            if (slot.state != State::Empty && slot.context == static_cast<std::uint64_t>(hash))
                return &slot;
        }

        return nullptr;
    }

    void discard(Slot& slot) {
        if (owner_ != 0)
            (void)pipelang_crypto_stream_v2_close(owner_, slot.context);

        slot.state = State::Closed;
    }

    void observeProviderFailure(std::int32_t status) {
        if (status != 3 && status != 6)
            return;

        // A caught Rust panic invalidates every owner. Do not silently create a
        // replacement and accidentally continue an uncertain consumed stream.
        owner_ = 0;
        startupStatus_ = status;
        for (auto& slot : slots_) {
            if (slot.state != State::Empty)
                slot.state = State::Closed;
        }
    }
};

} // namespace pipelang::crypto
