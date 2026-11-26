// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <pipelang/json/ReaderAdapter.hpp>
#include <pipelang/json/ValueRendererAdapter.hpp>

#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace api = pipelang_json_tests::source::PipeLang::Json;
using Reader = pipelang::json::ReaderAdapter<api::JsonReader, api::JsonReadLimits, api::JsonNode, api::JsonKind>;
using Renderer = pipelang::json::ValueRendererAdapter<api::JsonValueRenderer, api::JsonNode,
    api::JsonRenderOptions, api::JsonRenderLayout>;

int main() {
    try {
        auto reader = std::make_shared<Reader>();
        std::string emitted;
        auto renderer = std::make_shared<Renderer>(reader->nativeReader(), [&](std::span<const std::uint8_t> bytes) {
            emitted.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        });
        api::RenderTests::JsonRenderSuite suite(reader, renderer);
        pipelang::model::CancellationSource cancellation;
        cancellation.cancel();
        const auto invoked = suite.Exercise({}, cancellation.token());
        if (!invoked.is_success())
            throw std::runtime_error(invoked.error());
        if (emitted != R"({"a": "\u20ac", "z": []})" || reader->nativeReader().liveDocuments() != 0 ||
            pipelang::json::Reader::aggregateRetainedBytes() != 0)
            throw std::runtime_error("Source output or document lifetime changed");

        std::cout << "Generated JSON value renderer Source contract passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
