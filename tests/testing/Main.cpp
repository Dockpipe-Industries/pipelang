#include "Model.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        using Model = pipelang_testing::source::PipeLang::Testing::SelfTests::SelfTests;
        using Outcome = pipelang_testing::source::PipeLang::Testing::TestOutcome;
        using Phase = pipelang_testing::source::PipeLang::Testing::TestPhase;
        Model model;
        const auto result = model.Run({});
        if (!result.is_success())
            throw std::runtime_error(result.error());
        const auto cases = result.value();
        if (cases.count() != 5)
            throw std::runtime_error("self-test registration count changed");
        for (const auto& test : cases.values()) {
            if (test.outcome != Outcome::Passed) {
                std::cerr << test.id << ": " << test.primaryFailure.value().message << '\n';
                return 1;
            }
        }
        const auto duplicate = model.DuplicateRegistrationRefused({});
        if (!duplicate.is_success() || !duplicate.value())
            throw std::runtime_error("duplicate registration was not refused");
        const auto deliberate = model.DeliberateFailure({});
        if (!deliberate.is_success())
            throw std::runtime_error(deliberate.error());
        const auto failure = deliberate.value();
        if (failure.outcome != Outcome::Failed || failure.primaryOutcome != Outcome::Failed ||
            failure.primaryPhase != Phase::Body || failure.primaryFailure.value().message != "body" ||
            failure.cleanupFailure.value().message != "cleanup" || !failure.cleanupRan)
            throw std::runtime_error("structured primary and cleanup failure changed");
        pipelang::model::CancellationSource cancellation;
        cancellation.cancel();
        const auto cancelled = model.Run(cancellation.token());
        if (!cancelled.is_success() || cancelled.value().count() != 5)
            throw std::runtime_error("cancelled suite did not retain its case identities");
        for (const auto& test : cancelled.value().values()) {
            if (test.outcome != Outcome::Cancelled || test.bodyRan || test.cleanupRan)
                throw std::runtime_error("cancelled case entered its lifecycle");
        }
        std::cout << "PipeLang authored tests: 5 passed; duplicate, dual failure and cancellation contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
