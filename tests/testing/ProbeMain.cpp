#include "Model.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tests = pipelang_testing::source::PipeLang::Testing::SelfTests;
namespace {

// Four real native owners correspond exactly to the four independently mutable
// token parameters of ProbeSuite.Run. Source chooses every cancellation action.
class CancellationControl final : public tests::ProbeCancellationControl {
public:
    pipelang::model::Result<void, std::string> RequestCancellation(std::int64_t scope) override {
        if (scope < 0 || static_cast<std::uint64_t>(scope) >= sources_.size())
            return pipelang::model::Result<void, std::string>::Failure("probe.cancellation_scope");

        sources_[static_cast<std::size_t>(scope)].cancel();
        return pipelang::model::Result<void, std::string>::Success();
    }

    pipelang::model::CancellationToken token(std::size_t index) const {
        return sources_.at(index).token();
    }

private:
    std::array<pipelang::model::CancellationSource, 4> sources_;
};

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

int main() {
    try {
        auto control = std::make_shared<CancellationControl>();
        tests::ProbeSuite suite(control);
        const auto evaluated = suite.Run({}, control->token(0), control->token(1),
            control->token(2), control->token(3));
        if (!evaluated.is_success())
            throw std::runtime_error(evaluated.error());

        const auto& report = evaluated.value();
        std::cout << "{\"schema\":\"pipelang.testing-probe-contracts/v1\",\"allPassed\":"
                  << (report.allPassed ? "true" : "false") << ",\"results\":[";
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
        std::cerr << "Probe source consumer infrastructure: " << error.what() << '\n';
        return 2;
    }
}
