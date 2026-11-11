// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompositionLifecycleAdmission.hpp"
#include "../model-compiler/ResourceBudget.hpp"
#include <limits>

namespace pipelang::tools {
namespace {
std::string identity(const Json& value, const std::string& label) {
    require(value.is_string(), label + " must be text");
    const auto& result = value.get_ref<const std::string&>();
    require(!result.empty() && result.size() <= models::resourceBudget().outputBytes &&
            result.find('\0') == std::string::npos, label + " is empty or exceeds its text bounds");
    return result;
}

std::size_t ordinal(const Json& value, const std::string& label) {
    require(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0),
            label + " must be a nonnegative integer");
    const auto number = value.get<std::uint64_t>();
    require(number <= std::numeric_limits<std::size_t>::max(), label + " exceeds native size range");
    return static_cast<std::size_t>(number);
}

void account(std::size_t& work, std::size_t amount) {
    require(!models::budgetExceeded(work, amount, models::resourceBudget().tokens),
            "composition lifecycle metadata tokens resource budget exceeded; configure tokens");
    work += amount;
}

void origin(const Json& value) {
    exactObject(value, {"module", "path", "offset"}, {}, "composition lifecycle origin");
    for (const auto& field : {"module", "path"}) {
        require(value.at(field).is_string(), "composition lifecycle origin must contain text");
        const auto& text = value.at(field).get_ref<const std::string&>();
        require(text.size() <= models::resourceBudget().outputBytes && text.find('\0') == std::string::npos,
                "composition lifecycle origin text exceeds bounds");
    }
    ordinal(value.at("offset"), "composition lifecycle offset");
}
}

void admitCompositionLifecycle(const Json& plan, std::size_t& work) {
    const auto& contracts = plan.at("cleanup_contracts");
    require(contracts.is_array(), "composition cleanup contracts must be an array");
    account(work, contracts.size());
    std::set<std::pair<std::string, std::string>> selectors;
    for (const auto& contract : contracts) {
        exactObject(contract, {"contract", "method", "signature", "origin", "selector_origin"}, {},
                    "composition cleanup contract");
        const auto type = identity(contract.at("contract"), "cleanup contract type");
        const auto method = identity(contract.at("method"), "cleanup contract method");
        identity(contract.at("signature"), "cleanup contract signature");
        require(selectors.emplace(type, method).second, "duplicate composition cleanup selector");
        origin(contract.at("origin"));
        origin(contract.at("selector_origin"));
    }

    const auto& services = plan.at("services");
    for (const auto& service : services) {
        const bool input = service.at("input") != "none";
        const auto& cleanup = service.at("cleanup_index");
        if (!cleanup.is_null()) {
            require(!input, "borrowed or snapshot input cannot be owned for cleanup");
            require(ordinal(cleanup, "service cleanup index") < contracts.size(),
                    "service cleanup refers to an unknown contract");
        }
        const auto& factory = service.at("factory");
        if (factory.is_null())
            continue;
        require(!input, "input service cannot carry a factory recipe");
        exactObject(factory, {"receiver", "receiver_index", "method", "signature", "origin", "parameters"}, {},
                    "composition factory");
        const auto receiver = identity(factory.at("receiver"), "factory receiver");
        const auto receiverIndex = ordinal(factory.at("receiver_index"), "factory receiver index");
        require(receiverIndex < services.size() && services.at(receiverIndex).at("service") == receiver,
                "factory receiver refers to an unknown service");
        identity(factory.at("method"), "factory method");
        identity(factory.at("signature"), "factory signature");
        origin(factory.at("origin"));
        const auto& dependencies = service.at("dependencies");
        require(!dependencies.empty(), "factory recipe requires an explicit receiver dependency");
        const auto& receiverDependency = dependencies.at(0);
        require(receiverDependency.at("parameter") == "$factoryReceiver" &&
                receiverDependency.at("index") == receiverIndex &&
                receiverDependency.at("service") == receiver && receiverDependency.at("type") == receiver,
                "factory receiver dependency disagrees with its checked recipe");

        const auto& parameters = factory.at("parameters");
        require(parameters.is_array(), "factory parameters must be an array");
        account(work, parameters.size());
        std::set<std::string> names;
        std::size_t dependencyIndex = 1;
        bool cancellationSeen = false;
        for (const auto& parameter : parameters) {
            exactObject(parameter, {"name", "type", "index"}, {}, "composition factory parameter");
            const auto name = identity(parameter.at("name"), "factory parameter name");
            require(name != "$factoryReceiver" && names.insert(name).second,
                    "duplicate or reserved factory parameter name");
            const auto type = identity(parameter.at("type"), "factory parameter type");
            if (parameter.at("index").is_null()) {
                require(type == "CancellationToken" && !cancellationSeen,
                        "only one direct CancellationToken may omit a factory dependency index");
                cancellationSeen = true;
                continue;
            }
            require(type != "CancellationToken", "factory cancellation must not be a registered service");
            const auto index = ordinal(parameter.at("index"), "factory parameter service index");
            require(index < services.size() && dependencyIndex < dependencies.size(),
                    "factory parameter refers to an unknown dependency");
            const auto& dependency = dependencies.at(dependencyIndex++);
            require(dependency.at("parameter") == name && dependency.at("type") == type &&
                    dependency.at("index") == index && dependency.at("service") == services.at(index).at("service"),
                    "factory parameters disagree with ordered dependency recipes");
        }
        require(dependencyIndex == dependencies.size(), "factory recipe contains unmatched dependencies");
    }
}
}
