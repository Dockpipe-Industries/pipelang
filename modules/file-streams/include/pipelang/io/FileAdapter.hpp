// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LinuxFiles.hpp"
#include <pipelang/crypto/StreamAdapter.hpp>
#include <pipelang/model/byte_buffer.hpp>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace pipelang::io {

// Generated record/interface parameters avoid coupling this optional adapter to
// one consumer namespace. Crypto is the selected package's real adapter/service.
template<class Contract, class Captured, class Digest, class Identity, class Failure,
         class Effect, class CryptoContract, class CryptoFailure, class CryptoDigest>
class FileAdapter final : public Contract {
    template<class Value>
    using Result = pipelang::model::Result<Value, Failure>;
    static_assert(std::is_nothrow_move_constructible_v<Result<Captured>>);

    using Token = pipelang::model::CancellationToken;
    using Bytes = pipelang::model::ByteBuffer;
    using Crypto = pipelang::crypto::StreamAdapter<CryptoContract, CryptoFailure, CryptoDigest>;
    using Reservation = detail::Reservation;

    struct Snapshot {
        std::int64_t identifier = 0;
        std::size_t size = 0;
        std::unique_ptr<Reservation> reservation;
        std::unique_ptr<std::uint8_t[]> bytes;
    };

    struct Operation {
        explicit Operation(Crypto& service) : crypto(service) {}
        ~Operation() { (void)close(); }

        Crypto& crypto;
        std::unique_ptr<Reservation> active;
        std::unique_ptr<Reservation> descriptors;
        detail::Chain parents;
        detail::Descriptor leaf;
        std::int64_t hash = 0;

        bool close() noexcept {
            bool success = true;
            if (hash != 0) {
                // This concrete adapter's Close path does not allocate.
                try {
                    if (!crypto.CloseSha256(std::exchange(hash, 0)).is_success())
                        success = false;
                } catch (...) {
                    success = false;
                }
            }
            if (!leaf.close())
                success = false;
            if (!parents.close())
                success = false;
            descriptors.reset();
            active.reset();
            return success;
        }
    };

public:
    // Composition-time root admission has no Source token. All four Source
    // methods are bounded effects; token-bearing methods refuse pre-cancellation
    // before operation reservation, allocation, path traversal, read or hash.
    FileAdapter(const std::string& absoluteRoot, PathProfile profile) : profile_(profile) {
        try {
            detail::Work work(profile_.maximumSystemCalls);
            auto names = detail::components(absoluteRoot, true, profile_);
            owner_ = std::make_unique<Reservation>(Reservation::Kind::Owner);
            rootReservation_ = std::make_unique<Reservation>(Reservation::Kind::Descriptors, names.size() + 1);
            root_.directories.reserve(names.size() + 1);
            root_.append(AT_FDCWD, "/", {}, work);
            for (const auto& name : names)
                root_.append(root_.last(), name, {}, work);

            root_.verify(AT_FDCWD, {}, work);
            crypto_ = std::make_unique<Crypto>();
        } catch (const detail::Error& failure) {
            startupError_ = failure.what();
            retireRoot();
        } catch (const std::bad_alloc&) {
            startupError_ = "io.memory_limit";
            retireRoot();
        }
    }

    ~FileAdapter() override {
        // Lifetime must outlive every call; concurrent destruction is forbidden.
        for (auto& snapshot : snapshots_) {
            snapshot.bytes.reset();
            snapshot.reservation.reset();
        }
        crypto_.reset();
        retireRoot();
    }

    FileAdapter(const FileAdapter&) = delete;
    FileAdapter& operator=(const FileAdapter&) = delete;
    FileAdapter(FileAdapter&&) = delete;
    FileAdapter& operator=(FileAdapter&&) = delete;

    Result<Captured> CaptureSnapshot(std::string relative, std::int64_t maximumBytes, Token token) override {
        if (token.is_cancelled())
            return Result<Captured>::Cancelled(error("io.cancelled"));

        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<Captured>::Failure(error("io.busy"));
        if (!startupError_.empty())
            return Result<Captured>::Failure(error(startupError_));
        if (maximumBytes < 0 || static_cast<std::uint64_t>(maximumBytes) > detail::captureLimit)
            return Result<Captured>::Failure(error("io.invalid_limit"));

        Operation operation(*crypto_);
        try {
            operation.active = std::make_unique<Reservation>(Reservation::Kind::Operation);
            auto names = detail::components(relative, false, profile_);
            operation.descriptors = std::make_unique<Reservation>(Reservation::Kind::Descriptors, names.size());
            Snapshot* selected = nullptr;
            for (auto& slot : snapshots_) {
                if (!slot.reservation) {
                    selected = &slot;
                    break;
                }
            }
            if (selected == nullptr)
                throw detail::Error("io.snapshot_limit");

            const auto extent = static_cast<std::size_t>(maximumBytes);
            auto reservation = std::make_unique<Reservation>(Reservation::Kind::Capture, extent);
            const auto identifier = detail::nextIdentifier();
            detail::checkCancelled(token);
            std::unique_ptr<std::uint8_t[]> bytes;
            if (extent != 0)
                bytes = std::make_unique<std::uint8_t[]>(extent);

            auto digest = read(operation, names, extent, bytes.get(), token);
            if (!operation.close())
                throw detail::Error("io.close_failed");

            Captured captured;
            captured.snapshotId = identifier;
            captured.file = std::move(digest);
            const auto capturedSize = static_cast<std::size_t>(captured.file.byteCount);
            auto result = Result<Captured>::Success(std::move(captured));
            detail::checkCancelled(token);

            selected->identifier = identifier;
            selected->size = capturedSize;
            selected->bytes = std::move(bytes);
            selected->reservation = std::move(reservation);
            return result;
        } catch (const detail::Error& failure) {
            return failed<Captured>(failure.what(), operation, token);
        } catch (const std::bad_alloc&) {
            return failed<Captured>("io.memory_limit", operation, token);
        }
    }

    Result<Bytes> ReadSnapshot(std::int64_t snapshot, std::int64_t offset, std::int64_t count) override {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<Bytes>::Failure(error("io.busy"));

        auto* slot = find(snapshot);
        if (slot == nullptr || !slot->reservation)
            return Result<Bytes>::Failure(error("io.invalid_snapshot"));
        if (offset < 0 || count < 0 || static_cast<std::uint64_t>(count) > pipelang::model::maximumByteBufferBytes ||
                static_cast<std::uint64_t>(offset) > slot->size ||
                static_cast<std::uint64_t>(count) > slot->size - static_cast<std::size_t>(offset))
            return Result<Bytes>::Failure(error("io.invalid_range"));

        try {
            // In particular, empty captures never perform arithmetic on null.
            if (count == 0)
                return Result<Bytes>::Success(Bytes::Empty());

            const auto* first = slot->bytes.get() + static_cast<std::size_t>(offset);
            return Result<Bytes>::Success(Bytes(std::vector<std::uint8_t>(first, first + count)));
        } catch (const std::bad_alloc&) {
            return Result<Bytes>::Failure(error("io.memory_limit"));
        }
    }

    Result<void> CloseSnapshot(std::int64_t snapshot) override {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<void>::Failure(error("io.busy"));

        auto* slot = find(snapshot);
        if (slot == nullptr)
            return Result<void>::Failure(error("io.invalid_snapshot"));

        slot->bytes.reset();
        slot->reservation.reset();
        slot->size = 0;
        // The slot keeps this ID until its next admission: at most eight bounded
        // local tombstones, never a recycled ID valid in this or another owner.
        return Result<void>::Success();
    }

    Result<Digest> DigestFile(std::string relative, std::int64_t maximumBytes, Token token) override {
        if (token.is_cancelled())
            return Result<Digest>::Cancelled(error("io.cancelled"));

        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<Digest>::Failure(error("io.busy"));
        if (!startupError_.empty())
            return Result<Digest>::Failure(error(startupError_));
        if (maximumBytes < 0 || static_cast<std::uint64_t>(maximumBytes) > detail::digestLimit)
            return Result<Digest>::Failure(error("io.invalid_limit"));

        Operation operation(*crypto_);
        try {
            operation.active = std::make_unique<Reservation>(Reservation::Kind::Operation);
            auto names = detail::components(relative, false, profile_);
            operation.descriptors = std::make_unique<Reservation>(Reservation::Kind::Descriptors, names.size());
            auto digest = read(operation, names, static_cast<std::size_t>(maximumBytes), nullptr, token);
            if (!operation.close())
                throw detail::Error("io.close_failed");

            detail::checkCancelled(token);
            return Result<Digest>::Success(std::move(digest));
        } catch (const detail::Error& failure) {
            return failed<Digest>(failure.what(), operation, token);
        } catch (const std::bad_alloc&) {
            return failed<Digest>("io.memory_limit", operation, token);
        }
    }

private:
    PathProfile profile_;
    std::mutex mutex_;
    std::unique_ptr<Reservation> owner_;
    std::unique_ptr<Reservation> rootReservation_;
    detail::Chain root_;
    std::unique_ptr<Crypto> crypto_;
    std::string startupError_;
    std::array<Snapshot, detail::snapshotSlots> snapshots_{};

    static Failure error(const std::string& code) {
        Failure failure;
        failure.code = code;
        failure.effect = Effect::NoDurableOutput;
        return failure;
    }

    template<class Value>
    static Result<Value> failed(std::string code, Operation& operation, const Token& token) {
        if (!operation.close())
            code += "+io.close_failed";

        if (token.is_cancelled())
            return Result<Value>::Cancelled(error(code));

        return Result<Value>::Failure(error(code));
    }

    void retireRoot() noexcept {
        (void)root_.close();
        rootReservation_.reset();
        owner_.reset();
    }

    Snapshot* find(std::int64_t identifier) {
        if (identifier <= 0)
            return nullptr;
        for (auto& slot : snapshots_) {
            if (slot.identifier == identifier)
                return &slot;
        }
        return nullptr;
    }

    static Identity identity(const struct stat& value) {
        Identity result;
        result.objectId = std::to_string(value.st_dev) + ":" + std::to_string(value.st_ino);
        result.revision = std::to_string(value.st_size) + ":" + std::to_string(value.st_mtim.tv_sec) + ":" +
            std::to_string(value.st_mtim.tv_nsec) + ":" + std::to_string(value.st_ctim.tv_sec) + ":" +
            std::to_string(value.st_ctim.tv_nsec);
        if (static_cast<std::uintmax_t>(value.st_nlink) >
                static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()))
            throw detail::Error("io.identity_limit");

        result.linkCount = static_cast<std::int64_t>(value.st_nlink);
        return result;
    }

    Digest read(Operation& operation, const std::vector<std::string>& names, std::size_t maximumBytes,
            std::uint8_t* capture, const Token& token) {
        detail::Work work(profile_.maximumSystemCalls);
        root_.verify(AT_FDCWD, token, work);
        operation.parents.directories.reserve(names.size() - 1);
        int parent = root_.last();
        for (std::size_t index = 0; index + 1 < names.size(); ++index) {
            operation.parents.append(parent, names[index], token, work);
            parent = operation.parents.last();
        }
        operation.parents.verify(root_.last(), token, work);

        struct stat initial{};
        detail::namedStat(parent, names.back(), initial, token, work);
        if (!S_ISREG(initial.st_mode))
            throw detail::Error("io.file_type");

        operation.leaf = detail::openAt(parent, names.back(), O_RDONLY, token, work);
        struct stat opened{};
        detail::openedStat(operation.leaf.get(), opened, token, work);
        if (!S_ISREG(opened.st_mode) || !detail::sameRevision(initial, opened))
            throw detail::Error("io.file_changed");
        if (opened.st_size < 0 || static_cast<std::uintmax_t>(opened.st_size) > maximumBytes)
            throw detail::Error("io.byte_limit");

        detail::checkCancelled(token);
        auto begun = crypto_->BeginSha256(static_cast<std::int64_t>(maximumBytes));
        if (!begun.is_success())
            throw detail::Error("io.crypto_begin");
        operation.hash = begun.value();
        detail::checkCancelled(token);
        std::size_t accepted = 0;
        for (;;) {
            // Exactly one <=64KiB vector is live. ByteBuffer takes its storage;
            // no whole-file digest allocation or geometric capture growth.
            const auto wanted = std::min(detail::chunkSize, maximumBytes - accepted);
            std::vector<std::uint8_t> block(wanted);
            std::uint8_t extra = 0;
            auto* destination = wanted == 0 ? &extra : block.data();
            const auto requested = wanted == 0 ? std::size_t{1} : wanted;
            ssize_t count;
            for (;;) {
                detail::checkCancelled(token);
                work.attempt();
                count = ::read(operation.leaf.get(), destination, requested);
                const auto failure = errno;
                detail::checkCancelled(token);
                if (count >= 0)
                    break;
                if (failure != EINTR)
                    throw detail::Error("io.read_failed");
            }
            if (count == 0)
                break;
            if (static_cast<std::size_t>(count) > maximumBytes - accepted)
                throw detail::Error("io.byte_limit");

            block.resize(static_cast<std::size_t>(count));
            if (capture != nullptr)
                std::memcpy(capture + accepted, block.data(), block.size());
            auto hashed = crypto_->UpdateSha256(operation.hash, Bytes(std::move(block)), token);
            if (!hashed.is_success())
                throw detail::Error("io.crypto_update");

            accepted += static_cast<std::size_t>(count);
            detail::checkCancelled(token);
        }

        struct stat after{};
        struct stat named{};
        detail::openedStat(operation.leaf.get(), after, token, work);
        detail::namedStat(parent, names.back(), named, token, work);
        if (!S_ISREG(named.st_mode) || !detail::sameRevision(opened, after) ||
                !detail::sameRevision(opened, named) || static_cast<std::uintmax_t>(after.st_size) != accepted)
            throw detail::Error("io.file_changed");

        operation.parents.verify(root_.last(), token, work);
        root_.verify(AT_FDCWD, token, work);
        detail::checkCancelled(token);
        auto finished = crypto_->FinishSha256(operation.hash, token);
        if (!finished.is_success())
            throw detail::Error("io.crypto_finish");
        if (finished.value().byteCount != static_cast<std::int64_t>(accepted))
            throw detail::Error("io.crypto_count");

        Digest result;
        result.byteCount = finished.value().byteCount;
        result.sha256 = finished.value().lowercaseHex;
        result.identity = identity(after);
        detail::checkCancelled(token);
        return result;
    }
};

} // namespace pipelang::io
