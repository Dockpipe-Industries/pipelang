// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Fixture.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

std::string json(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char byte : text) {
        if (byte == '"' || byte == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(byte));
        } else if (byte < 0x20) {
            result += "\\u00";
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 15]);
        } else {
            result.push_back(static_cast<char>(byte));
        }
    }

    result.push_back('"');
    return result;
}

template<class OptionalFailure>
void writeFailure(const OptionalFailure& failure) {
    if (!failure.has_value()) {
        std::cout << "null";
        return;
    }

    std::cout << "{\"code\":" << json(failure.value().code)
              << ",\"message\":" << json(failure.value().message) << '}';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 || !std::filesystem::path(argv[1]).is_absolute())
            throw std::runtime_error("Select a fresh absolute private fixture directory");

        const std::string estate = argv[1];
        const std::string root = estate + "/root";
        if (::mkdir(estate.c_str(), 0700) != 0 || ::mkdir(root.c_str(), 0700) != 0)
            throw std::runtime_error("Could not create the private fixture directories");

        auto writer = std::make_shared<Writer>(root, pipelang::io::PathProfile{4096, 32, 4096});
        auto reader = std::make_shared<Reader>(root, pipelang::io::PathProfile{4096, 32, 262144});
        auto fixture = std::make_shared<Fixture>(root);
        // Explicit existing predicate profile for this finite whole-suite fixture.
        // Source cases remain one invocation; initialization/cleanup stay 65536.
        pipelang::model::TaskOptions options;
        options.evaluationStepsPerSegment = 33554432;
        tests::OutputSuite suite(writer, reader, fixture, options);
        const auto evaluated = suite.Run({}, fixture->cancelled(),
            fixture->probe(0), fixture->probe(1), fixture->probe(2));
        if (!evaluated.is_success())
            throw std::runtime_error(evaluated.error());

        const auto& report = evaluated.value();
        const pipelang::model::TaskOptions defaults;
        std::cout << "{\"schema\":\"pipelang.exclusive-output-source-contracts/v1\",\"allPassed\":"
                  << (report.allPassed ? "true" : "false")
                  << ",\"profile\":{\"evaluationStepsPerSegment\":" << options.evaluationStepsPerSegment
                  << ",\"initializationSteps\":" << options.initializationSteps
                  << ",\"defaultEvaluationStepsPerSegment\":" << defaults.evaluationStepsPerSegment
                  << ",\"cleanupSteps\":" << pipelang::model::EvaluationBudget::maximumSteps
                  << "},\"results\":[";
        bool first = true;
        for (const auto& result : report.results.values()) {
            if (!first)
                std::cout << ',';

            first = false;
            std::cout << "{\"id\":" << json(result.id)
                      << ",\"name\":" << json(result.name)
                      << ",\"outcome\":" << static_cast<std::int64_t>(result.outcome)
                      << ",\"primaryOutcome\":" << static_cast<std::int64_t>(result.primaryOutcome)
                      << ",\"primaryPhase\":" << static_cast<std::int64_t>(result.primaryPhase)
                      << ",\"primaryFailure\":";
            writeFailure(result.primaryFailure);
            std::cout << ",\"cleanupOutcome\":" << static_cast<std::int64_t>(result.cleanupOutcome)
                      << ",\"cleanupFailure\":";
            writeFailure(result.cleanupFailure);
            std::cout << ",\"bodyRan\":" << (result.bodyRan ? "true" : "false")
                      << ",\"cleanupRan\":" << (result.cleanupRan ? "true" : "false") << '}';
        }

        std::cout << "]}\n";

        return report.allPassed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Exclusive output Source consumer infrastructure: " << error.what() << '\n';
        return 2;
    }
}
