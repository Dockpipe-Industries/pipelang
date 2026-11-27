// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LinuxFiles.hpp"
#include <pipelang/crypto/StreamAdapter.hpp>
#include <pipelang/model/byte_buffer.hpp>

#include <memory>
#include <new>

namespace pipelang::io {

template<class Contract, class Digest, class Identity, class Failure, class Effect,
         class CryptoContract, class CryptoFailure, class CryptoDigest>
class ExclusiveOutputAdapter final : public Contract {
    template<class Value>
    using Result = pipelang::model::Result<Value, Failure>;
    static_assert(std::is_nothrow_move_constructible_v<Result<Digest>>);

    using Token = pipelang::model::CancellationToken;
    using Bytes = pipelang::model::ByteBuffer;
    using Crypto = pipelang::crypto::StreamAdapter<CryptoContract, CryptoFailure, CryptoDigest>;
    using Reservation = detail::Reservation;

    struct Pending {
        Pending(Crypto& service, std::vector<std::string> components, std::size_t capacity,
                std::size_t maximumSystemCalls)
            : crypto(service), names(std::move(components)), work(maximumSystemCalls),
              maximumBytes(capacity), operation(Reservation::Kind::Operation),
              descriptors(Reservation::Kind::Descriptors, names.size()) {}

        ~Pending() { (void)close(); }

        Crypto& crypto;
        std::vector<std::string> names;
        detail::Work work;
        std::size_t maximumBytes;
        Reservation operation;
        Reservation descriptors;
        detail::Chain parents;
        detail::Descriptor leaf;
        struct stat revision{};
        std::int64_t hash = 0;
        std::size_t accepted = 0;
        Effect effect = Effect::NoDurableOutput;

        bool close() noexcept {
            bool succeeded = true;
            if (hash != 0) {
                try {
                    if (!crypto.CloseSha256(std::exchange(hash, 0)).is_success())
                        succeeded = false;
                } catch (...) {
                    succeeded = false;
                }
            }
            if (!leaf.close())
                succeeded = false;
            if (!parents.close())
                succeeded = false;
            return succeeded;
        }
    };

    struct Slot {
        std::int64_t id = 0;
        Effect effect = Effect::NoDurableOutput;
        std::unique_ptr<Pending> pending;
    };

public:
    ExclusiveOutputAdapter(const std::string& absoluteRoot, PathProfile profile,
                           std::size_t maximumOutputBytes = detail::captureLimit)
        : profile_(profile), maximumOutputBytes_(maximumOutputBytes) {
        try {
            if (maximumOutputBytes_ > 32 * 1024 * 1024)
                throw detail::Error("io.invalid_profile");

            detail::Work work(profile.maximumSystemCalls);
            const auto names = detail::components(absoluteRoot, true, profile);
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

    ~ExclusiveOutputAdapter() override {
        // Destruction requires that the composition owner has retired calls.
        for (auto& slot : slots_)
            slot.pending.reset();

        crypto_.reset();
        retireRoot();
    }

    ExclusiveOutputAdapter(const ExclusiveOutputAdapter&) = delete;
    ExclusiveOutputAdapter& operator=(const ExclusiveOutputAdapter&) = delete;

    Result<std::int64_t> BeginExclusiveOutput(std::string relative, std::int64_t maximumBytes, Token token) override {
        if (token.is_cancelled())
            return Result<std::int64_t>::Cancelled(error("io.cancelled", Effect::NoDurableOutput));

        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<std::int64_t>::Failure(error("io.busy", Effect::NoDurableOutput));
        if (!startupError_.empty())
            return Result<std::int64_t>::Failure(error(startupError_, Effect::NoDurableOutput));
        if (maximumBytes < 0 || static_cast<std::uint64_t>(maximumBytes) > maximumOutputBytes_)
            return Result<std::int64_t>::Failure(error("io.invalid_limit", Effect::NoDurableOutput));

        Slot* selected = freeSlot();
        if (selected == nullptr)
            return Result<std::int64_t>::Failure(error("io.output_limit", Effect::NoDurableOutput));

        std::unique_ptr<Pending> pending;
        try {
            auto names = detail::components(relative, false, profile_);
            pending = std::make_unique<Pending>(*crypto_, std::move(names),
                static_cast<std::size_t>(maximumBytes), profile_.maximumSystemCalls);
            const auto identifier = detail::nextIdentifier();
            prepare(*pending, token);
            create(*pending, token);

            selected->id = identifier;
            selected->effect = Effect::OwnedPartial;
            selected->pending = std::move(pending);
            return Result<std::int64_t>::Success(identifier);
        } catch (const detail::Error& failure) {
            return failedBegin(failure.what(), pending, token);
        } catch (const std::bad_alloc&) {
            return failedBegin("io.memory_limit", pending, token);
        }
    }

    Result<void> AppendOutput(std::int64_t output, Bytes bytes, Token token) override {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<void>::Failure(error("io.busy", Effect::Unknown));

        Slot* slot = find(output);
        if (slot == nullptr || !slot->pending)
            return Result<void>::Failure(error("io.invalid_output", terminalEffect(slot)));

        try {
            Pending& pending = *slot->pending;
            detail::checkCancelled(token);
            const auto& data = bytes.values();
            if (data.size() > pending.maximumBytes - pending.accepted)
                throw detail::Error("io.byte_limit");

            append(pending, data, token);
            if (!data.empty()) {
                auto hashed = crypto_->UpdateSha256(pending.hash, std::move(bytes), token);
                if (!hashed.is_success())
                    throw detail::Error("io.crypto_update");
            }

            detail::checkCancelled(token);
            return Result<void>::Success();
        } catch (const detail::Error& failure) {
            return failed<void>(failure.what(), *slot, token);
        } catch (const std::bad_alloc&) {
            return failed<void>("io.memory_limit", *slot, token);
        }
    }

    Result<Digest> CommitOutput(std::int64_t output, Token token) override {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<Digest>::Failure(error("io.busy", Effect::Unknown));

        Slot* slot = find(output);
        if (slot == nullptr || !slot->pending)
            return Result<Digest>::Failure(error("io.invalid_output", terminalEffect(slot)));

        try {
            Pending& pending = *slot->pending;
            detail::checkCancelled(token);
            auto result = Result<Digest>::Success(commit(pending, token));
            if (!retire(*slot, Effect::Unknown))
                return failed<Digest>("io.close_failed", *slot, token);

            detail::checkCancelled(token);
            slot->effect = Effect::Committed;
            return result;
        } catch (const detail::Error& failure) {
            return failed<Digest>(failure.what(), *slot, token);
        } catch (const std::bad_alloc&) {
            return failed<Digest>("io.memory_limit", *slot, token);
        }
    }

    Result<Effect> CloseOutput(std::int64_t output) override {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<Effect>::Failure(error("io.busy", Effect::Unknown));

        Slot* slot = find(output);
        if (slot == nullptr)
            return Result<Effect>::Failure(error("io.invalid_output", Effect::NoDurableOutput));
        if (slot->pending && !retire(*slot, Effect::Unknown))
            return Result<Effect>::Failure(error("io.close_failed", Effect::Unknown));

        return Result<Effect>::Success(slot->effect);
    }

private:
    PathProfile profile_;
    std::size_t maximumOutputBytes_;
    std::mutex mutex_;
    std::unique_ptr<Reservation> owner_;
    std::unique_ptr<Reservation> rootReservation_;
    detail::Chain root_;
    std::unique_ptr<Crypto> crypto_;
    std::string startupError_;
    std::array<Slot, detail::operationSlots> slots_{};

    void retireRoot() noexcept {
        (void)root_.close();
        rootReservation_.reset();
        owner_.reset();
    }

    Slot* freeSlot() {
        for (auto& slot : slots_) {
            if (!slot.pending)
                return &slot;
        }
        return nullptr;
    }

    Slot* find(std::int64_t identifier) {
        if (identifier <= 0)
            return nullptr;

        for (auto& slot : slots_) {
            if (slot.id == identifier)
                return &slot;
        }
        return nullptr;
    }

    static Effect terminalEffect(const Slot* slot) {
        if (slot == nullptr)
            return Effect::NoDurableOutput;

        return slot->effect;
    }

    static Failure error(std::string code, Effect effect) {
        Failure failure;
        failure.code = std::move(code);
        failure.effect = effect;
        return failure;
    }

    static bool retire(Slot& slot, Effect effect) noexcept {
        bool closed = true;
        if (slot.pending) {
            closed = slot.pending->close();
            slot.pending.reset();
        }

        slot.effect = effect;
        return closed;
    }

    template<class Value>
    static Result<Value> failed(std::string code, Slot& slot, const Token& token) {
        if (!retire(slot, Effect::Unknown))
            code += "+io.close_failed";
        if (token.is_cancelled())
            return Result<Value>::Cancelled(error(std::move(code), slot.effect));

        return Result<Value>::Failure(error(std::move(code), slot.effect));
    }

    static Result<std::int64_t> failedBegin(std::string code, std::unique_ptr<Pending>& pending,
                                            const Token& token) {
        auto effect = Effect::NoDurableOutput;
        if (pending) {
            effect = pending->effect;
            if (!pending->close())
                code += "+io.close_failed";
            pending.reset();
        }
        if (token.is_cancelled())
            return Result<std::int64_t>::Cancelled(error(std::move(code), effect));

        return Result<std::int64_t>::Failure(error(std::move(code), effect));
    }

    void prepare(Pending& pending, const Token& token) {
        root_.verify(AT_FDCWD, token, pending.work);
        pending.parents.directories.reserve(pending.names.size() - 1);
        int parent = root_.last();
        for (std::size_t index = 0; index + 1 < pending.names.size(); ++index) {
            pending.parents.append(parent, pending.names[index], token, pending.work);
            parent = pending.parents.last();
        }

        pending.parents.verify(root_.last(), token, pending.work);
        detail::checkCancelled(token);
        auto begun = crypto_->BeginSha256(static_cast<std::int64_t>(pending.maximumBytes));
        if (!begun.is_success())
            throw detail::Error("io.crypto_begin");

        pending.hash = begun.value();
    }

    int parent(const Pending& pending) const {
        if (pending.parents.directories.empty())
            return root_.last();

        return pending.parents.last();
    }

    template<class Function>
    static void checkedSystemCall(detail::Work& work, const Token& token,
            const char* failureCode, Function&& invoke) {
        for (;;) {
            detail::checkCancelled(token);
            work.attempt();
            const auto result = invoke();
            const auto failure = errno;
            detail::checkCancelled(token);
            if (result == 0)
                return;
            if (failure != EINTR)
                throw detail::Error(failureCode);
        }
    }

    void create(Pending& pending, const Token& token) {
        const int directory = parent(pending);
        detail::checkCancelled(token);
        pending.work.attempt();
        const auto fd = ::openat(directory, pending.names.back().c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        const auto failure = errno;
        if (fd < 0) {
            if (failure == EEXIST)
                throw detail::Error("io.output_exists");

            // A failed creating syscall may have left a name with no
            // descriptor receipt. Do not repeat a potentially creating call.
            pending.effect = Effect::Unknown;
            throw detail::Error("io.create_failed");
        }

        pending.leaf = detail::Descriptor(fd);
        pending.effect = Effect::Unknown;
        checkedSystemCall(pending.work, token, "io.output_mode", [&] { return ::fchmod(fd, 0600); });
        detail::openedStat(fd, pending.revision, token, pending.work);
        if (!privateRegular(pending.revision) || pending.revision.st_size != 0)
            throw detail::Error("io.output_changed");

        verifyNamed(pending, token);
    }

    static bool privateRegular(const struct stat& metadata) {
        return S_ISREG(metadata.st_mode) && (metadata.st_mode & 07777) == 0600 &&
            metadata.st_nlink == 1;
    }

    void verifyNamed(Pending& pending, const Token& token) {
        struct stat opened{};
        struct stat named{};
        detail::openedStat(pending.leaf.get(), opened, token, pending.work);
        detail::namedStat(parent(pending), pending.names.back(), named, token, pending.work);
        if (!privateRegular(opened) || !privateRegular(named) ||
                !detail::sameRevision(pending.revision, opened) || !detail::sameRevision(opened, named))
            throw detail::Error("io.output_changed");

        pending.parents.verify(root_.last(), token, pending.work);
        root_.verify(AT_FDCWD, token, pending.work);
    }

    template<class Data>
    void append(Pending& pending, const Data& data, const Token& token) {
        verifyNamed(pending, token);
        std::size_t offset = 0;
        while (offset < data.size()) {
            detail::checkCancelled(token);
            pending.work.attempt();
            const auto count = ::write(pending.leaf.get(), data.data() + offset, data.size() - offset);
            const auto failure = errno;
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                pending.accepted += static_cast<std::size_t>(count);
            }
            detail::checkCancelled(token);
            if (count < 0 && failure != EINTR)
                throw detail::Error("io.write_failed");
            if (count == 0)
                throw detail::Error("io.write_progress");
        }

        struct stat updated{};
        detail::openedStat(pending.leaf.get(), updated, token, pending.work);
        if (!privateRegular(updated) || !detail::sameObject(pending.revision, updated) ||
                updated.st_size != static_cast<off_t>(pending.accepted))
            throw detail::Error("io.output_changed");

        pending.revision = updated;
        verifyNamed(pending, token);
    }

    Digest commit(Pending& pending, const Token& token) {
        verifyNamed(pending, token);
        checkedSystemCall(pending.work, token, "io.file_sync", [&] { return ::fsync(pending.leaf.get()); });
        checkedSystemCall(pending.work, token, "io.directory_sync", [&] { return ::fsync(parent(pending)); });
        auto sealed = crypto_->FinishSha256(pending.hash, token);
        if (!sealed.is_success())
            throw detail::Error("io.crypto_finish");
        if (sealed.value().byteCount != static_cast<std::int64_t>(pending.accepted))
            throw detail::Error("io.crypto_count");

        verifyNamed(pending, token);
        Digest result;
        result.byteCount = sealed.value().byteCount;
        result.sha256 = sealed.value().lowercaseHex;
        result.identity = identity(pending.revision);
        detail::checkCancelled(token);
        return result;
    }

    static Identity identity(const struct stat& value) {
        Identity result;
        result.objectId = std::to_string(value.st_dev) + ":" + std::to_string(value.st_ino);
        result.revision = std::to_string(value.st_size) + ":" + std::to_string(value.st_mtim.tv_sec) + ":" +
            std::to_string(value.st_mtim.tv_nsec) + ":" + std::to_string(value.st_ctim.tv_sec) + ":" +
            std::to_string(value.st_ctim.tv_nsec);
        result.linkCount = value.st_nlink;
        return result;
    }
};

} // namespace pipelang::io
