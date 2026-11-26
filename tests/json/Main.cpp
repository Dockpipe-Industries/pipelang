// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"
#include "FixtureInputs.hpp"
#include <pipelang/json/ReaderAdapter.hpp>

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace api = pipelang_json_tests::source::PipeLang::Json;
namespace tests = pipelang_json_tests::source::PipeLang::Json::Tests;
using Adapter = pipelang::json::ReaderAdapter<api::JsonReader, api::JsonReadLimits, api::JsonNode, api::JsonKind>;

// Native fixture supplies bytes and a genuine cancelled-token call. Expected admission, traversal,
// conversion, ownership and cleanup outcomes are asserted in authored PipeLang.
class Fixture final : public tests::FixtureBytes {
    template<class T> using Result = pipelang::model::Result<T, std::string>;
    using Buffer = pipelang::model::ByteBuffer;

public:
    explicit Fixture(std::shared_ptr<api::JsonReader> reader)
        : reader_(std::move(reader)), inputs_(json_contracts::fixtureInputs()) {}

    Result<std::int64_t> Length(std::int64_t scenario) override {
        if (scenario < 0 || static_cast<std::uint64_t>(scenario) >= inputs_.size())
            return Result<std::int64_t>::Failure("fixture.scenario");
        return Result<std::int64_t>::Success(static_cast<std::int64_t>(inputs_[scenario].size()));
    }

    Result<Buffer> Slice(std::int64_t scenario, std::int64_t start, std::int64_t count) override {
        const auto length = Length(scenario);
        if (!length.is_success() || start < 0 || count < 0 || count > 1048576 ||
            start > length.value() || count > length.value() - start)
            return Result<Buffer>::Failure("fixture.slice");
        const auto fragment = inputs_[scenario].substr(static_cast<std::size_t>(start), static_cast<std::size_t>(count));
        return Result<Buffer>::Success(Buffer({fragment.begin(), fragment.end()}));
    }

    Result<api::JsonNode> CompleteCancelled(std::int64_t document) override {
        pipelang::model::CancellationSource cancellation;
        cancellation.cancel();
        return reader_->CompleteDocument(document, cancellation.token());
    }

private:
    std::shared_ptr<api::JsonReader> reader_;
    std::array<std::string, 41> inputs_;
};

int main() {
    try {
        auto reader = std::make_shared<Adapter>(pipelang::json::ReaderOptions{2});
        auto other = std::make_shared<Adapter>();
        auto fixture = std::make_shared<Fixture>(reader);
        tests::JsonSuite suite(reader, other, fixture);
        const auto result = suite.Run({});
        if (!result.is_success())
            throw std::runtime_error(result.error());
        if (result.value().count() != 42)
            throw std::runtime_error("source test registration count changed");
        for (const auto& test : result.value().values()) {
            if (test.outcome == pipelang_json_tests::source::PipeLang::Testing::TestOutcome::Passed)
                continue;
            std::cerr << test.id;
            if (test.primaryFailure.has_value())
                std::cerr << ": " << test.primaryFailure.value().message;
            if (test.cleanupFailure.has_value())
                std::cerr << " cleanup: " << test.cleanupFailure.value().message;
            std::cerr << '\n';
            return 1;
        }
        if (reader->nativeReader().liveDocuments() != 0 || other->nativeReader().liveDocuments() != 0)
            throw std::runtime_error("source cleanup left a document open");
        std::cout << "PipeLang.Json generated ABI/source query contracts: 42 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
