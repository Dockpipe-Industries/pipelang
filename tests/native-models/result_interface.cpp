// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ResultInterface.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ResultInterfaceChecks.hpp"
#include <functional>
#include <optional>
#include <type_traits>
#include <vector>

namespace {
struct PriorResultLayout {
    pipelang::model::Outcome outcome;
    std::optional<std::int64_t> value;
    std::string error;
};
using Typed = pipelang::model::Result<std::int64_t>;
static_assert(sizeof(Typed) == sizeof(PriorResultLayout));
static_assert(alignof(Typed) == alignof(PriorResultLayout));
static_assert(!std::is_polymorphic_v<Typed>);
constexpr auto constant = pipelang::model::Result<std::int64_t,result_interface::ReadCode>::Success(9);
static_assert(constant.is_success() && constant.value() == 9);

template<class Function>
void refuses(Function function, const std::string& expected) {
    bool failed = false;
    try {
        function();
    } catch (const pipelang::model::ExecutionFailure& error) {
        failed = true;
        assert(error.what() == expected);
    }
    assert(failed);
}
}

int main() {
    using namespace result_interface_checks;
    ResultInterfaceModel model;
    boundary(model);
    const auto success = model.Number(Result<std::int64_t,ReadError>::Success(7));
    assert(success.is_success());
    const auto failed = model.Text(Result<std::string,ReadError>::Failure(error(42)));
    assert(failed.is_failure());
    exact(failed, 42);
    const auto cancelled = model.Nothing(Result<void,ReadError>::Cancelled(error(43)));
    assert(cancelled.is_cancelled());
    exact(cancelled, 43);
    assert(model.NumberMessage(Result<std::int64_t,ReadError>::Failure(error(42))) == "version conflict");
    assert(model.TextMessage(Result<std::string,ReadError>::Cancelled(error(43))) == "version conflict");
    assert(model.Retain(Result<std::int64_t,ReadError>::Success(19)) == 19);
    assert(model.Snapshot() && model.Identity());
    const auto outcomes = model.Outcomes();
    assert(outcomes.count() == 3 && outcomes.at(0).is_success());
    assert(outcomes.at(1).is_failure() && outcomes.at(1).error().request == 42);
    assert(outcomes.at(2).is_cancelled() && outcomes.at(2).error().request == 43);
    assert(model.CountFailures(outcomes) == 1);
    const auto escaped = ResultInterfaceModel{}.Temporary();
    assert(escaped.is_failure());
    exact(escaped, 41);
    const auto stringError = model.String(Result<std::int64_t>::Cancelled("read cancelled"));
    assert(stringError.is_cancelled() && stringError.error() == "read cancelled");
    auto invalid = error(55);
    invalid.message = std::string("\xff", 1);
    refuses([&] { (void)IResult<ReadError>::From(Result<void,ReadError>::Failure(invalid)); },
        "string contains invalid UTF-8");
    invalid = error(56);
    invalid.code = static_cast<ReadCode>(99);
    refuses([&] { (void)IResult<std::string>::From(Result<ReadError>::Success(invalid)); },
        "invalid enum value");
    const pipelang::model::ByteBuffer maximum(std::vector<std::uint8_t>(pipelang::model::maximumListBytes, 1));
    refuses([&] { (void)IResult<std::string>::From(Result<pipelang::model::ByteBuffer>::Success(maximum)); },
        "result interface exceeds owned value limits");
    pipelang::model::validate(outcomes);
    assert(pipelang::model::ownedBytes(outcomes) > sizeof(outcomes));
    assert(pipelang::model::ownedElements(outcomes) >= 3);
}
