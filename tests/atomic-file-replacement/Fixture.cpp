// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Fixture.hpp"

#include <array>
#include <stdexcept>

namespace {
Fixture* active = nullptr;
}

Fixture::Fixture(std::filesystem::path root) : root_(std::move(root)) {
    if (active != nullptr)
        throw std::runtime_error("One report fixture may own the syscall schedules");

    preCancelled_.cancel();
    active = this;
}

Fixture::~Fixture() {
    active = nullptr;
}

bool Fixture::valid(std::string_view path, std::string_view prefix) {
    if (!path.starts_with(prefix) || path.size() != prefix.size() + 1)
        return false;

    return path.back() >= '0' && path.back() <= '9';
}

FixtureResult<void> Fixture::Prepare(std::string target, std::string staged, bool existing, std::string fault) {
    if (!valid(target, "report-") || !valid(staged, "stage-") || !target_.empty() ||
            (!fault.empty() && fault != "cancel_after_rename" && fault != "sync_after_rename"))
        return FixtureResult<void>::Failure("fixture invalid composition");

    target_ = std::move(target);
    staged_ = std::move(staged);
    fault_ = std::move(fault);
    if (!existing)
        return FixtureResult<void>::Success();

    previous_ = pipelang::io::detail::Descriptor(::open((root_ / target_).c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (previous_.get() < 0 || ::write(previous_.get(), "old", 3) != 3)
        return FixtureResult<void>::Failure("fixture previous report creation");

    return FixtureResult<void>::Success();
}

FixtureResult<void> Fixture::ChangeStage() {
    pipelang::io::detail::Descriptor file(::open((root_ / "staged" / staged_).c_str(),
        O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
    const std::array<timespec,2> changed{{{1, 0}, {1, 0}}};
    if (file.get() < 0 || ::pwrite(file.get(), "new", 3, 0) != 3 || ::futimens(file.get(), changed.data()) != 0)
        return FixtureResult<void>::Failure("fixture stage mutation");

    if (!file.close())
        return FixtureResult<void>::Failure("fixture stage close");

    return FixtureResult<void>::Success();
}

FixtureResult<Bytes> Fixture::OldHandleBytes() {
    std::vector<std::uint8_t> bytes(3);
    if (previous_.get() < 0 || ::pread(previous_.get(), bytes.data(), bytes.size(), 0) != 3)
        return FixtureResult<Bytes>::Failure("fixture previous handle read");

    return FixtureResult<Bytes>::Success(Bytes(std::move(bytes)));
}

FixtureResult<Bytes> Fixture::Block(std::int64_t count) {
    if (count < 0 || count > 1048576)
        return FixtureResult<Bytes>::Failure("fixture byte bound");

    return FixtureResult<Bytes>::Success(Bytes(std::vector<std::uint8_t>(static_cast<std::size_t>(count), 'a')));
}

FixtureResult<std::int64_t> Fixture::Renames() {
    return FixtureResult<std::int64_t>::Success(renames_);
}

FixtureResult<void> Fixture::Close() {
    const bool closed = previous_.close();
    target_.clear();
    staged_.clear();
    fault_.clear();
    reachedRename_ = false;
    if (!closed)
        return FixtureResult<void>::Failure("fixture previous report close");

    return FixtureResult<void>::Success();
}

void Fixture::renamed(const char* stage, const char* target, int result) {
    if (stage != staged_ || target != target_ || result != 0)
        return;

    ++renames_;
    reachedRename_ = true;
    if (fault_ == "cancel_after_rename")
        postRename_.cancel();
}

bool Fixture::refuseSync() const {
    return reachedRename_ && fault_ == "sync_after_rename";
}

extern "C" int __real_renameat(int from, const char* staged, int to, const char* target);
extern "C" int __real_fsync(int descriptor);

extern "C" int __wrap_renameat(int from, const char* staged, int to, const char* target) {
    const int result = __real_renameat(from, staged, to, target);
    const int observedError = errno;
    if (active != nullptr)
        active->renamed(staged, target, result);

    errno = observedError;
    return result;
}

extern "C" int __wrap_fsync(int descriptor) {
    if (active != nullptr && active->refuseSync()) {
        errno = EIO;
        return -1;
    }

    return __real_fsync(descriptor);
}
