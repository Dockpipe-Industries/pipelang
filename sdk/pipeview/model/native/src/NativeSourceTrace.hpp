// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <pipelang/model/source_call.hpp>

#include <array>
#include <exception>
#include <list>
#include <span>
#include <string>
#include <string_view>

namespace pipelang::native_app_v19 {

struct SourceTraceFrame {
    const char* function = nullptr;
    const char* file = nullptr;
    std::uint64_t line = 0;
};

class SourceTrace {
    struct Stack {
        void add(SourceTraceFrame frame) noexcept
        {
            if (count < frames.size() && frame.function)
                frames[count++] = frame;
        }

        std::array<SourceTraceFrame, model::SourceCallContext::maximumDepth> frames{};
        std::size_t count = 0;
    };

    struct TracedError {
        std::string message;
        Stack stack;
    };

public:
    void record(const std::exception_ptr& failure, SourceTraceFrame frame) noexcept
    {
        if (failure != failure_) {
            failure_ = failure;
            current_.count = 0;
        }
        current_.add(frame);
    }

    TextView retain(TextView message, const std::exception_ptr& failure) noexcept
    {
        if (failure_ != failure || current_.count == 0)
            return message;
        return retain(message, current_);
    }

    TextView withCaller(TextView message, SourceTraceFrame caller) noexcept
    {
        const auto* origin = find(message);
        if (!origin || !caller.function)
            return message;
        auto stack = origin->stack;
        stack.add(caller);
        return retain(message, stack);
    }

    void publish(TextView message) noexcept
    {
        published_ = find(message);
    }

    std::span<const SourceTraceFrame> frames() const noexcept
    {
        if (!published_)
            return {};
        return {published_->stack.frames.data(), published_->stack.count};
    }

private:
    TextView retain(TextView message, const Stack& stack) noexcept
    {
        if (errors_.size() >= maximumErrors || message.size > maximumTextBytes - retainedTextBytes_)
            return message;
        try {
            const std::string_view bytes(message.bytes ? message.bytes : "", message.size);
            const auto& traced = errors_.emplace_back(TracedError{std::string(bytes), stack});
            retainedTextBytes_ += traced.message.size();
            return {traced.message.data(), traced.message.size()};
        } catch (...) {
            return message;
        }
    }

    const TracedError* find(TextView message) const noexcept
    {
        // Trace strings deliberately use distinct storage. Equal error messages
        // do not imply that one failure propagated from another.
        for (const auto& error : errors_) {
            if (error.message.data() == message.bytes && error.message.size() == message.size)
                return &error;
        }
        return nullptr;
    }

    static constexpr auto maximumErrors = 32;
    static constexpr auto maximumTextBytes = 64 * 1024;
    Stack current_;
    std::exception_ptr failure_;
    std::list<TracedError> errors_;
    std::size_t retainedTextBytes_ = 0;
    const TracedError* published_ = nullptr;
};

}
