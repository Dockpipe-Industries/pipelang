// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace pipelang::native_app_v19;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_object_composition;

namespace {

HostedValue get(const HostedInstance& app, TaskScope& scope, std::string_view method)
{
    const auto result = app.startMethod(scope, method, {}).result();
    if (!result.is_success())
        throw std::runtime_error(std::string(method) + ": " + result.error());
    return result.value();
}

void checkInteger(const HostedInstance& app, TaskScope& scope, std::string_view method, std::int64_t expected)
{
    if (get(app, scope, method).integer() != expected)
        throw std::runtime_error("unexpected result for " + std::string(method));
}

}

int main()
{
    try {
        const auto bound = HostedInstance::bind(proof_object_composition, {});
        if (!bound.is_success())
            throw std::runtime_error(bound.error());
        const auto& app = bound.value();
        TaskScope scope;
        checkInteger(app, scope, "InternalNames", 20);
        checkInteger(app, scope, "BagWork", 14101);
        checkInteger(app, scope, "BagState", 14101);
        for (std::size_t attempt = 0; attempt < 4200; ++attempt) {
            const auto failed = app.startMethod(scope, "FailBagMutation", {}).result();
            if (!failed.is_failure() || failed.error() != "bag fault")
                throw std::runtime_error("object graph rollback failed: " + failed.error());
        }
        checkInteger(app, scope, "BagState", 14101);
        checkInteger(app, scope, "UpcastEnumeration", 23);
        checkInteger(app, scope, "PrivateFactory", 42);
        if (get(app, scope, "Text").text() != "pl_scope pl_transaction.commit() [this]")
            throw std::runtime_error("wrong source object string");
        checkInteger(app, scope, "IncrementField", 5);
        std::cout << "The hosted graph preserved list, queue, and dictionary aliases across calls and rollback.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
