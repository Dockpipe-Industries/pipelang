// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace typed_host_checks {
using namespace pipelang::model;
using namespace typed_host_errors;
using ReadResult = Result<std::int64_t, DomainError>;
using TouchResult = Result<void, DomainError>;
using KindResult = Result<std::int64_t, ErrorKind>;

inline DomainError domainError(std::int64_t mode) {
    DomainError error;
    error.kind = ErrorKind::Conflict;
    error.message = "version conflict";
    error.origin.provider = "local-test";
    error.origin.request = mode;
    return error;
}

class Host final : public TypedHost {
public:
    ReadResult Read(std::int64_t mode) override {
        ++reads;
        auto error = domainError(mode);
        if (mode == 1)
            return ReadResult::Failure(error);
        if (mode == 2)
            return ReadResult::Cancelled(error);
        if (mode == 3)
            throw std::runtime_error("host execution fault");
        if (mode == 4)
            throw OperationCancelled("host execution cancelled");
        if (mode == 5) {
            error.origin.provider = std::string("\xff", 1);
            return ReadResult::Failure(error);
        }
        if (mode == 6) {
            error.kind = static_cast<ErrorKind>(99);
            return ReadResult::Cancelled(error);
        }
        if (mode == 7)
            return ReadResult::Failure(domainError(700));
        return ReadResult::Success(42);
    }

    TouchResult Touch(std::int64_t mode) override {
        ++touches;
        if (mode == 1)
            return TouchResult::Failure(domainError(mode));
        if (mode == 2)
            return TouchResult::Cancelled(domainError(mode));
        return TouchResult::Success();
    }

    KindResult ReadKind(std::int64_t mode) override {
        if (mode == 1)
            return KindResult::Failure(ErrorKind::Missing);
        if (mode == 2)
            return KindResult::Cancelled(ErrorKind::Conflict);
        return KindResult::Success(17);
    }

    int reads = 0;
    int touches = 0;
};

inline void checkError(const DomainError& error, std::int64_t request) {
    assert(error.kind == ErrorKind::Conflict);
    assert(error.message == "version conflict");
    assert(error.origin.provider == "local-test");
    assert(error.origin.request == request);
}

template<class Model>
void outcomes(Model& model, const std::shared_ptr<Host>& host) {
    const auto success = model.Read(0);
    static_assert(std::is_same_v<std::remove_cv_t<decltype(success)>, Result<ReadResult>>);
    assert(success.is_success() && success.value().is_success() && success.value().value() == 42);
    const auto failed = model.Read(1);
    assert(failed.is_success() && failed.value().is_failure());
    checkError(failed.value().error(), 1);
    const auto cancelled = model.Read(2);
    assert(cancelled.is_success() && cancelled.value().is_cancelled());
    checkError(cancelled.value().error(), 2);
    const auto sameCode = model.Read(7);
    assert(sameCode.is_success() && sameCode.value().is_failure());
    checkError(sameCode.value().error(), 700);
    const auto fault = model.Read(3);
    assert(fault.is_failure() && fault.error() == "host execution fault");
    const auto interrupted = model.Read(4);
    assert(interrupted.is_cancelled() && interrupted.error() == "host execution cancelled");
    assert(model.Read(5).is_failure());
    assert(model.Read(6).is_failure());
    assert(host->reads == 8 && host->touches == 8);
    for (std::int64_t mode = 0; mode < 3; ++mode) {
        const auto touched = model.Touch(mode);
        static_assert(std::is_same_v<std::remove_cv_t<decltype(touched)>, Result<TouchResult>>);
        assert(touched.is_success());
        const auto kind = model.ReadKind(mode);
        assert(kind.is_success());
        if (mode == 0) {
            assert(touched.value().is_success());
            assert(kind.value().is_success() && kind.value().value() == 17);
        } else if (mode == 1) {
            assert(touched.value().is_failure());
            checkError(touched.value().error(), 1);
            assert(kind.value().is_failure() && kind.value().error() == ErrorKind::Missing);
        } else {
            assert(touched.value().is_cancelled());
            checkError(touched.value().error(), 2);
            assert(kind.value().is_cancelled() && kind.value().error() == ErrorKind::Conflict);
        }
    }
}
}
