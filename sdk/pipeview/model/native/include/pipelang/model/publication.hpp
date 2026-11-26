// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <utility>

namespace pipelang::model {

// Independent of source values and Results, so transactions can capture the
// invocation authority without a runtime/context header dependency cycle.
class PublicationGuard {
public:
    using Check = bool (*)(const void*) noexcept;

    PublicationGuard() = default;
    PublicationGuard(std::shared_ptr<const void> state, Check check) noexcept
        : state_(std::move(state)), check_(check) {}

    bool is_current() const noexcept {
        return !check_ || check_(state_.get());
    }

private:
    std::shared_ptr<const void> state_;
    Check check_ = nullptr;
};

namespace publication_detail {
inline thread_local PublicationGuard currentGuard;
}

inline PublicationGuard currentPublicationGuard() noexcept {
    return publication_detail::currentGuard;
}

class PublicationContextScope {
public:
    explicit PublicationContextScope(PublicationGuard guard) noexcept
        : previous_(currentPublicationGuard()) {
        publication_detail::currentGuard = std::move(guard);
    }

    PublicationContextScope(const PublicationContextScope&) = delete;
    PublicationContextScope& operator=(const PublicationContextScope&) = delete;

    ~PublicationContextScope() {
        publication_detail::currentGuard = std::move(previous_);
    }

private:
    PublicationGuard previous_;
};

}
