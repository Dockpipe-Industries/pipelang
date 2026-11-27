// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#if !defined(__linux__) || !defined(__x86_64__)
#error PipeLangFileStreams 0.1.0 selects Linux x86_64 only
#endif

#include <pipelang/model/cancellation.hpp>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pipelang::io {

// An explicit caller profile, not a universal filesystem limit. The caller
// must provide both values. The implementation's finite ledger has 256 slots.
struct PathProfile {
    std::size_t maximumPathBytes;
    std::size_t maximumComponents;
    std::size_t maximumSystemCalls;
};

namespace detail {

inline constexpr std::size_t captureLimit = 4 * 1024 * 1024;
inline constexpr std::size_t digestLimit = 512 * 1024 * 1024;
inline constexpr std::size_t chunkSize = 64 * 1024;
inline constexpr std::size_t snapshotSlots = 8;
inline constexpr std::size_t captureBytes = snapshotSlots * captureLimit;
inline constexpr std::size_t descriptorSlots = 256;
inline constexpr std::size_t ownerSlots = 8;
inline constexpr std::size_t operationSlots = 8;

// One inline variable in one executable link-domain. No provider is copied.
// Independently loaded DSOs are separate domains, not a process-wide promise.
struct Budget {
    std::mutex mutex;
    std::size_t owners = 0;
    std::size_t operations = 0;
    std::size_t descriptors = 0;
    std::size_t snapshots = 0;
    std::size_t bytes = 0;
    std::int64_t lastIdentifier = 0;
};
inline Budget budget;

class Error : public std::runtime_error {
public:
    explicit Error(const char* code) : std::runtime_error(code) {}
};

inline void checkCancelled(const pipelang::model::CancellationToken& token) {
    if (token.is_cancelled())
        throw Error("io.cancelled");
}

// Reservations are acquired before the corresponding allocation/syscall.
class Work {
public:
    explicit Work(std::size_t maximum) : remaining_(maximum) {
        if (maximum == 0 || maximum > 1048576)
            throw Error("io.invalid_profile");
    }
    void attempt() {
        if (remaining_ == 0)
            throw Error("io.work_limit");
        --remaining_;
    }
private:
    std::size_t remaining_;
};

class Reservation {
public:
    enum class Kind { Owner, Operation, Descriptors, Capture };

    Reservation(Kind kind, std::size_t count = 1) : kind_(kind), count_(count) {
        std::lock_guard guard(budget.mutex);
        switch (kind_) {
        case Kind::Owner:
            if (budget.owners == ownerSlots)
                throw Error("io.owner_limit");
            ++budget.owners;
            break;
        case Kind::Operation:
            if (budget.operations == operationSlots)
                throw Error("io.operation_limit");
            ++budget.operations;
            break;
        case Kind::Descriptors:
            if (count_ > descriptorSlots - budget.descriptors)
                throw Error("io.descriptor_limit");
            budget.descriptors += count_;
            break;
        case Kind::Capture:
            if (budget.snapshots == snapshotSlots || count_ > captureBytes - budget.bytes)
                throw Error("io.snapshot_limit");
            ++budget.snapshots;
            budget.bytes += count_;
            break;
        }
    }

    ~Reservation() { release(); }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

    void release() noexcept {
        if (!active_)
            return;

        std::lock_guard guard(budget.mutex);
        switch (kind_) {
        case Kind::Owner: --budget.owners; break;
        case Kind::Operation: --budget.operations; break;
        case Kind::Descriptors: budget.descriptors -= count_; break;
        case Kind::Capture:
            --budget.snapshots;
            budget.bytes -= count_;
            break;
        }
        active_ = false;
    }

private:
    Kind kind_;
    std::size_t count_;
    bool active_ = true;
};

inline std::int64_t nextIdentifier() {
    std::lock_guard guard(budget.mutex);
    if (budget.lastIdentifier == std::numeric_limits<std::int64_t>::max())
        throw Error("io.identifier_limit");

    return ++budget.lastIdentifier;
}

class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() { (void)close(); }
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        (void)close();
        value_ = std::exchange(other.value_, -1);
        return *this;
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }

    bool close() noexcept {
        if (value_ < 0)
            return true;

        // Linux consumes the descriptor even on EINTR; never close a reused FD.
        return ::close(std::exchange(value_, -1)) == 0;
    }

private:
    int value_;
};

inline bool sameObject(const struct stat& first, const struct stat& second) {
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino &&
        (first.st_mode & S_IFMT) == (second.st_mode & S_IFMT);
}

inline bool sameRevision(const struct stat& first, const struct stat& second) {
    return sameObject(first, second) && first.st_size == second.st_size &&
        first.st_nlink == second.st_nlink && first.st_mtim.tv_sec == second.st_mtim.tv_sec &&
        first.st_mtim.tv_nsec == second.st_mtim.tv_nsec &&
        first.st_ctim.tv_sec == second.st_ctim.tv_sec && first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
}

inline void namedStat(int parent, const std::string& name, struct stat& value,
        const pipelang::model::CancellationToken& token, Work& work) {
    for (;;) {
        checkCancelled(token);
        work.attempt();
        const auto status = ::fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW);
        const auto failure = errno;
        checkCancelled(token);
        if (status == 0)
            return;
        if (failure != EINTR)
            throw Error("io.named_identity");
    }
}

inline void openedStat(int descriptor, struct stat& value,
        const pipelang::model::CancellationToken& token, Work& work) {
    for (;;) {
        checkCancelled(token);
        work.attempt();
        const auto status = ::fstat(descriptor, &value);
        const auto failure = errno;
        checkCancelled(token);
        if (status == 0)
            return;
        if (failure != EINTR)
            throw Error("io.opened_identity");
    }
}

inline Descriptor openAt(int parent, const std::string& name, int flags,
        const pipelang::model::CancellationToken& token, Work& work) {
    for (;;) {
        checkCancelled(token);
        work.attempt();
        const auto value = ::openat(parent, name.c_str(), flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        const auto failure = errno;
        Descriptor admitted(value);
        checkCancelled(token);
        if (value >= 0)
            return admitted;
        if (failure != EINTR)
            throw Error("io.open_failed");
    }
}

inline std::vector<std::string> components(const std::string& path, bool absolute, PathProfile profile) {
    if (profile.maximumPathBytes == 0 || profile.maximumComponents == 0 ||
            profile.maximumComponents >= descriptorSlots || path.empty() ||
            path.size() > profile.maximumPathBytes || path.find('\0') != std::string::npos ||
            (path.front() == '/') != absolute)
        throw Error("io.invalid_path");

    std::vector<std::string> result;
    // Every accepted component owns at most path.size() bytes in aggregate;
    // reserve the finite component ledger, never a path-sized component array.
    result.reserve(profile.maximumComponents);
    std::size_t position = absolute ? 1 : 0;
    if (absolute && path == "/")
        return result;

    while (position < path.size()) {
        const auto end = path.find('/', position);
        const auto length = (end == std::string::npos ? path.size() : end) - position;
        if (length == 0 || result.size() == profile.maximumComponents)
            throw Error("io.invalid_path");

        auto component = path.substr(position, length);
        if (component == "." || component == "..")
            throw Error("io.invalid_path");

        result.push_back(std::move(component));
        if (end == std::string::npos)
            return result;

        position = end + 1;
        if (position == path.size())
            throw Error("io.invalid_path");
    }
    return result;
}

struct Directory {
    Descriptor descriptor;
    std::string name;
    struct stat identity{};
};

// Edges bind to held parent objects. Revalidation observes names at start/end;
// it is not an atomic namespace lease or a continuously named-root guarantee.
class Chain {
public:
    std::vector<Directory> directories;

    void append(int parent, const std::string& name, const pipelang::model::CancellationToken& token, Work& work) {
        struct stat initial{};
        namedStat(parent, name, initial, token, work);
        if (!S_ISDIR(initial.st_mode))
            throw Error("io.directory_type");

        auto opened = openAt(parent, name, O_RDONLY | O_DIRECTORY, token, work);
        struct stat identity{};
        openedStat(opened.get(), identity, token, work);
        if (!S_ISDIR(identity.st_mode) || !sameObject(initial, identity))
            throw Error("io.directory_changed");

        directories.push_back(Directory{std::move(opened), name, identity});
    }

    void verify(int parent, const pipelang::model::CancellationToken& token, Work& work) const {
        for (const auto& directory : directories) {
            struct stat named{};
            struct stat opened{};
            namedStat(parent, directory.name, named, token, work);
            openedStat(directory.descriptor.get(), opened, token, work);
            if (!S_ISDIR(named.st_mode) || !sameObject(directory.identity, named) ||
                    !sameObject(directory.identity, opened))
                throw Error("io.directory_changed");

            parent = directory.descriptor.get();
        }
    }

    int last() const { return directories.back().descriptor.get(); }

    bool close() noexcept {
        bool success = true;
        for (auto position = directories.rbegin(); position != directories.rend(); ++position) {
            if (!position->descriptor.close())
                success = false;
        }
        return success;
    }
};

} // namespace detail
} // namespace pipelang::io
