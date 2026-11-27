// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Reader.hpp"

#include <pipelang/model/cancellation.hpp>
#include <pipelang/model/result.hpp>

#include <cstdint>
#include <new>
#include <string_view>
#include <utility>

namespace pipelang::json {

// Composition owns the sink and the Reader. Cancellation is observed between
// delivered fragments. Callers close or abort their staged output separately.
template<class Contract, class Node, class Options, class Layout>
class ValueRendererAdapter final : public Contract {
    using Result = pipelang::model::Result<std::int64_t, std::string>;
    using Token = pipelang::model::CancellationToken;

public:
    ValueRendererAdapter(Reader& reader, JsonByteSink sink)
        : reader_(reader), sink_(std::move(sink)) {}

    Result WriteValue(Node value, Options options, Token token) override {
        if (options.maximumBytes <= 0 || options.initialDepth < 0 ||
            static_cast<std::uint64_t>(options.maximumBytes) > Reader::maximumRenderBytes ||
            static_cast<std::uint64_t>(options.initialDepth) > Reader::maximumRenderInitialDepth)
            return Result::Failure("json.render.options");

        RenderLayout layout;
        switch (options.layout) {
        case Layout::Compact:
            layout = RenderLayout::Compact;
            break;
        case Layout::Spaced:
            layout = RenderLayout::Spaced;
            break;
        case Layout::Indented:
            layout = RenderLayout::Indented;
            break;
        default:
            return Result::Failure("json.render.options");
        }

        try {
            const RenderOptions render{static_cast<std::size_t>(options.maximumBytes), layout,
                options.ascii, static_cast<std::size_t>(options.initialDepth)};
            const auto bytes = reader_.render({value.document, value.node}, render, sink_,
                [&] { return token.is_cancelled(); });
            return Result::Success(static_cast<std::int64_t>(bytes));
        } catch (const ReadFailure& error) {
            if (std::string_view(error.what()) == "json.cancelled")
                return Result::Cancelled(error.what());

            return Result::Failure(error.what());
        } catch (const std::bad_alloc&) {
            return Result::Failure("json.memory_limit");
        }
    }

private:
    Reader& reader_;
    JsonByteSink sink_;
};

} // namespace pipelang::json
