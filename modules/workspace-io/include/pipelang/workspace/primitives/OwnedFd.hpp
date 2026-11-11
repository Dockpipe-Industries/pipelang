// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <unistd.h>
#include <utility>

namespace pipelang::workspace {
class OwnedFd final {
    int descriptor_ = -1;
public:
    OwnedFd() = default;
    explicit OwnedFd(int descriptor) : descriptor_(descriptor) {}
    ~OwnedFd() {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0)
                ::close(descriptor_);
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    int get() const { return descriptor_; }
    explicit operator bool() const { return descriptor_ >= 0; }
};
}
