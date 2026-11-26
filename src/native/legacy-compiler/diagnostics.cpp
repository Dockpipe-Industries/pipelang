// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"
#include <algorithm>

namespace pipelang::legacy {
namespace {
using Ordered = nlohmann::ordered_json;
Ordered position(const std::string* source, std::size_t offset) {
    if (!source)
        return {{"offset", offset}, {"line", 1}, {"column", offset + 1}, {"utf16_column", offset + 1}};
    offset = std::min(offset, source->size());
    std::size_t line = 1;
    std::size_t start = 0;
    for (std::size_t index = 0; index < offset; ++index) {
        if ((*source)[index] == '\n') {
            ++line;
            start = index + 1;
        }
    }
    std::size_t column = 1;
    std::size_t utf16 = 1;
    while (start < offset) {
        const auto first = static_cast<unsigned char>((*source)[start]);
        std::size_t width = 1;
        if (first >= 0xc2 && first <= 0xdf)
            width = 2;
        else if (first >= 0xe0 && first <= 0xef)
            width = 3;
        else if (first >= 0xf0 && first <= 0xf4)
            width = 4;
        if (width > offset - start)
            width = 1;
        ++column;
        utf16 += width == 4 ? 2 : 1;
        start += width;
    }
    return {{"offset", offset}, {"line", line}, {"column", column}, {"utf16_column", utf16}};
}
Ordered range(const std::map<std::string, std::string>& sources, const Diagnostic& diagnostic) {
    const auto found = sources.find(diagnostic.file);
    // The reference source set excludes a file whose UTF-8 admission failed.
    const auto* source = found == sources.end() || diagnostic.code == "PL0001" ? nullptr : &found->second;
    return {{"file", diagnostic.file}, {"start", position(source, diagnostic.start)}, {"end", position(source, diagnostic.end)}};
}
std::string category(const std::string& code) {
    if (code.starts_with("PL0"))
        return "source";
    if (code.starts_with("PL1"))
        return "lexical";
    if (code.starts_with("PL2"))
        return "syntax";
    if (code.starts_with("PL4"))
        return "evaluation";
    return "semantic";
}
std::string escapedJson(const Ordered& value) {
    const auto bytes = value.dump(2);
    std::string result;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] == '<')
            result += "\\u003c";
        else if (bytes[index] == '>')
            result += "\\u003e";
        else if (bytes[index] == '&')
            result += "\\u0026";
        else if (bytes.substr(index, 3) == "\xe2\x80\xa8" || bytes.substr(index, 3) == "\xe2\x80\xa9") {
            result += bytes[index + 2] == '\xa8' ? "\\u2028" : "\\u2029";
            index += 2;
        } else
            result += bytes[index];
    }
    return result;
}
}
std::vector<Diagnostic> analyze(const std::map<std::string, std::string>& sources) {
    require(!sources.empty() && sources.size() <= 128, "legacy source-set bound exceeded");
    std::vector<Diagnostic> diagnostics;
    Program program;
    std::size_t bytes = 0;
    for (const auto& [file, source] : sources) {
        require(source.size() <= 65536 - bytes, "legacy aggregate source bound exceeded");
        bytes += source.size();
        try {
            auto local = parse(source, file);
            if (program.span.file.empty())
                program.span = local.span;
            program.declarations.insert(program.declarations.end(), std::make_move_iterator(local.declarations.begin()), std::make_move_iterator(local.declarations.end()));
            require(program.declarations.size() <= 128, "legacy aggregate declaration bound exceeded");
        } catch (const Diagnostic& diagnostic) {
            diagnostics.push_back(diagnostic);
        }
    }
    if (!diagnostics.empty())
        return diagnostics;
    try {
        check(program);
    } catch (const Diagnostic& diagnostic) {
        diagnostics.push_back(diagnostic);
    } catch (const std::exception& error) {
        diagnostics.emplace_back("PL3001", error.what(), sources.begin()->first, 0, sources.begin()->second.size());
    }
    return diagnostics;
}
std::string diagnosticsJson(const std::map<std::string, std::string>& sources, const std::vector<Diagnostic>& diagnostics) {
    Ordered values = Ordered::array();
    for (const auto& diagnostic : diagnostics) {
        Ordered value{{"code", diagnostic.code}, {"category", category(diagnostic.code)}, {"severity", "error"},
            {"message", diagnostic.what()}, {"primary", range(sources, diagnostic)}};
        if (!diagnostic.related.empty()) {
            value["related"] = Ordered::array();
            for (const auto& related : diagnostic.related) {
                const auto found = sources.find(related.span.file);
                const auto* source = found == sources.end() ? nullptr : &found->second;
                const Ordered resolved{{"file", related.span.file}, {"start", position(source, related.span.start)}, {"end", position(source, related.span.end)}};
                value["related"].push_back(Ordered{{"range", resolved}, {"message", related.message}});
            }
        }
        values.push_back(std::move(value));
    }
    return escapedJson(Ordered{{"schema", 1}, {"diagnostics", values}}) + '\n';
}
std::string diagnosticText(const std::map<std::string, std::string>& sources, const Diagnostic& diagnostic) {
    const auto primary = range(sources, diagnostic);
    const auto& start = primary.at("start");
    return (diagnostic.file.empty() ? "<input>" : diagnostic.file) + ':' + std::to_string(start.at("line").get<std::size_t>()) + ':' +
        std::to_string(start.at("column").get<std::size_t>()) + ": error[" + diagnostic.code + "]: " + diagnostic.what();
}
}
