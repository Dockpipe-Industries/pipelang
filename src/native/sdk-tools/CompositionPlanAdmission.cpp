// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompositionPlanAdmission.hpp"
#include "CompositionLifecycleAdmission.hpp"
#include "../model-compiler/ResourceBudget.hpp"

#include <limits>

namespace pipelang::tools {
namespace {

std::size_t ordinal(const Json& value, const std::string& label) {
    require(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0),
            label + " must be a nonnegative integer");
    const auto number = value.get<std::uint64_t>();
    require(number <= std::numeric_limits<std::size_t>::max(), label + " exceeds native size range");
    return static_cast<std::size_t>(number);
}

std::string identity(const Json& value, const std::string& label, bool empty = false) {
    require(value.is_string(), label + " must be text");
    const auto& result = value.get_ref<const std::string&>();
    require((empty || !result.empty()) && result.size() <= models::resourceBudget().outputBytes &&
            result.find('\0') == std::string::npos, label + " is empty or exceeds its text bounds");
    return result;
}

void origin(const Json& value) {
    exactObject(value, {"module", "path", "offset"}, {}, "composition origin");
    identity(value.at("module"), "composition origin module", true);
    identity(value.at("path"), "composition origin path", true);
    ordinal(value.at("offset"), "composition origin offset");
}

void account(std::size_t& work, std::size_t additional) {
    require(!models::budgetExceeded(work, additional, models::resourceBudget().tokens),
            "composition metadata tokens resource budget exceeded; configure tokens");
    work += additional;
}

}

void admitCompositionPlans(const Json& plans) {
    require(plans.is_array() && !plans.empty() && plans.size() <= models::resourceBudget().declarations,
            "composition plans must be a bounded nonempty array");
    std::set<std::string> graphNames;
    std::size_t work = 0;
    for (const auto& plan : plans) {
        account(work, 1);
        require(plan.is_object() && plan.contains("protocol"), "composition plan requires a protocol");
        const bool lifecycle = plan.at("protocol") == "pipelang.composition-graph.v2";
        std::set<std::string> planFields{"protocol", "name", "origin", "includes", "services", "exports",
                                         "construction_order", "checkpoint"};
        if (lifecycle)
            planFields.insert("cleanup_contracts");
        exactObject(plan, planFields, {}, "composition plan");
        require((plan.at("protocol") == "pipelang.composition-graph.v1" || lifecycle) &&
                plan.at("checkpoint") == "graph-only", "unsupported composition plan protocol or checkpoint");
        require(graphNames.insert(identity(plan.at("name"), "composition name")).second,
                "duplicate composition plan name");
        origin(plan.at("origin"));
        const auto& includes = plan.at("includes");
        require(includes.is_array(), "composition includes must be an array");
        account(work, includes.size());
        std::set<std::string> included;
        for (const auto& include : includes)
            require(included.insert(identity(include, "included composition")).second,
                    "duplicate included composition metadata");
        const auto& services = plan.at("services");
        require(services.is_array(), "composition services must be an array");
        account(work, services.size());
        std::map<std::string, std::size_t> serviceIndices;
        for (std::size_t index = 0; index < services.size(); ++index) {
            const auto& service = services[index];
            std::set<std::string> serviceFields{"service", "implementation", "lifetime", "input", "constructor", "origin",
                                                "constructor_origin", "dependencies", "root_resolvable", "scope_resolvable",
                                                "context"};
            if (lifecycle) {
                serviceFields.insert("factory");
                serviceFields.insert("cleanup_index");
            }
            exactObject(service, serviceFields, {}, "composition service");
            if (lifecycle)
                require(service.at("factory").is_null() || service.at("factory").is_object(),
                        "composition factory must be null or an object");
            const bool factory = lifecycle && !service.at("factory").is_null();
            require(serviceIndices.emplace(identity(service.at("service"), "service identity"), index).second,
                    "duplicate composition service metadata");
            const auto implementation = identity(service.at("implementation"), "implementation identity", true);
            const auto constructor = identity(service.at("constructor"), "constructor identity", true);
            const auto lifetime = identity(service.at("lifetime"), "service lifetime");
            require(lifetime == "singleton" || lifetime == "scoped" || lifetime == "transient",
                    "unknown composition service lifetime");
            const auto input = identity(service.at("input"), "service input mode");
            require(input == "none" || input == "snapshot" || input == "borrowed", "unknown service input mode");
            const auto context = identity(service.at("context"), "service resolution context");
            require(context == "root" || context == "scope" || context == "caller", "unknown service context");
            std::string expectedContext = "caller";
            if (lifetime == "singleton")
                expectedContext = "root";
            else if (lifetime == "scoped")
                expectedContext = "scope";
            require(context == expectedContext, "service context disagrees with lifetime");
            require(service.at("root_resolvable").is_boolean() && service.at("scope_resolvable").is_boolean(),
                    "service resolvability must be boolean");
            origin(service.at("origin"));
            origin(service.at("constructor_origin"));
            require(service.at("dependencies").is_array(), "service dependencies must be an array");
            if (input == "none") {
                if (lifecycle) {
                    require(!implementation.empty() && (factory ? constructor.empty() : !constructor.empty()),
                            "constructed service requires implementation and exactly one constructor or factory recipe");
                } else {
                    require(!implementation.empty() && !constructor.empty(),
                            "constructed service requires implementation and constructor identities");
                }
            } else {
                require(!factory && lifetime != "transient" && implementation.empty() && constructor.empty() &&
                        service.at("dependencies").empty(), "input service cannot carry a construction recipe");
                const auto& constructorOrigin = service.at("constructor_origin");
                require(constructorOrigin.at("module") == "" && constructorOrigin.at("path") == "" &&
                        ordinal(constructorOrigin.at("offset"), "input constructor offset") == 0,
                        "input service cannot carry a constructor origin");
            }
            if (factory) {
                const auto& constructorOrigin = service.at("constructor_origin");
                require(constructorOrigin.at("module") == "" && constructorOrigin.at("path") == "" &&
                        ordinal(constructorOrigin.at("offset"), "factory constructor offset") == 0,
                        "factory service cannot carry a constructor origin");
            }
            account(work, service.at("dependencies").size());
        }
        for (const auto& service : services) {
            std::set<std::string> parameters;
            for (const auto& dependency : service.at("dependencies")) {
                exactObject(dependency, {"parameter", "type", "service", "index", "origin"}, {},
                            "composition dependency");
                require(parameters.insert(identity(dependency.at("parameter"), "dependency parameter")).second,
                        "duplicate composition dependency parameter");
                identity(dependency.at("type"), "dependency type");
                const auto name = identity(dependency.at("service"), "dependency service");
                const auto index = ordinal(dependency.at("index"), "dependency index");
                require(index < services.size() && serviceIndices.contains(name) && serviceIndices.at(name) == index,
                        "composition dependency refers to an unknown service index");
                origin(dependency.at("origin"));
            }
        }
        if (lifecycle)
            admitCompositionLifecycle(plan, work);
        const auto& exports = plan.at("exports");
        require(exports.is_array(), "composition exports must be an array");
        account(work, exports.size());
        std::set<std::string> exported;
        for (const auto& item : exports) {
            const auto name = identity(item, "composition export");
            require(serviceIndices.contains(name) && exported.insert(name).second,
                    "unknown or duplicate composition export metadata");
        }
        const auto& order = plan.at("construction_order");
        require(order.is_array() && order.size() == services.size(), "incomplete composition construction order");
        account(work, order.size());
        std::set<std::string> visited;
        std::map<std::string, bool> rootAvailability;
        for (const auto& item : order) {
            const auto name = identity(item, "construction order service");
            require(serviceIndices.contains(name) && !visited.contains(name), "unknown or duplicate ordered service");
            const auto& service = services.at(serviceIndices.at(name));
            const auto lifetime = service.at("lifetime").get<std::string>();
            bool rootResolvable = lifetime != "scoped";
            for (const auto& dependency : service.at("dependencies")) {
                const auto dependencyName = dependency.at("service").get<std::string>();
                require(visited.contains(dependencyName),
                        "composition construction order violates dependencies");
                rootResolvable = rootResolvable && rootAvailability.at(dependencyName);
            }
            require(service.at("scope_resolvable").get<bool>(), "composition scope availability is inconsistent");
            require(service.at("root_resolvable").get<bool>() == rootResolvable &&
                    (lifetime != "singleton" || rootResolvable),
                    "composition root availability disagrees with lifetime or dependencies");
            rootAvailability.emplace(name, rootResolvable);
            visited.insert(name);
        }
    }
    for (const auto& plan : plans) {
        for (const auto& include : plan.at("includes"))
            require(graphNames.contains(include.get<std::string>()), "included composition plan is missing");
    }
}

}
