// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../src/native/support/support.hpp"
#include <string>
#include <utility>
#include <vector>

namespace pipelang::tests {

// Uses the existing process oracle and installed compiler, not a second checker.
template <typename Oracle>
void nativeXmlViewTests(Oracle &oracle, const tools::Json &declarativeRequest,
                        const tools::Json &declarativeResponse)
{
    using tools::Json;
    using tools::require;
    const auto replace = [](std::string source, const std::string &before, const std::string &after) {
        const auto position = source.find(before);
        require(position != std::string::npos, "XML test mutation did not match: " + before);
        source.replace(position, before.size(), after);
        return source;
    };
    const auto source = oracle.source("tests/native-model-views/Fixture.pxml");
    auto xml = declarativeRequest;
    xml["source_format"] = "pxml";
    xml["source"] = source;
    const auto plain = oracle.admitted("XmlView", xml, true);
    require(plain == declarativeResponse,
        "XML changed typed adapter bytes, action/cancellation transport or view identity");

    auto explicitDeclarative = declarativeRequest;
    explicitDeclarative["source_format"] = "pview";
    require(oracle.admitted("ExplicitDeclarativeView", explicitDeclarative, true) == plain,
        "explicit pview format changed legacy protocol behavior");

    auto styledXml = xml;
    styledXml["source"] = replace(replace(source, "profile=", "style=\"Compact\" profile="),
        "    <Column", oracle.source("tests/native-model-views/Theme.pxml") + "    <Column");
    styledXml["style_inspection"] = "pipelang.native-style-descriptor.v1";
    auto styledDeclarative = declarativeRequest;
    styledDeclarative["source"] = oracle.source("tests/native-model-views/Theme.pview")
        + replace(declarativeRequest.at("source").template get<std::string>(),
                  "FixtureModel) {", "FixtureModel) style Compact {");
    styledDeclarative["style_inspection"] = "pipelang.native-style-descriptor.v1";
    const auto themed = oracle.admitted("XmlStyledView", styledXml, true);
    require(themed == oracle.admitted("XmlEquivalentDeclarativeStyle", styledDeclarative, true),
        "XML style inheritance, overrides or descriptor differ from declarative styles");

    const auto parity = [&](const std::string &id, const std::string &xmlSource,
                             const std::string &declarativeSource) {
        auto markup = xml;
        markup["source"] = xmlSource;
        auto text = declarativeRequest;
        text["source"] = declarativeSource;
        require(oracle.admitted("Xml" + id, markup, true)
            == oracle.admitted("Declarative" + id, text, true), "XML parity failed: " + id);
    };
    const auto declarativeSource = declarativeRequest.at("source").template get<std::string>();
    parity("Escaping", replace(source, "label=\"Save\"",
        "label=\"Caf&#233; &amp; &lt; &gt; &quot; &apos; &#x1F3CD;\""),
        replace(declarativeSource, "label: \"Save\"", "label: \"Café & < > \\\" ' 🏍\""));
    parity("Whitespace", replace(source, "label=\"Save\"", "label=\"A\r\nB\tC&#10;D\""),
        replace(declarativeSource, "label: \"Save\"", "label: \"A B C\\nD\""));
    parity("Dimensions", replace(source, "id=\"root\"", "id=\"root\" width=\"640\" height=\"480\""),
        replace(declarativeSource, "id: \"root\"", "id: \"root\", width: 640, height: 480"));
    parity("Disabled", replace(source, "enabled=\"{bind model.CanSave()}\"", "enabled=\"false\""),
        replace(declarativeSource, "enabled: bind model.CanSave()", "enabled: false"));
    parity("XmlDeclaration", "\xEF\xBB\xBF<?xml version='1.0' encoding='UTF-8'?>\n<!-- typed view -->\n" + source,
        declarativeSource);
    parity("PairedLeaf", replace(source, "<Label id=\"error\" value=\"{bind model.Error}\"/>",
        "<Label id=\"error\" value=\"{bind model.Error}\"><!-- empty --></Label>"), declarativeSource);

    const auto refusal = [&](const std::string &id, const std::string &candidate,
                              const std::string &message = "") {
        auto request = xml;
        request["source"] = candidate;
        oracle.refused("xml/" + id, request, true, message);
    };
    const auto mutation = [&](const std::string &id, const std::string &before,
                               const std::string &after, const std::string &message = "") {
        refusal(id, replace(source, before, after), message);
    };
    mutation("unknown-profile", "pipelang.native-model-view.v1", "pipelang.pipeview.source.v2", "historical State/Action");
    mutation("missing-model", " model=\"FixtureModel\"", "", "missing XML attribute: model");
    mutation("missing-profile", " profile=\"pipelang.native-model-view.v1\"", "", "missing XML attribute: profile");
    mutation("unknown-model", "model=\"FixtureModel\"", "model=\"Missing\"", "public model class");
    mutation("duplicate-attribute", "id=\"root\"", "id=\"root\" id=\"again\"", "duplicate XML attribute");
    mutation("attribute-separation", "name=\"FixtureView\" model=", "name=\"FixtureView\"model=", "whitespace separation");
    mutation("unknown-attribute", "id=\"root\"", "id=\"root\" mystery=\"1\"", "unknown XML attribute");
    mutation("invalid-boolean", "sensitive=\"true\"", "sensitive=\"yes\"", "Boolean attribute");
    mutation("negative-dimension", "id=\"root\"", "id=\"root\" width=\"-1\"", "nonnegative decimal");
    mutation("oversized-dimension", "id=\"root\"", "id=\"root\" width=\"8193\"", "dimension outside");
    mutation("identity-collision", "id=\"error\"", "id=\"name\"", "explicit and unique");
    mutation("unsafe-identifier", "name=\"FixtureView\"", "name=\"class\"", "unsafe code identifier");
    mutation("unknown-field", "model.Draft.Name", "model.Draft.Missing", "unknown public typed model field");
    mutation("wrong-binding-type", "model.Draft.Name", "model.Saving", "wrong scalar type");
    mutation("impure-getter", "model.CanSave()", "model.Fail()", "pure zero-argument scalar getter");
    mutation("unknown-method", "{model.Save}", "{model.Missing}", "public effect method");
    mutation("wrong-event-payload", "{model.Save}", "{model.SetName}", "no event payload");
    mutation("unbraced-reference", "{model.Save}", "model.Save", "require braces");
    mutation("attribute-injection", "{model.Save}", "{model.Save, label: &quot;Injected&quot;}", "trailing tokens");
    mutation("expression-injection", "{bind model.Draft.Name}", "{bind model.Draft.Name, sensitive: false}", "trailing tokens");
    mutation("state-declaration", "<Column id=\"root\">", "<State id=\"root\">", "historical XML State/Action");
    mutation("action-declaration", "<Column id=\"root\">", "<Action id=\"root\">", "historical XML State/Action");
    mutation("unknown-control", "<Column id=\"root\">", "<Grid id=\"root\">", "unsupported XML control");
    mutation("closing-tag", "</Row>", "</Column>", "mismatched XML closing tag");
    mutation("leaf-content", "<Label id=\"error\" value=\"{bind model.Error}\"/>",
        "<Label id=\"error\" value=\"{bind model.Error}\">ignored</Label>", "cannot contain text");
    mutation("multiple-roots", "</View>", "<Label id=\"extra\" text=\"No\"/></View>");
    mutation("namespace", "<View ", "<View xmlns=\"urn:pipeview\" ", "unknown XML attribute");
    mutation("raw-angle", "label=\"Save\"", "label=\"<Save\"", "raw '<'");
    mutation("unknown-entity", "label=\"Save\"", "label=\"&secret;\"", "unknown XML entity");
    mutation("null-reference", "label=\"Save\"", "label=\"&#0;\"", "invalid XML character reference");
    mutation("surrogate-reference", "label=\"Save\"", "label=\"&#xD800;\"", "invalid XML character reference");
    mutation("out-of-range-reference", "label=\"Save\"", "label=\"&#x110000;\"", "invalid XML character reference");
    refusal("dtd", "<!DOCTYPE View [<!ENTITY secret SYSTEM 'file:///tmp/private'>]>" + source, "DTDs");
    refusal("processing-instruction", "<?fetch url='https://example.invalid'?>" + source, "processing instructions");
    refusal("cdata", "<![CDATA[ignored]]>" + source, "CDATA");
    refusal("bad-comment", "<!-- invalid -- comment -->" + source, "malformed XML comment");
    refusal("trailing-content", source + "<Other/>", "after View");
    refusal("non-xml", declarativeSource);
    refusal("invalid-xml-character", std::string(1, '\0') + source, "invalid XML character");
    refusal("wrong-encoding", "<?xml version='1.0' encoding='UTF-16'?>" + source, "UTF-8 encoding");
    refusal("declaration-reference", "<?xml version='&#49;.0'?>" + source, "cannot contain character references");
    refusal("declaration-order", "<?xml version='1.0' standalone='yes' encoding='UTF-8'?>" + source,
        "encoding must precede standalone");
    refusal("source-size", std::string(65537, ' '), "invalid string member");

    const std::string prefix = "<View name=\"FixtureView\" model=\"FixtureModel\" profile=\"pipelang.native-model-view.v1\">";
    std::string wide = prefix + "<Column id=\"root\">";
    for (int index = 0; index < 128; ++index)
        wide += "<Label id=\"node" + std::to_string(index) + "\" text=\"value\"/>";
    refusal("node-capacity", wide + "</Column></View>", "node or depth capacity");
    auto maximumNodes = xml;
    maximumNodes["source"] = replace(wide, "<Label id=\"node127\" text=\"value\"/>", "") + "</Column></View>";
    oracle.admitted("XmlMaximumNodes", maximumNodes, true);
    std::string deep = prefix;
    for (int index = 0; index < 9; ++index)
        deep += "<Column id=\"depth" + std::to_string(index) + "\">";
    for (int index = 0; index < 9; ++index)
        deep += "</Column>";
    refusal("depth-capacity", deep + "</View>", "node or depth capacity");
    auto maximumDepth = xml;
    maximumDepth["source"] = replace(deep, "<Column id=\"depth8\"></Column>", "") + "</View>";
    oracle.admitted("XmlMaximumDepth", maximumDepth, true);
    std::string styles = prefix;
    for (int index = 0; index < 33; ++index)
        styles += "<Style name=\"Theme" + std::to_string(index) + "\"/>";
    refusal("style-capacity", styles + "<Column id=\"root\"/></View>", "style declaration limit");
    auto maximumStyles = xml;
    maximumStyles["source"] = replace(styles, "<Style name=\"Theme32\"/>", "") + "<Column id=\"root\"/></View>";
    oracle.admitted("XmlMaximumStyles", maximumStyles, true);
    std::string tokens = prefix + "<Style name=\"Theme\">";
    for (int index = 0; index < 33; ++index)
        tokens += "<Token name=\"value" + std::to_string(index) + "\" type=\"length\" value=\"1px\"/>";
    refusal("style-token-capacity", tokens + "</Style><Column id=\"root\"/></View>", "style token limit");
    auto maximumTokens = xml;
    maximumTokens["source"] = replace(tokens, "<Token name=\"value32\" type=\"length\" value=\"1px\"/>", "")
        + "</Style><Column id=\"root\"/></View>";
    oracle.admitted("XmlMaximumStyleTokens", maximumTokens, true);
    auto maximumBytes = xml;
    maximumBytes["source"] = "<!--" + std::string(65536 - source.size() - 7, ' ') + "-->" + source;
    require(oracle.admitted("XmlMaximumSourceBytes", maximumBytes, true) == plain,
        "XML maximum-sized source changed generated behavior");
    refusal("style-semantic-error", prefix
        + "<Style name=\"Theme\"><Property name=\"gap\" value=\"#FFFFFFFF\"/></Style>"
          "<Column id=\"root\"/></View>", "style value type");
    refusal("style-unknown-parent", prefix + "<Style name=\"Theme\" extends=\"Missing\"/>"
        "<Column id=\"root\"/></View>", "unknown inherited style");
    refusal("style-trailing-declaration", prefix + "<Column id=\"root\"/><Style name=\"Theme\"/></View>");

    const std::string locationSource = "<!-- café 🏍 -->\n" + replace(source, "id=\"root\"", "id=\"root\" mystery=\"1\"");
    auto locationRequest = xml;
    locationRequest["source"] = locationSource;
    const auto location = oracle.invoke(locationRequest, true);
    const auto start = locationSource.find("mystery=");
    require(!location.at("ok").template get<bool>() && location.at("diagnostics")[0].at("code") == "PMV3001"
        && location.at("diagnostics")[0].at("start") == start
        && location.at("diagnostics")[0].at("end") == start + std::string("mystery=\"1\"").size(),
        "XML diagnostics did not retain authored UTF-8 byte offsets");
    auto semanticRequest = xml;
    const auto semanticSource = replace(source, "model.Draft.Name", "model.Draft.Missing");
    semanticRequest["source"] = semanticSource;
    const auto semantic = oracle.invoke(semanticRequest, true);
    const auto attributeStart = semanticSource.find("value=\"{bind model.Draft.Missing}\"");
    require(!semantic.at("ok").template get<bool>() && semantic.at("diagnostics")[0].at("code") == "PMV1001"
        && semantic.at("diagnostics")[0].at("start") == attributeStart,
        "shared typed checker lost the XML attribute location");

    for (const auto &format : Json::array({"xml", "vpipe", "", false, nullptr, Json::object()})) {
        auto invalidFormat = xml;
        invalidFormat["source_format"] = format;
        oracle.refused("xml/format-" + format.dump(), invalidFormat, true);
    }
    auto wrongFormat = xml;
    wrongFormat["source_format"] = "pview";
    oracle.refused("xml/no-content-sniffing", wrongFormat, true);

    const auto settingsModel = oracle.admitted("XmlSettingsModel", {
        {"protocol", "pipelang.native-model.v1"}, {"namespace", "pipelang_models"},
        {"source", oracle.source("examples/settings/Settings.pipe")}});
    auto settings = xml;
    settings["model_manifest"] = settingsModel.at("manifest");
    settings["source"] = oracle.source("examples/settings/SettingsView.pxml");
    settings["style_inspection"] = "pipelang.native-style-descriptor.v1";
    const auto settingsXml = oracle.admitted("XmlSettingsView", settings, true);
    settings["source_format"] = "pview";
    settings["source"] = oracle.source("examples/settings/SettingsView.pview");
    require(settingsXml == oracle.admitted("XmlEquivalentSettingsView", settings, true),
        "active Settings XML and declarative examples differ");
}

}
