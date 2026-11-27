// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"
#include <pipelang/json/ReaderAdapter.hpp>
#include <pipelang/json/StringIndexAdapter.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace api = pipelang_json_tests::source::PipeLang::Json;
namespace tests = pipelang_json_tests::source::PipeLang::Json::Tests;
using ReaderAdapter = pipelang::json::ReaderAdapter<api::JsonReader, api::JsonReadLimits, api::JsonNode, api::JsonKind>;
using IndexAdapter = pipelang::json::StringIndexAdapter<api::JsonStringIndexer, api::JsonStringIndexLimits,
                                                       api::JsonStringIndex, api::JsonNode>;

template<class Result>
void checked(const Result& result) {
    if (result.is_cancelled())
        throw std::runtime_error("source invocation cancelled: " + result.error());

    if (!result.is_success())
        throw std::runtime_error("source invocation failed: " + result.error());

    const auto& assertion = result.value();
    if (assertion.is_cancelled())
        throw std::runtime_error("source assertion cancelled: " + assertion.error().message);

    if (!assertion.is_success())
        throw std::runtime_error("source assertion failed: " + assertion.error().message);
}

pipelang::model::ByteBuffer bytes(const std::string& input) {
    return pipelang::model::ByteBuffer({input.begin(), input.end()});
}

int main() {
    try {
        auto reader = std::make_shared<ReaderAdapter>();
        auto indexer = std::make_shared<IndexAdapter>(reader->nativeReader());
        tests::StringIndexSuite suite(reader, indexer);
        std::string input = "[";
        for (std::int64_t element = 0; element < 4098; ++element) {
            if (element != 0)
                input += ',';

            input += "\"k" + std::to_string(element) + "\"";
        }
        input += ']';

        checked(suite.Open(bytes(input), 4097, {}));
        pipelang::model::CancellationSource cancellation;
        cancellation.cancel();
        checked(suite.Cancelled(0, cancellation.token()));
        for (std::int64_t element = 0; element < 4097; ++element)
            checked(suite.Insert(element, true, {}));

        checked(suite.Insert(4096, false, {}));
        checked(suite.AtCapacity(4097, {}));
        checked(suite.Insert(0, false, {}));
        checked(suite.Close(cancellation.token()));

        checked(suite.Open(bytes(R"(["a","\u0061","a\u0000b","a\u0000c","A","é","e\u0301"])"), 6, {}));
        checked(suite.Insert(0, true, {}));
        checked(suite.Insert(1, false, {}));
        for (std::int64_t element = 2; element < 7; ++element)
            checked(suite.Insert(element, true, {}));
        checked(suite.Insert(1, false, {}));
        checked(suite.CrossDocumentLookup({}, cancellation.token()));
        checked(suite.Close({}));

        if (reader->nativeReader().liveDocuments() != 0 || pipelang::json::Reader::aggregateRetainedBytes() != 0)
            throw std::runtime_error("generated source index cleanup leaked storage");

        std::cout << "JSON generated optional string-index ABI/source contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
