// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LinuxFiles.hpp"
#include <pipelang/model/byte_buffer.hpp>
#include <pipelang/model/result.hpp>

#include <memory>
#include <new>

namespace pipelang::io {

template<class Contract, class State, class Identity, class Failure, class Effect>
class ExistingFileAppendAdapter final : public Contract {
    template<class T>
    using Result = pipelang::model::Result<T, Failure>;
    using Token = pipelang::model::CancellationToken;
    using Bytes = pipelang::model::ByteBuffer;
    using Reservation = detail::Reservation;

    struct OpenFile {
        explicit OpenFile(std::size_t maxWork) : work(maxWork), operation(Reservation::Kind::Operation) {}

        detail::Work work;
        Reservation operation;
        std::unique_ptr<Reservation> descriptors;
        detail::Chain parents;
        detail::Descriptor leaf;
        std::vector<std::string> names;
        struct stat revision{};
        int parent = -1;
        Effect effect = Effect::NoDurableOutput;

        bool close() noexcept {
            const bool leafClosed = leaf.close();
            const bool parentsClosed = parents.close();
            descriptors.reset();
            return leafClosed && parentsClosed;
        }
    };

public:
    ExistingFileAppendAdapter(const std::string& absoluteRoot, PathProfile profile) : profile_(profile) {
        try {
            detail::Work work(profile.maximumSystemCalls);
            const auto names = detail::components(absoluteRoot, true, profile);
            owner_ = std::make_unique<Reservation>(Reservation::Kind::Owner);
            rootReservation_ = std::make_unique<Reservation>(Reservation::Kind::Descriptors, names.size() + 1);
            root_.directories.reserve(names.size() + 1);
            root_.append(AT_FDCWD, "/", {}, work);
            for (const auto& name : names)
                root_.append(root_.last(), name, {}, work);

            root_.verify(AT_FDCWD, {}, work);
        } catch (const detail::Error& error) {
            startupError_ = error.what();
            retireRoot();
        } catch (const std::bad_alloc&) {
            startupError_ = "io.memory_limit";
            retireRoot();
        }
    }

    ~ExistingFileAppendAdapter() override { retireRoot(); }
    ExistingFileAppendAdapter(const ExistingFileAppendAdapter&) = delete;
    ExistingFileAppendAdapter& operator=(const ExistingFileAppendAdapter&) = delete;

    Result<State> InspectAppendTarget(std::string relative, std::int64_t maximumBytes, Token token) override {
        return withFile(relative, maximumBytes, O_RDONLY, token, [](OpenFile& file) {
            return describe(file.revision);
        });
    }

    Result<State> AppendExistingFile(std::string relative, State expected, std::int64_t maximumBytes,
                                     Bytes bytes, Token token) override {
        return withFile(relative, maximumBytes, O_WRONLY | O_APPEND, token, [&](OpenFile& file) {
            const auto observed = describe(file.revision);
            if (expected.byteCount != observed.byteCount || expected.identity.objectId != observed.identity.objectId ||
                    expected.identity.revision != observed.identity.revision ||
                    expected.identity.linkCount != observed.identity.linkCount)
                throw detail::Error("io.append_stale");

            const auto& fragment = bytes.values();
            if (fragment.size() > static_cast<std::uint64_t>(maximumBytes - observed.byteCount))
                throw detail::Error("io.byte_limit");
            if (fragment.empty())
                return observed;

            detail::checkCancelled(token);
            file.work.attempt();
            file.effect = Effect::Unknown;
            const auto written = ::write(file.leaf.get(), fragment.data(), fragment.size());
            detail::checkCancelled(token);
            if (written < 0)
                throw detail::Error("io.append_write");
            if (static_cast<std::size_t>(written) != fragment.size())
                throw detail::Error("io.append_short");

            struct stat after{};
            detail::openedStat(file.leaf.get(), after, token, file.work);
            if (!privateRegular(after) || !detail::sameObject(file.revision, after) ||
                    after.st_size != observed.byteCount + written)
                throw detail::Error("io.append_changed");

            verify(file, after, token);
            return describe(after);
        });
    }

private:
    static constexpr std::int64_t maximumAppendBytes = 8LL * 1024 * 1024 * 1024;
    PathProfile profile_;
    std::mutex mutex_;
    std::unique_ptr<Reservation> owner_;
    std::unique_ptr<Reservation> rootReservation_;
    detail::Chain root_;
    std::string startupError_;

    static Failure failure(std::string code, Effect effect) {
        Failure value;
        value.code = std::move(code);
        value.effect = effect;
        return value;
    }

    static Result<State> failed(std::string code, std::unique_ptr<OpenFile>& file, const Token& token) {
        auto effect = Effect::NoDurableOutput;
        if (file) {
            effect = file->effect;
            if (!file->close())
                code += "+io.close_failed";
        }

        if (token.is_cancelled())
            return Result<State>::Cancelled(failure(std::move(code), effect));

        return Result<State>::Failure(failure(std::move(code), effect));
    }

    template<class Function>
    Result<State> withFile(const std::string& name, std::int64_t maximumBytes, int flags,
                           const Token& token, Function&& use) {
        if (token.is_cancelled())
            return Result<State>::Cancelled(failure("io.cancelled", Effect::NoDurableOutput));

        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<State>::Failure(failure("io.busy", Effect::NoDurableOutput));
        if (!startupError_.empty())
            return Result<State>::Failure(failure(startupError_, Effect::NoDurableOutput));
        if (maximumBytes < 0 || maximumBytes > maximumAppendBytes)
            return Result<State>::Failure(failure("io.invalid_limit", Effect::NoDurableOutput));

        std::unique_ptr<OpenFile> file;
        try {
            file = std::make_unique<OpenFile>(profile_.maximumSystemCalls);
            admit(*file, name, maximumBytes, flags, token);
            auto result = Result<State>::Success(use(*file));
            if (!file->close())
                return failed("io.close_failed", file, token);

            detail::checkCancelled(token);
            return result;
        } catch (const detail::Error& error) {
            return failed(error.what(), file, token);
        } catch (const std::bad_alloc&) {
            return failed("io.memory_limit", file, token);
        }
    }

    void admit(OpenFile& file, const std::string& name, std::int64_t maximumBytes, int flags,
               const Token& token) {
        file.names = detail::components(name, false, profile_);
        file.descriptors = std::make_unique<Reservation>(Reservation::Kind::Descriptors, file.names.size());
        root_.verify(AT_FDCWD, token, file.work);
        file.parents.directories.reserve(file.names.size() - 1);
        file.parent = root_.last();
        for (std::size_t index = 0; index + 1 < file.names.size(); ++index) {
            file.parents.append(file.parent, file.names[index], token, file.work);
            file.parent = file.parents.last();
        }

        struct stat before{};
        detail::namedStat(file.parent, file.names.back(), before, token, file.work);
        if (!privateRegular(before))
            throw detail::Error("io.append_type");

        file.leaf = detail::openAt(file.parent, file.names.back(), flags, token, file.work);
        detail::openedStat(file.leaf.get(), file.revision, token, file.work);
        if (!privateRegular(file.revision) || !detail::sameRevision(before, file.revision))
            throw detail::Error("io.append_changed");
        if (file.revision.st_size < 0 || file.revision.st_size > maximumBytes)
            throw detail::Error("io.byte_limit");

        verify(file, file.revision, token);
    }

    void verify(OpenFile& file, const struct stat& expected, const Token& token) {
        struct stat named{};
        detail::namedStat(file.parent, file.names.back(), named, token, file.work);
        if (!privateRegular(named) || !detail::sameRevision(expected, named))
            throw detail::Error("io.append_changed");

        file.parents.verify(root_.last(), token, file.work);
        root_.verify(AT_FDCWD, token, file.work);
    }

    static bool privateRegular(const struct stat& observed) {
        return S_ISREG(observed.st_mode) && (observed.st_mode & 07777) == 0600 &&
            observed.st_uid == ::geteuid() && observed.st_nlink == 1;
    }

    static State describe(const struct stat& value) {
        Identity identity;
        identity.objectId = std::to_string(value.st_dev) + ":" + std::to_string(value.st_ino);
        identity.revision = std::to_string(value.st_size) + ":" + std::to_string(value.st_mtim.tv_sec) + ":" +
            std::to_string(value.st_mtim.tv_nsec) + ":" + std::to_string(value.st_ctim.tv_sec) + ":" +
            std::to_string(value.st_ctim.tv_nsec);
        identity.linkCount = value.st_nlink;

        State result;
        result.byteCount = value.st_size;
        result.identity = std::move(identity);
        return result;
    }

    void retireRoot() noexcept {
        (void)root_.close();
        rootReservation_.reset();
        owner_.reset();
    }
};

} // namespace pipelang::io
