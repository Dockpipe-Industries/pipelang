// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Fixture.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
Inputs* active = nullptr;
std::string leafName;
}

Inputs::Inputs(std::string estate, std::shared_ptr<Peer> peer)
    : estate_(std::move(estate)), root_(estate_ + "/root"), peer_(std::move(peer)) {
    if (active != nullptr)
        throw std::runtime_error("fixture observer already installed");
    active = this;
}

Inputs::~Inputs() {
    releaseAndJoin();
    active = nullptr;
}

void Inputs::require(bool success, const char* operation) {
    if (!success)
        throw std::runtime_error(operation);
}

std::string Inputs::file(const std::string& path) const {
    // The fixture's write authority is only its private, caller-owned estate.
    // No general hostile-directory read/write implementation is copied here.
    if (path.empty() || path.size() > 256 || path.front() == '/' ||
            path.find('\0') != std::string::npos || path.find("..") != std::string::npos)
        throw std::runtime_error("fixture path refused");
    return root_ + "/" + path;
}

void Inputs::put(const std::string& path, std::int64_t count, const std::string& pattern) {
    require(count >= 0 && count <= 536870913, "fixture extent refused");
    require(pattern == "a" || pattern == "abc" || pattern == "binary" || pattern == "sparse" || pattern == "new",
        "fixture byte generator refused");
    pipelang::io::detail::Descriptor output(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
    require(output.get() >= 0, "fixture open");
    if (pattern == "sparse") {
        require(::ftruncate(output.get(), count) == 0, "fixture sparse extent");
    } else {
        std::array<unsigned char, 65536> block{};
        std::int64_t position = 0;
        while (position < count) {
            const auto length = static_cast<std::size_t>(std::min<std::int64_t>(block.size(), count - position));
            for (std::size_t index = 0; index < length; ++index) {
                const auto offset = position + static_cast<std::int64_t>(index);
                if (pattern == "binary")
                    block[index] = static_cast<unsigned char>(offset & 255);
                else
                    block[index] = static_cast<unsigned char>(pattern[static_cast<std::size_t>(offset) % pattern.size()]);
            }
            std::size_t written = 0;
            std::size_t attempts = 0;
            while (written < length) {
                require(++attempts <= 1048576, "fixture write attempt limit");
                const auto result = ::write(output.get(), block.data() + written, length - written);
                if (result < 0 && errno == EINTR)
                    continue;
                require(result > 0, "fixture write");
                written += static_cast<std::size_t>(result);
            }
            position += static_cast<std::int64_t>(length);
        }
    }
    require(output.close(), "fixture close");
}

FixtureResult<void> Inputs::Create(std::string path, std::int64_t count, std::string pattern) {
    try {
        put(file(path), count, pattern);
        return FixtureResult<void>::Success();
    } catch (const std::exception& failure) {
        return FixtureResult<void>::Failure(failure.what());
    }
}

FixtureResult<void> Inputs::MakeDirectory(std::string path) {
    try {
        const auto name = file(path);
        require(::mkdir(name.c_str(), 0700) == 0, "fixture mkdir");
        return FixtureResult<void>::Success();
    } catch (const std::exception& failure) {
        return FixtureResult<void>::Failure(failure.what());
    }
}

FixtureResult<void> Inputs::MakeSymlink(std::string path, std::string target) {
    try {
        const auto name = file(path);
        (void)file(target);
        require(::symlink(target.c_str(), name.c_str()) == 0, "fixture symlink");
        return FixtureResult<void>::Success();
    } catch (const std::exception& failure) {
        return FixtureResult<void>::Failure(failure.what());
    }
}

FixtureResult<void> Inputs::MakeFifo(std::string path) {
    try {
        require(::mkfifo(file(path).c_str(), 0600) == 0, "fixture FIFO");
        return FixtureResult<void>::Success();
    } catch (const std::exception& failure) {
        return FixtureResult<void>::Failure(failure.what());
    }
}

FixtureResult<void> Inputs::MakeHardlink(std::string path, std::string target) {
    try {
        require(::link(file(target).c_str(), file(path).c_str()) == 0, "fixture hardlink");
        return FixtureResult<void>::Success();
    } catch (const std::exception& failure) {
        return FixtureResult<void>::Failure(failure.what());
    }
}

FixtureResult<void> Inputs::Arm(std::string boundary, std::string action, std::string path, std::int64_t value) {
    std::lock_guard guard(mutex_);
    if (schedule_.armed || worker_.joinable() || value < 0 || value > 1048576)
        return FixtureResult<void>::Failure("fixture schedule busy/invalid");
    try {
        (void)file(path);
        schedule_ = Schedule{std::move(boundary), std::move(action), std::move(path), value, 0, true};
        const auto slash = schedule_.path.find_last_of('/');
        leafName = schedule_.path.substr(slash == std::string::npos ? 0 : slash + 1);
        observedDescriptor_ = -1;
        return FixtureResult<void>::Success();
    } catch (const std::exception& failure) {
        return FixtureResult<void>::Failure(failure.what());
    }
}

bool Inputs::take(const char* boundary) {
    if (!schedule_.armed || schedule_.boundary != boundary)
        return false;
    schedule_.armed = false;
    ++schedules_;
    return true;
}

void Inputs::effect(std::unique_lock<std::mutex>& lock) {
    const auto path = file(schedule_.path);
    if (schedule_.action == "cancel") {
        require(schedule_.value < 8, "fixture cancellation scope");
        cancellation_[static_cast<std::size_t>(schedule_.value)].cancel();
    } else if (schedule_.action == "pause") {
        paused_ = true;
        condition_.notify_all();
        require(condition_.wait_for(lock, std::chrono::seconds(5), [&] { return released_; }), "fixture pending barrier timeout");
        paused_ = false;
    } else if (schedule_.action == "mutate") {
        pipelang::io::detail::Descriptor output(::open(path.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
        require(output.get() >= 0 && ::pwrite(output.get(), "b", 1, 0) == 1, "fixture actual mutation");
        const std::array<timespec, 2> times{{{1, 0}, {1, 0}}};
        require(::futimens(output.get(), times.data()) == 0, "fixture explicit changed revision");
    } else if (schedule_.action == "truncate") {
        require(::truncate(path.c_str(), 0) == 0, "fixture actual truncation");
    } else if (schedule_.action == "append") {
        pipelang::io::detail::Descriptor output(::open(path.c_str(), O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC));
        require(output.get() >= 0 && ::write(output.get(), "a", 1) == 1, "fixture actual append");
    } else if (schedule_.action == "replace" || schedule_.action == "symlink" || schedule_.action == "fifo") {
        const auto saved = path + ".saved." + std::to_string(schedules_);
        require(::rename(path.c_str(), saved.c_str()) == 0, "fixture actual leaf rename");
        renamed_.emplace_back(saved, path);
        if (schedule_.action == "replace")
            put(path, 3, "new");
        else if (schedule_.action == "symlink")
            require(::symlink("vector", path.c_str()) == 0, "fixture substituted symlink");
        else
            require(::mkfifo(path.c_str(), 0600) == 0, "fixture substituted FIFO");
    } else if (schedule_.action == "ancestor" || schedule_.action == "root") {
        const auto original = schedule_.action == "root" ? root_ : root_ + "/tree";
        const auto saved = original + ".saved." + std::to_string(schedules_);
        require(::rename(original.c_str(), saved.c_str()) == 0, "fixture actual directory rename");
        renamed_.emplace_back(saved, original);
        require(::mkdir(original.c_str(), 0700) == 0, "fixture actual directory replacement");
        put(original + "/race", 3, "new");
    } else {
        require(schedule_.action == "read_error" || schedule_.action == "close_error" || schedule_.action == "interrupts",
            "fixture unknown action");
    }
}

void Inputs::beforeOpen(const char* name) {
    std::unique_lock lock(mutex_);
    ++opens_;
    if (name != leafName || !take("before_open"))
        return;
    try { effect(lock); }
    catch (const std::exception& failure) { fixtureError_ = failure.what(); }
}

void Inputs::afterOpen(const char* name, int descriptor) {
    std::lock_guard guard(mutex_);
    if (name == leafName && descriptor >= 0)
        observedDescriptor_ = descriptor;
}

bool Inputs::beforeRead(int descriptor, std::size_t& count) {
    std::unique_lock lock(mutex_);
    if (descriptor != observedDescriptor_)
        return false;
    ++reads_;
    if (schedule_.remainingInterrupts > 0) {
        --schedule_.remainingInterrupts;
        errno = EINTR;
        return true;
    }
    const auto boundary = count == 1 ? "before_probe" : "before_read";
    if (!take(boundary))
        return false;
    try {
        effect(lock);
        if (schedule_.action == "read_error") {
            errno = EIO;
            return true;
        }
        if (schedule_.action == "interrupts") {
            schedule_.remainingInterrupts = schedule_.value;
            errno = EINTR;
            return true;
        }
    } catch (const std::exception& failure) {
        fixtureError_ = failure.what();
        errno = EIO;
        return true;
    }
    return false;
}

void Inputs::afterRead(int descriptor, ssize_t count) {
    std::unique_lock lock(mutex_);
    if (descriptor != observedDescriptor_ || count < 0)
        return;
    if (!take(count == 0 ? "after_eof" : "after_read"))
        return;
    try { effect(lock); }
    catch (const std::exception& failure) { fixtureError_ = failure.what(); }
}

bool Inputs::afterClose(int descriptor) {
    if (descriptor != observedDescriptor_.load())
        return false;
    std::unique_lock lock(mutex_);
    if (descriptor != observedDescriptor_)
        return false;
    observedDescriptor_ = -1;
    if (!take("after_close"))
        return false;
    try { effect(lock); }
    catch (const std::exception& failure) { fixtureError_ = failure.what(); }
    errno = EIO;
    return true;
}

void Inputs::provider(const char* boundary) {
    std::unique_lock lock(mutex_);
    if (std::strcmp(boundary, "after_hash") == 0)
        ++hashes_;
    else
        ++finishes_;
    if (!take(boundary))
        return;
    try { effect(lock); }
    catch (const std::exception& failure) { fixtureError_ = failure.what(); }
}

FixtureResult<void> Inputs::Restore() {
    releaseAndJoin();
    std::lock_guard guard(mutex_);
    schedule_.armed = false;
    schedule_.remainingInterrupts = 0;
    observedDescriptor_ = -1;
    leafName.clear();
    // Retain replacements too: rename them to unique fixture-owned siblings,
    // never unlink a checked name. Parent estate cleanup is independent.
    try {
        for (auto entry = renamed_.rbegin(); entry != renamed_.rend(); ++entry) {
            const auto retained = entry->second + ".retained." + std::to_string(schedules_);
            require(::rename(entry->second.c_str(), retained.c_str()) == 0, "fixture retain replacement");
            require(::rename(entry->first.c_str(), entry->second.c_str()) == 0, "fixture restore original object");
        }
        renamed_.clear();
        if (!fixtureError_.empty())
            return FixtureResult<void>::Failure(fixtureError_);
        return FixtureResult<void>::Success();
    } catch (const std::exception& failure) {
        return FixtureResult<void>::Failure(failure.what());
    }
}

FixtureResult<Bytes> Inputs::ReadFixture(std::string path, std::int64_t count) {
    try {
        require(count >= 0 && count <= 1048576, "fixture read cap");
        pipelang::io::detail::Descriptor input(::open(file(path).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        require(input.get() >= 0, "fixture observation open");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
        const auto observed = ::pread(input.get(), bytes.data(), bytes.size(), 0);
        require(observed >= 0, "fixture observation read");
        bytes.resize(static_cast<std::size_t>(observed));
        return FixtureResult<Bytes>::Success(Bytes(std::move(bytes)));
    } catch (const std::exception& failure) {
        return FixtureResult<Bytes>::Failure(failure.what());
    }
}

FixtureResult<tests::FileUsage> Inputs::Usage() {
    std::lock_guard fixtureGuard(mutex_);
    if (!fixtureError_.empty())
        return FixtureResult<tests::FileUsage>::Failure(fixtureError_);
    tests::FileUsage result;
    auto& budget = pipelang::io::detail::budget;
    std::lock_guard budgetGuard(budget.mutex);
    result.owners = static_cast<std::int64_t>(budget.owners);
    result.operations = static_cast<std::int64_t>(budget.operations);
    result.descriptors = static_cast<std::int64_t>(budget.descriptors);
    result.snapshots = static_cast<std::int64_t>(budget.snapshots);
    result.reservedBytes = static_cast<std::int64_t>(budget.bytes);
    result.reads = reads_;
    result.hashes = hashes_;
    result.finishes = finishes_;
    result.schedules = schedules_;
    result.opens = opens_;
    DIR* descriptors = ::opendir("/proc/self/fd");
    if (descriptors == nullptr)
        return FixtureResult<tests::FileUsage>::Failure("fixture descriptor observation");
    std::int64_t count = 0;
    while (const auto* entry = ::readdir(descriptors)) {
        if (entry->d_name[0] != '.')
            ++count;
        if (count > 1024) {
            (void)::closedir(descriptors);
            return FixtureResult<tests::FileUsage>::Failure("fixture descriptor observation limit");
        }
    }
    if (::closedir(descriptors) != 0)
        return FixtureResult<tests::FileUsage>::Failure("fixture descriptor observation close");
    result.openDescriptors = count;
    return FixtureResult<tests::FileUsage>::Success(result);
}

FixtureResult<void> Inputs::CancelProbe(std::int64_t scope) {
    if (scope < 0 || scope >= 8)
        return FixtureResult<void>::Failure("fixture cancellation scope");
    cancellation_[static_cast<std::size_t>(scope)].cancel();
    return FixtureResult<void>::Success();
}

pipelang::model::Result<api::FileDigest, api::IoFailure> Inputs::DigestWithWorkLimit(std::string path,
        std::int64_t maximumBytes, std::int64_t systemCalls, Token token) {
    FileAdapter bounded(root_, {4096, 32, static_cast<std::size_t>(systemCalls)});
    return bounded.DigestFile(std::move(path), maximumBytes, std::move(token));
}

FixtureResult<void> Inputs::RecreatePeer() {
    if (worker_.joinable())
        return FixtureResult<void>::Failure("fixture pending owner");
    peer_->recreate();
    return FixtureResult<void>::Success();
}

FixtureResult<void> Inputs::StartPending(std::string path, std::int64_t cap) {
    std::lock_guard guard(mutex_);
    if (worker_.joinable() || !schedule_.armed)
        return FixtureResult<void>::Failure("fixture pending busy/unarmed");
    paused_ = false;
    released_ = false;
    completed_.reset();
    worker_ = std::thread([this, path = std::move(path), cap] {
        try {
            auto result = peer_->CaptureSnapshot(path, cap, pendingCancellation_.token());
            std::lock_guard completedGuard(mutex_);
            completed_ = std::move(result);
        } catch (const std::exception& failure) {
            std::lock_guard completedGuard(mutex_);
            fixtureError_ = failure.what();
            api::IoFailure error;
            error.code = "fixture.worker_exception";
            error.effect = api::OutputEffect::NoDurableOutput;
            completed_ = CaptureResult::Failure(error);
        }
        condition_.notify_all();
    });
    return FixtureResult<void>::Success();
}

FixtureResult<void> Inputs::AwaitPendingRead() {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds(2), [&] { return paused_ || completed_.has_value(); }) || !paused_)
        return FixtureResult<void>::Failure("fixture pending read not observed");
    return FixtureResult<void>::Success();
}

void Inputs::releaseAndJoin() {
    {
        std::lock_guard guard(mutex_);
        released_ = true;
        condition_.notify_all();
    }
    if (worker_.joinable())
        worker_.join();
}

CaptureResult Inputs::FinishPending() {
    releaseAndJoin();
    std::lock_guard guard(mutex_);
    if (!completed_.has_value()) {
        api::IoFailure failure;
        failure.code = "fixture.no_pending_result";
        failure.effect = api::OutputEffect::NoDurableOutput;
        return CaptureResult::Failure(failure);
    }
    auto result = std::move(completed_.value());
    completed_.reset();
    return result;
}

extern "C" int __real_openat(int parent, const char* name, int flags, ...);
extern "C" ssize_t __real_read(int descriptor, void* bytes, std::size_t count);
extern "C" int __real_close(int descriptor);
extern "C" std::int32_t __real_pipelang_crypto_stream_v2_update(std::uint64_t owner,
    std::uint64_t context, const unsigned char* input, std::size_t length);
extern "C" std::int32_t __real_pipelang_crypto_stream_v2_finish(std::uint64_t owner,
    std::uint64_t context, unsigned char* output, std::size_t length, std::uint64_t* count);

extern "C" int __wrap_openat(int parent, const char* name, int flags, ...) {
    if (active != nullptr)
        active->beforeOpen(name);
    // The read-only production adapter never passes O_CREAT or a mode argument.
    const auto result = __real_openat(parent, name, flags);
    const auto observedErrno = errno;
    if (active != nullptr)
        active->afterOpen(name, result);
    errno = observedErrno;
    return result;
}

extern "C" ssize_t __wrap_read(int descriptor, void* bytes, std::size_t count) {
    if (active != nullptr && active->beforeRead(descriptor, count))
        return -1;
    const auto result = __real_read(descriptor, bytes, count);
    const auto observedErrno = errno;
    if (active != nullptr)
        active->afterRead(descriptor, result);
    errno = observedErrno;
    return result;
}

// glibc's fortified headers call this checked entrypoint when the buffer
// size is known. Keep the original bounds check and the same fixture schedule.
#if defined(__GLIBC__)
extern "C" ssize_t __real___read_chk(int descriptor, void* bytes, std::size_t count, std::size_t capacity);

extern "C" ssize_t __wrap___read_chk(int descriptor, void* bytes, std::size_t count, std::size_t capacity) {
    if (active != nullptr && active->beforeRead(descriptor, count))
        return -1;

    const auto result = __real___read_chk(descriptor, bytes, count, capacity);
    const auto observedErrno = errno;
    if (active != nullptr)
        active->afterRead(descriptor, result);

    errno = observedErrno;
    return result;
}
#endif

extern "C" int __wrap_close(int descriptor) {
    const auto result = __real_close(descriptor);
    const auto observedErrno = errno;
    if (active != nullptr && active->afterClose(descriptor))
        return -1;
    errno = observedErrno;
    return result;
}

extern "C" std::int32_t __wrap_pipelang_crypto_stream_v2_update(std::uint64_t owner,
        std::uint64_t context, const unsigned char* input, std::size_t length) {
    const auto result = __real_pipelang_crypto_stream_v2_update(owner, context, input, length);
    if (active != nullptr)
        active->provider("after_hash");
    return result;
}

extern "C" std::int32_t __wrap_pipelang_crypto_stream_v2_finish(std::uint64_t owner,
        std::uint64_t context, unsigned char* output, std::size_t length, std::uint64_t* count) {
    const auto result = __real_pipelang_crypto_stream_v2_finish(owner, context, output, length, count);
    if (active != nullptr)
        active->provider("after_finish");
    return result;
}
