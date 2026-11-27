// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Fixture.hpp"

#include <cstdarg>
#include <fstream>
#include <stdexcept>

namespace {
Fixture* active = nullptr;
}

Fixture::Fixture(std::filesystem::path root) : root_(std::move(root)) {
    if (active != nullptr)
        throw std::runtime_error("One output fixture may own the syscall schedule");

    preCancelled_.cancel();
    active = this;
}

Fixture::~Fixture() {
    active = nullptr;
}

bool Fixture::validPath(std::string_view relative) {
    const auto suffix = relative.substr(std::min(relative.size(), std::string_view("output-").size()));
    return relative.starts_with("output-") && !suffix.empty() && suffix.size() <= 2 &&
           suffix.find_first_not_of("0123456789") == std::string_view::npos;
}

FixtureResult<void> Fixture::Arm(std::string relative, std::string fault, std::int64_t scope) {
    const bool supported = fault == "cancel_create" || fault == "cancel_write" || fault == "short_error" ||
        fault == "zero" || fault == "interrupt" || fault == "sync_error" || fault == "cancel_sync" ||
        fault == "replace" || fault == "close_error";
    if (!validPath(relative) || !fault_.empty() || !supported || scope < 0 ||
            static_cast<std::uint64_t>(scope) >= tokens_.size())
        return FixtureResult<void>::Failure("fixture schedule is invalid or already armed");

    target_ = std::move(relative);
    fault_ = std::move(fault);
    scope_ = static_cast<std::size_t>(scope);
    return FixtureResult<void>::Success();
}

FixtureResult<void> Fixture::Restore() {
    if (observedDescriptor_ >= 0)
        return FixtureResult<void>::Failure("fixture writer descriptor is still open");
    if (!failure_.empty())
        return FixtureResult<void>::Failure(failure_);

    target_.clear();
    fault_.clear();
    shortWritten_ = false;
    return FixtureResult<void>::Success();
}

FixtureResult<std::int64_t> Fixture::Mode(std::string relative) {
    if (!validPath(relative))
        return FixtureResult<std::int64_t>::Failure("fixture relative file refused");

    struct stat information{};
    if (::lstat((root_ / relative).c_str(), &information) != 0 || !S_ISREG(information.st_mode))
        return FixtureResult<std::int64_t>::Failure("fixture regular file unavailable");

    return FixtureResult<std::int64_t>::Success(information.st_mode & 07777);
}

FixtureResult<Bytes> Fixture::Block(std::int64_t length) {
    if (length < 0 || length > 1048576)
        return FixtureResult<Bytes>::Failure("fixture byte bound");

    return FixtureResult<Bytes>::Success(Bytes(std::vector<std::uint8_t>(static_cast<std::size_t>(length), 'a')));
}

FixtureResult<std::int64_t> Fixture::Effects() {
    if (!failure_.empty())
        return FixtureResult<std::int64_t>::Failure(failure_);

    return FixtureResult<std::int64_t>::Success(effects_);
}

FixtureResult<std::int64_t> Fixture::OpenOperations() {
    auto& budget = pipelang::io::detail::budget;
    std::lock_guard guard(budget.mutex);
    if (observedDescriptor_ >= 0)
        return FixtureResult<std::int64_t>::Failure("fixture descriptor retained");

    return FixtureResult<std::int64_t>::Success(static_cast<std::int64_t>(budget.operations));
}

void Fixture::created(const char* name, int descriptor) {
    if (descriptor < 0 || name != target_)
        return;

    observedDescriptor_ = descriptor;
    if (fault_ == "cancel_create") {
        ++effects_;
        tokens_[scope_].cancel();
    }
}

bool Fixture::beforeWrite(int descriptor, std::size_t& count, ssize_t& failedCount) {
    if (descriptor != observedDescriptor_)
        return false;
    if (fault_ == "short_error" && !shortWritten_) {
        count = 1;
        shortWritten_ = true;
        return false;
    }
    if (fault_ == "short_error" || fault_ == "interrupt" || fault_ == "zero") {
        ++effects_;
        failedCount = -1;
        if (fault_ == "short_error") {
            errno = EIO;
        } else if (fault_ == "interrupt") {
            errno = EINTR;
        } else {
            failedCount = 0;
        }
        return true;
    }

    return false;
}

void Fixture::afterWrite(int descriptor, ssize_t count) {
    if (descriptor == observedDescriptor_ && count > 0 && fault_ == "cancel_write") {
        ++effects_;
        tokens_[scope_].cancel();
    }
}

bool Fixture::beforeSync(int descriptor) {
    if (descriptor != observedDescriptor_ || fault_ != "sync_error")
        return false;

    ++effects_;
    errno = EIO;
    return true;
}

void Fixture::afterSync(int descriptor, int result) {
    if (descriptor != observedDescriptor_ || result != 0)
        return;

    if (fault_ == "cancel_sync") {
        ++effects_;
        tokens_[scope_].cancel();
    } else if (fault_ == "replace") {
        try {
            const auto current = root_ / target_;
            std::filesystem::rename(current, root_ / (target_ + ".saved"));
            std::ofstream replacement(current, std::ios::out | std::ios::binary);
            replacement.exceptions(std::ios::failbit | std::ios::badbit);
            replacement.write("new", 3);
            replacement.close();
            ++effects_;
        } catch (const std::exception& error) {
            failure_ = error.what();
        }
    }
}

bool Fixture::afterClose(int descriptor, int result) {
    if (descriptor != observedDescriptor_)
        return false;

    observedDescriptor_ = -1;
    if (result != 0 || fault_ != "close_error")
        return false;

    ++effects_;
    errno = EIO;
    return true;
}

extern "C" int __real_openat(int parent, const char* name, int flags, ...);
extern "C" ssize_t __real_write(int descriptor, const void* data, std::size_t count);
extern "C" int __real_fsync(int descriptor);
extern "C" int __real_close(int descriptor);

extern "C" int __wrap_openat(int parent, const char* name, int flags, ...) {
    if ((flags & O_CREAT) == 0)
        return __real_openat(parent, name, flags);

    std::va_list arguments;
    va_start(arguments, flags);
    const auto mode = va_arg(arguments, int);
    va_end(arguments);

    const auto result = __real_openat(parent, name, flags, mode);
    const auto observedErrno = errno;
    if (active != nullptr)
        active->created(name, result);

    errno = observedErrno;
    return result;
}

extern "C" ssize_t __wrap_write(int descriptor, const void* data, std::size_t count) {
    ssize_t failedCount = -1;
    if (active != nullptr && active->beforeWrite(descriptor, count, failedCount))
        return failedCount;

    const auto result = __real_write(descriptor, data, count);
    const auto observedErrno = errno;
    if (active != nullptr)
        active->afterWrite(descriptor, result);

    errno = observedErrno;
    return result;
}

extern "C" int __wrap_fsync(int descriptor) {
    if (active != nullptr && active->beforeSync(descriptor))
        return -1;

    const auto result = __real_fsync(descriptor);
    const auto observedErrno = errno;
    if (active != nullptr)
        active->afterSync(descriptor, result);

    errno = observedErrno;
    return result;
}

extern "C" int __wrap_close(int descriptor) {
    const auto result = __real_close(descriptor);
    const auto observedErrno = errno;
    if (active != nullptr && active->afterClose(descriptor, result))
        return -1;

    errno = observedErrno;
    return result;
}
