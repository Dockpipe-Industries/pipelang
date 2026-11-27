// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Fixture.hpp"

#include <array>
#include <stdexcept>

namespace {
Fixture* active = nullptr;
}

Fixture::Fixture(std::filesystem::path root) : root_(std::move(root)) {
    if (active != nullptr)
        throw std::runtime_error("One append fixture may own the syscall schedules");

    preCancelled_.cancel();
    active = this;
}

Fixture::~Fixture() {
    active = nullptr;
}

bool Fixture::valid(std::string_view path) {
    return path.starts_with("events-") && path.size() == 8 && path.back() >= '0' && path.back() <= '9';
}

FixtureResult<void> Fixture::Prepare(std::string relative, std::string fault) {
    if (!valid(relative) || !target_.empty() || (!fault.empty() && fault != "cancel" &&
            fault != "short" && fault != "link" && fault != "large"))
        return FixtureResult<void>::Failure("fixture invalid composition");

    target_ = std::move(relative);
    fault_ = std::move(fault);
    std::string actual = target_;
    if (fault_ == "link")
        actual += "-target";

    pipelang::io::detail::Descriptor output(::open((root_ / actual).c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (output.get() < 0 || ::fchmod(output.get(), 0600) != 0)
        return FixtureResult<void>::Failure("fixture output creation");

    if (fault_ == "large") {
        if (::ftruncate(output.get(), 8589934592LL) != 0)
            return FixtureResult<void>::Failure("fixture sparse 8 GiB extent");
    } else {
        if (::write(output.get(), "old\n", 4) != 4)
            return FixtureResult<void>::Failure("fixture initial content");
    }

    if (!output.close())
        return FixtureResult<void>::Failure("fixture initial content close");
    if (fault_ == "link" && ::symlink(actual.c_str(), (root_ / target_).c_str()) != 0)
        return FixtureResult<void>::Failure("fixture actual symlink");

    return FixtureResult<void>::Success();
}

FixtureResult<void> Fixture::ChangeRevision() {
    pipelang::io::detail::Descriptor output(::open((root_ / target_).c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
    const std::array<timespec,2> changed{{{1, 0}, {1, 0}}};
    if (output.get() < 0 || ::pwrite(output.get(), "OLD", 3, 0) != 3 ||
            ::futimens(output.get(), changed.data()) != 0)
        return FixtureResult<void>::Failure("fixture same-length modification");

    if (!output.close())
        return FixtureResult<void>::Failure("fixture different revision close");

    return FixtureResult<void>::Success();
}

FixtureResult<void> Fixture::Close() {
    auto& budget = pipelang::io::detail::budget;
    std::lock_guard guard(budget.mutex);
    if (descriptor_ >= 0 || budget.operations != 0 || budget.snapshots != 0)
        return FixtureResult<void>::Failure("fixture native operation survives cleanup");

    target_.clear();
    fault_.clear();
    return FixtureResult<void>::Success();
}

FixtureResult<std::int64_t> Fixture::Effects() {
    return FixtureResult<std::int64_t>::Success(effects_);
}

void Fixture::opened(const char* name, int flags, int descriptor) {
    if (name == target_ && descriptor >= 0 && (flags & O_APPEND) != 0)
        descriptor_ = descriptor;
}

std::size_t Fixture::beforeWrite(int descriptor, std::size_t length) const {
    if (descriptor == descriptor_ && fault_ == "short" && length > 1)
        return 1;

    return length;
}

void Fixture::afterWrite(int descriptor, ssize_t count) {
    if (descriptor != descriptor_ || count <= 0)
        return;

    ++effects_;
    if (fault_ == "cancel")
        postWrite_.cancel();
}

void Fixture::closed(int descriptor) {
    if (descriptor == descriptor_)
        descriptor_ = -1;
}

extern "C" int __real_openat(int parent, const char* name, int flags, ...);
extern "C" ssize_t __real_write(int descriptor, const void* bytes, std::size_t length);
extern "C" int __real_close(int descriptor);

extern "C" int __wrap_openat(int parent, const char* name, int flags, ...) {
    // The existing-file append and reader effects never pass O_CREAT.
    const int result = __real_openat(parent, name, flags);
    const int observedError = errno;
    if (active != nullptr)
        active->opened(name, flags, result);

    errno = observedError;
    return result;
}

#if defined(__GLIBC__)
// Preserve glibc's checked open contract when the flags are a runtime value.
extern "C" int __real___openat_2(int parent, const char* name, int flags);

extern "C" int __wrap___openat_2(int parent, const char* name, int flags) {
    const int result = __real___openat_2(parent, name, flags);
    const int observedError = errno;
    if (active != nullptr)
        active->opened(name, flags, result);

    errno = observedError;
    return result;
}
#endif

extern "C" ssize_t __wrap_write(int descriptor, const void* bytes, std::size_t length) {
    if (active != nullptr)
        length = active->beforeWrite(descriptor, length);

    const auto result = __real_write(descriptor, bytes, length);
    const auto observedError = errno;
    if (active != nullptr)
        active->afterWrite(descriptor, result);

    errno = observedError;
    return result;
}

extern "C" int __wrap_close(int descriptor) {
    const auto result = __real_close(descriptor);
    const auto observedError = errno;
    if (active != nullptr)
        active->closed(descriptor);

    errno = observedError;
    return result;
}
