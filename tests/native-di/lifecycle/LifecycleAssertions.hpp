// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResourceEvent.hpp"
#include <pipelang/model/result.hpp>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lifecycle_test {

class LifecycleAssertions {
public:
    void Require(bool condition, const std::string& message) {
        ++count_;
        if (!condition)
            throw std::runtime_error(message);
    }

    template<class T>
    T Value(const pipelang::model::Result<T>& result, const std::string& operation) {
        Require(result.is_success(), operation + ": " + result.error());
        return result.value();
    }

    void Success(const pipelang::model::Result<void>& result, const std::string& operation) {
        Require(result.is_success(), operation + ": " + result.error());
    }

    void Trace(const std::vector<ResourceEvent>& actual, const std::vector<ResourceEvent>& expected) {
        Require(actual.size() == expected.size(), "resource trace length differs: actual=" +
            std::to_string(actual.size()) + ", expected=" + std::to_string(expected.size()));
        for (std::size_t index = 0; index < expected.size(); ++index) {
            Require(actual[index] == expected[index], "resource trace event " + std::to_string(index) +
                " differs: actual=" + Describe(actual[index]) + ", expected=" + Describe(expected[index]));
        }
    }

    void CloseOrder(const std::vector<ResourceEvent>& events, const std::vector<std::int64_t>& expected) {
        std::vector<std::int64_t> observed;
        for (const auto& event : events) {
            if (event.operation == "close-attempt")
                observed.push_back(event.resource);
        }
        Require(observed == expected, "resource close attempts are not the expected exact reverse order");
    }

    std::size_t Count() const {
        return count_;
    }

private:
    static std::string Describe(const ResourceEvent& event) {
        std::ostringstream result;
        result << event.operation << '(' << event.resource << ',' << event.kind << ','
               << event.context << ',' << event.dependency << ')';
        return result.str();
    }

    std::size_t count_ = 0;
};
}
