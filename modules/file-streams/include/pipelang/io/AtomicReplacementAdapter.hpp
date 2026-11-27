// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LinuxFiles.hpp"

#include <memory>
#include <new>
#include <utility>

namespace pipelang::io {

template<class Contract, class Identity, class Failure, class Effect>
class AtomicReplacementAdapter final : public Contract {
    template<class Value>
    using Result = pipelang::model::Result<Value, Failure>;
    using Token = pipelang::model::CancellationToken;
    using Reservation = detail::Reservation;

public:
    AtomicReplacementAdapter(const std::string& absoluteRoot, const std::string& stagingComponent,
                              PathProfile profile)
        : profile_(profile) {
        try {
            detail::Work work(profile.maximumSystemCalls);
            const auto names = detail::components(absoluteRoot, true, profile);
            const auto stagingName = leafName(stagingComponent);
            owner_ = std::make_unique<Reservation>(Reservation::Kind::Owner);
            descriptors_ = std::make_unique<Reservation>(Reservation::Kind::Descriptors, names.size() + 2);
            root_.directories.reserve(names.size() + 1);
            root_.append(AT_FDCWD, "/", {}, work);
            for (const auto& name : names)
                root_.append(root_.last(), name, {}, work);

            staging_.directories.reserve(1);
            staging_.append(root_.last(), stagingName, {}, work);
            verifyDirectories({}, work);
        } catch (const detail::Error& error) {
            startupError_ = error.what();
            retire();
        } catch (const std::bad_alloc&) {
            startupError_ = "io.memory_limit";
            retire();
        }
    }

    ~AtomicReplacementAdapter() override { retire(); }
    AtomicReplacementAdapter(const AtomicReplacementAdapter&) = delete;
    AtomicReplacementAdapter& operator=(const AtomicReplacementAdapter&) = delete;

    Result<Effect> ReplaceFile(std::string stagedName, std::string destinationName,
                               Identity stagedIdentity, std::int64_t stagedBytes, Token token) override {
        auto effect = Effect::NoDurableOutput;
        if (token.is_cancelled())
            return Result<Effect>::Cancelled(failure("io.cancelled", effect));

        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<Effect>::Failure(failure("io.busy", effect));
        if (!startupError_.empty())
            return Result<Effect>::Failure(failure(startupError_, effect));

        std::unique_ptr<Reservation> operation;
        std::unique_ptr<Reservation> descriptor;
        detail::Descriptor leaf;
        try {
            detail::Work work(profile_.maximumSystemCalls);
            const auto source = leafName(stagedName);
            const auto destination = leafName(destinationName);
            const auto& stagingName = staging_.directories.front().name;
            if (destination == stagingName)
                throw detail::Error("io.replacement_destination");
            if (stagedBytes < 0 || stagedBytes > 33554432)
                throw detail::Error("io.invalid_limit");

            operation = std::make_unique<Reservation>(Reservation::Kind::Operation);
            descriptor = std::make_unique<Reservation>(Reservation::Kind::Descriptors);
            verifyDirectories(token, work);
            struct stat named{};
            detail::namedStat(staging_.last(), source, named, token, work);
            if (!privateRegular(named))
                throw detail::Error("io.replacement_source_type");

            leaf = detail::openAt(staging_.last(), source, O_RDONLY, token, work);
            struct stat opened{};
            detail::openedStat(leaf.get(), opened, token, work);
            if (!detail::sameRevision(named, opened) || !pinMatches(stagedIdentity, stagedBytes, opened))
                throw detail::Error("io.replacement_source_changed");

            verifyDirectories(token, work);
            struct stat immediatelyBefore{};
            detail::namedStat(staging_.last(), source, immediatelyBefore, token, work);
            if (!privateRegular(immediatelyBefore) || !detail::sameRevision(opened, immediatelyBefore))
                throw detail::Error("io.replacement_source_changed");

            detail::checkCancelled(token);
            work.attempt();
            effect = Effect::Unknown;
            if (::renameat(staging_.last(), source.c_str(), root_.last(), destination.c_str()) != 0)
                throw detail::Error("io.replace_failed");

            verifyDestination(destination, opened, leaf.get(), token, work);
            syncDirectory(staging_.last(), token, work);
            syncDirectory(root_.last(), token, work);
            verifyDirectories(token, work);
            verifyDestination(destination, opened, leaf.get(), token, work);

            if (!leaf.close())
                throw detail::Error("io.close_failed");

            detail::checkCancelled(token);
            return Result<Effect>::Success(Effect::Committed);
        } catch (const detail::Error& error) {
            return failed(error.what(), effect, leaf, token);
        } catch (const std::bad_alloc&) {
            return failed("io.memory_limit", effect, leaf, token);
        }
    }

private:
    PathProfile profile_;
    std::mutex mutex_;
    std::unique_ptr<Reservation> owner_;
    std::unique_ptr<Reservation> descriptors_;
    detail::Chain root_;
    detail::Chain staging_;
    std::string startupError_;

    static Failure failure(std::string code, Effect effect) {
        Failure result;
        result.code = std::move(code);
        result.effect = effect;
        return result;
    }

    static Result<Effect> failed(std::string code, Effect effect, detail::Descriptor& leaf, const Token& token) {
        if (!leaf.close())
            code += "+io.close_failed";
        if (token.is_cancelled())
            return Result<Effect>::Cancelled(failure(std::move(code), effect));

        return Result<Effect>::Failure(failure(std::move(code), effect));
    }

    void retire() noexcept {
        (void)staging_.close();
        (void)root_.close();
        descriptors_.reset();
        owner_.reset();
    }

    std::string leafName(const std::string& path) const {
        auto components = detail::components(path, false, profile_);
        if (components.size() != 1)
            throw detail::Error("io.replacement_leaf");

        return std::move(components.front());
    }

    void verifyDirectories(const Token& token, detail::Work& work) const {
        root_.verify(AT_FDCWD, token, work);
        staging_.verify(root_.last(), token, work);
        struct stat opened{};
        detail::openedStat(staging_.last(), opened, token, work);
        if ((opened.st_mode & 07777) != 0700 || opened.st_uid != ::geteuid())
            throw detail::Error("io.replacement_staging_mode");
    }

    static bool privateRegular(const struct stat& value) {
        return S_ISREG(value.st_mode) && (value.st_mode & 07777) == 0600 && value.st_nlink == 1;
    }

    static bool pinMatches(const Identity& expected, std::int64_t expectedBytes, const struct stat& actual) {
        if (!privateRegular(actual) || actual.st_size != expectedBytes ||
                !std::cmp_equal(actual.st_nlink, expected.linkCount))
            return false;

        const auto objectId = std::to_string(actual.st_dev) + ":" + std::to_string(actual.st_ino);
        const auto revision = std::to_string(actual.st_size) + ":" + std::to_string(actual.st_mtim.tv_sec) + ":" +
            std::to_string(actual.st_mtim.tv_nsec) + ":" + std::to_string(actual.st_ctim.tv_sec) + ":" +
            std::to_string(actual.st_ctim.tv_nsec);
        return objectId == expected.objectId && revision == expected.revision;
    }

    void verifyDestination(const std::string& target, const struct stat& before, int held,
                           const Token& token, detail::Work& work) const {
        struct stat named{};
        struct stat opened{};
        detail::namedStat(root_.last(), target, named, token, work);
        detail::openedStat(held, opened, token, work);
        // Rename can advance ctime. The held inode, size and mtime must still
        // match, and the name must resolve to the currently observed revision.
        if (!privateRegular(named) || !privateRegular(opened) || !detail::sameObject(before, opened) ||
                before.st_size != opened.st_size || before.st_mtim.tv_sec != opened.st_mtim.tv_sec ||
                before.st_mtim.tv_nsec != opened.st_mtim.tv_nsec || !detail::sameRevision(named, opened))
            throw detail::Error("io.replacement_destination_changed");
    }

    static void syncDirectory(int directory, const Token& token, detail::Work& work) {
        for (;;) {
            detail::checkCancelled(token);
            work.attempt();
            const int status = ::fsync(directory);
            const int error = errno;
            detail::checkCancelled(token);
            if (status == 0)
                return;
            if (error != EINTR)
                throw detail::Error("io.replacement_directory_sync");
        }
    }
};

} // namespace pipelang::io
