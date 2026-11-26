// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "BindingModel.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace pipelang::qt_bindings;
using Json = nlohmann::json;
std::size_t assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value)
        throw std::runtime_error(message);
}
Json manifest() {
    return {{"protocol", "pipelang.native-model-manifest.v2"}, {"namespace", "source_model"},
        {"classes", Json::array({{{"name", "pl_module_type_3"}, {"sourceName", "Example.Presentation.State"},
            {"fields", Json::array({{{"name", "Title"}, {"type", "string"}}, {{"name", "Pages"}, {"type", "List<string>"}}})},
            {"methods", Json::array({{{"name", "Navigate"}, {"returnType", "Result<void,string>"},
                {"pure", false}, {"async", false}, {"parameters", Json::array({{{"name", "page"}, {"type", "string"}}})}},
                {{"name", "Check"}, {"returnType", "bool"}, {"pure", true}, {"async", false},
                    {"parameters", Json::array({{{"name", "permission"}, {"type", "string"}}})}}})}}})}};
}
void rejected(Json input, const std::string& cause) {
    try {
        (void)admit(input, "Example.Presentation.State");
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(cause) != std::string::npos, "unexpected metadata refusal");
        return;
    }
    throw std::runtime_error("unsupported Qt metadata accepted");
}
}
int main() {
    try {
        auto value = manifest();
        const auto model = admit(value, "Example.Presentation.State");
        require(model.nativeModel == "pl_module_type_3" && model.properties.size() == 2, "canonical model identity lost");
        const auto header = emit(model, "qt_fixture", "StateQt");
        require(header.find("using Model = ::source_model::pl_module_type_3") != std::string::npos, "checked root identity not emitted");
        require(header.find("Q_PROPERTY(QStringList Pages") != std::string::npos, "owned string-list projection missing");
        require(header.find("qt.wrong_owner") != std::string::npos && header.find("qt.reentry") != std::string::npos, "owner/reentry guards missing");
        const auto query = header.substr(header.find("ActionReport Check("));
        require(query.find("pl_group->publish()") == std::string::npos,
            "pure query emitted projection notifications");
        require(query.find("pl_group->invoking") == std::string::npos &&
            query.find("QScopedValueRollback<bool>") == std::string::npos,
            "committed pure queries must remain available during effect projection notification");
        require(header.find("public pipelang::qt::ContextualObject") != std::string::npos &&
            header.find("const auto pl_group = pl_group_") != std::string::npos,
            "context receiver or invocation model lifetime is missing");
        require(header.find("pl_binding.supervisedRoot ? pipelang::model::InvocationPermit{} : pl_binding.permit") != std::string::npos &&
            header.find("pl_binding.permit.requireCurrent();") != std::string::npos &&
            header.find("catch (const pipelang::model::StaleContextCancelled&)") != std::string::npos,
            "captured context does not guard source invocation");
        require(header.find("QScopedValueRollback<bool> pl_guard(pl_group->invoking, true)") != std::string::npos,
            "operation clones do not share the reentry guard");
        auto context = value;
        context["classes"][0]["methods"].push_back({{"name", "Lifetime"}, {"returnType", "ContextOwner"},
            {"pure", true}, {"async", false}, {"parameters", Json::array()}});
        const auto contextual = admit(context, "Example.Presentation.State");
        require(contextual.properties.back().type == "ContextOwner" && contextual.properties.back().getter,
            "opaque context owner getter was rejected");
        const auto contextHeader = emit(contextual, "qt_fixture", "StateQt");
        require(contextHeader.find("Q_PROPERTY(QObject* Lifetime") != std::string::npos &&
            contextHeader.find("ContextObject::capture(pl_group->model->Lifetime(), contextRoot())") != std::string::npos,
            "context owner was serialized instead of captured by the framework");
        require(contextHeader.find("pl_value_Lifetime.clear()") != std::string::npos,
            "expired receiver keeps an old context projection");
        auto invalidContext = context;
        invalidContext["classes"][0]["methods"].back()["pure"] = false;
        rejected(invalidContext, "string-error");
        invalidContext = context;
        invalidContext["classes"][0]["methods"].back()["parameters"].push_back({{"name", "request"}, {"type", "string"}});
        rejected(invalidContext, "string-error");
        invalidContext = value;
        invalidContext["classes"][0]["fields"][0]["type"] = "ContextOwner";
        rejected(invalidContext, "properties require");
        invalidContext = value;
        invalidContext["classes"][0]["methods"][0]["parameters"][0]["type"] = "ContextOwner";
        rejected(invalidContext, "action input");
        invalidContext = value;
        invalidContext["classes"][0]["methods"][0]["name"] = "latest";
        rejected(invalidContext, "unique synchronous");
        invalidContext = value;
        invalidContext["classes"][0]["methods"][0]["name"] = "captureSupervisor";
        rejected(invalidContext, "unique synchronous");
        auto changed = value;
        changed["protocol"] = "unknown";
        rejected(changed, "protocol");
        changed = value;
        changed["classes"][0]["fields"][0]["name"] = "escape;statement";
        rejected(changed, "properties require");
        changed = value;
        changed["classes"][0]["fields"][0]["type"] = "SecretReference";
        rejected(changed, "properties require");
        changed = value;
        changed["classes"][0]["methods"][0]["async"] = true;
        rejected(changed, "synchronous");
        changed = value;
        changed["classes"][0]["methods"][0]["returnType"] = "Result<bool,DomainError>";
        rejected(changed, "string-error");
        changed = value;
        changed["classes"][0]["methods"][1]["name"] = "Navigate";
        rejected(changed, "unique synchronous");
        bool missing = false;
        try { (void)admit(value, "Other.State"); } catch (const std::exception&) { missing = true; }
        require(missing, "unknown source model selected another root");
        std::cout << "Qt binding metadata assertions=" << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
