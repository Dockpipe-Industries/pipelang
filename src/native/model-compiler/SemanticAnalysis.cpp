// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CheckedProgram.hpp"
#include "OutputBudgetStream.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <ostream>
#include <set>
#include <tuple>

namespace pipelang::models {
namespace {

struct Binding {
    std::size_t offset;
    std::size_t scopeStart;
    std::size_t scopeEnd;
    std::string path;
};

using Bindings = std::map<std::string, Binding>;

struct SourceTokens {
    const ModuleSource* source;
    std::vector<native::Token> tokens;
};

struct MethodInfo {
    const Declaration* owner;
    const Method* method;
    const SourceTokens* source;
    nlohmann::json location;
    nlohmann::json signature;
    std::string name;
    bool renameSafe;
};

class SemanticSymbols final {
public:
    explicit SemanticSymbols(const std::vector<ModuleSource>& sources) : sources_(sources) {}

    nlohmann::json collect(const Program& program) {
        indexSources();
        indexDeclarations(program);
        emitMethods();

        for (const auto& declaration : program.declarations) {
            if (!selectSource(declaration))
                continue;

            for (const auto& field : declaration.fields) {
                const auto offset = fieldNameOffset(field);
                add(field.name, field.type, "field", offset, true,
                    Binding{offset, 0, source_->source.size(), source_->path});
            }

            for (const auto& method : declaration.methods) {
                const auto info = methods_.find(method.identity);
                if (info == methods_.end())
                    continue;

                caller_ = method.identity;
                const Binding methodScope{method.nameOffset, method.offset,
                                          method.endOffset, source_->path};
                add(info->second.name, method.result, "method", method.nameOffset, true, methodScope,
                    {{"callable", method.identity}, {"signature", info->second.signature},
                     {"rename_safe", info->second.renameSafe}});

                Bindings bindings;
                for (const auto& parameter : method.parameters) {
                    const Binding binding{parameter.offset, method.offset,
                                          method.endOffset, source_->path};
                    bindings.emplace(parameter.name, binding);
                    add(parameter.name, parameter.type, "parameter", parameter.offset, true, binding);
                }
                expression(method.baseConstructor, bindings);
                for (const auto& initializer : method.fieldInitializers)
                    expression(initializer, bindings);
                statements(method.body, std::move(bindings), method.offset, method.endOffset);
            }
        }
        return {{"symbols", std::move(symbols_)}, {"methods", std::move(methodRows_)},
                {"types", std::move(typeRows_)}, {"calls", std::move(calls_)}};
    }

private:
    using Origin = std::pair<std::string, std::string>;
    using TypeAnchor = std::tuple<std::string, std::string, std::size_t>;

    void append(nlohmann::json& rows, nlohmann::json row) {
        const auto bytes = row.dump().size() + 1;
        if (budgetExceeded(symbolBytes_, bytes, resourceBudget().outputBytes))
            throw Diagnostic("semantic output_bytes resource budget exceeded; configure output_bytes",
                row.value("offset", std::size_t{0}));

        symbolBytes_ += bytes;
        rows.push_back(std::move(row));
    }

    static std::size_t tokenIndex(const std::vector<native::Token>& tokens, std::size_t offset) {
        const auto found = std::lower_bound(tokens.begin(), tokens.end(), offset,
            [](const native::Token& token, std::size_t position) { return token.start < position; });
        return found != tokens.end() && found->start == offset
            ? static_cast<std::size_t>(found - tokens.begin()) : tokens.size();
    }

    static nlohmann::json location(const ModuleSource& source, const native::Token& token,
                                   const std::string& identity) {
        return {{"id", identity}, {"path", source.path}, {"module", source.name},
                {"offset", token.start}, {"length", token.end - token.start}};
    }

    void indexSources() {
        for (const auto& source : sources_) {
            const Origin origin{source.name, source.path};
            if (sourceTokens_.contains(origin))
                throw Diagnostic("semantic source origin is ambiguous", 0);

            auto lexical = native::lex(source.source, native::NumericSyntax::Binary64,
                                      resourceBudget().tokens);
            if (lexical.failure)
                throw Diagnostic(lexical.failure->message, lexical.failure->start);

            auto [entry, inserted] = sourceTokens_.emplace(origin,
                SourceTokens{&source, std::move(lexical.tokens)});
            (void)inserted;
            source_ = &source;
            tokens_ = &entry->second.tokens;

            // The owning parser retains template declarations that closed
            // normalization may erase. Their definition range is syntactic;
            // method binding below still comes only from checked identities.
            const auto authored = parse(source.source, true, 32, true);
            for (const auto& declaration : authored.declarations) {
                auto index = tokenIndex(*tokens_, declaration.offset);
                while (index < tokens_->size() &&
                    (tokens_->at(index).literal == "model" || tokens_->at(index).literal == "source"))
                    ++index;

                if (index + 1 >= tokens_->size() || tokens_->at(index).literal != declaration.kind)
                    continue;

                const auto& token = tokens_->at(index + 1);
                if (token.kind != "identifier" || token.literal != declaration.name)
                    continue;

                const auto identity = source.name + '.' + declaration.name;
                auto row = location(source, token, identity);
                row["name"] = declaration.name;
                row["type"] = identity;
                row["kind"] = declaration.kind;
                row["type_parameters"] = declaration.typeParameters;
                typeAnchors_.emplace(TypeAnchor{source.name, source.path, declaration.offset}, row);
                append(typeRows_, row);
            }
        }
    }

    bool selectSource(const Declaration& declaration) {
        const auto found = sourceTokens_.find({declaration.module, declaration.originPath});
        if (found == sourceTokens_.end())
            return false;

        source_ = found->second.source;
        tokens_ = &found->second.tokens;
        return true;
    }

    nlohmann::json typeDefinition(const Type& type) const {
        if (const auto found = nominalTypes_.find(type.name); found != nominalTypes_.end())
            return found->second;

        if (type.name == "Optional" && type.arguments.size() == 1)
            return typeDefinition(type.arguments.front());

        return nullptr;
    }

    nlohmann::json signature(const Method& method, const std::string& name) const {
        auto parameters = nlohmann::json::array();
        for (const auto& parameter : method.parameters) {
            nlohmann::json item{{"name", parameter.name}, {"type", parameter.type.displayText()}};
            if (auto definition = typeDefinition(parameter.type); !definition.is_null())
                item["type_definition"] = std::move(definition);

            parameters.push_back(std::move(item));
        }
        return {{"name", name}, {"parameters", std::move(parameters)},
                {"return_type", method.result.displayText()}};
    }

    void indexDeclarations(const Program& program) {
        for (const auto& declaration : program.declarations) {
            const auto anchor = typeAnchors_.find(
                {declaration.module, declaration.originPath, declaration.offset});
            if (anchor != typeAnchors_.end())
                nominalTypes_.emplace(declaration.name, anchor->second);
        }

        std::map<std::pair<std::string, std::size_t>, bool> safeSourceNames;
        for (const auto& declaration : program.declarations) {
            if (!selectSource(declaration))
                continue;

            const auto& frame = sourceTokens_.at({declaration.module, declaration.originPath});
            for (const auto& method : declaration.methods) {
                if (method.endOffset == 0)
                    continue;

                const auto* token = tokenAt(method.nameOffset);
                if (!token || token->kind != "identifier")
                    continue;

                const bool overloaded = std::any_of(declaration.methods.begin(), declaration.methods.end(),
                    [&](const Method& candidate) {
                        if (candidate.nameOffset == method.nameOffset || candidate.endOffset == 0)
                            return false;

                        const auto* candidateToken = tokenAt(candidate.nameOffset);
                        return candidateToken && candidateToken->literal == token->literal;
                    });
                const bool safe = !overloaded && !method.constructor && !method.virtualMethod &&
                    !method.overrideMethod && method.dispatchTargets.empty() &&
                    method.typeParameters.empty() && method.sourceName.find('<') == std::string::npos &&
                    declaration.kind == "Class" && declaration.interfaces.empty() &&
                    declaration.baseClass.empty();
                const auto key = std::make_pair(source_->path, method.nameOffset);
                const auto [known, inserted] = safeSourceNames.emplace(key, safe);
                if (!inserted)
                    known->second = known->second && safe;

                methods_.emplace(method.identity, MethodInfo{&declaration, &method, &frame,
                    location(*source_, *token, method.identity), signature(method, token->literal),
                    token->literal, safe});
            }
        }
        for (auto& [identity, info] : methods_) {
            (void)identity;
            info.renameSafe = safeSourceNames.at({info.source->source->path, info.method->nameOffset});
        }
    }

    nlohmann::json implementationLocations(const std::vector<std::string>& identities) const {
        auto result = nlohmann::json::array();
        std::set<std::pair<std::string, std::size_t>> seen;
        for (const auto& identity : identities) {
            const auto found = methods_.find(identity);
            if (found == methods_.end())
                continue;

            const auto& info = found->second;
            if (seen.insert({info.source->source->path, info.method->nameOffset}).second)
                result.push_back(info.location);
        }
        return result;
    }

    void emitMethods() {
        for (const auto& [identity, info] : methods_) {
            (void)identity;
            const auto& method = *info.method;
            auto row = info.location;
            row.update(info.signature);
            row["owner"] = info.owner->sourceName.empty() ? info.owner->name : info.owner->sourceName;
            row["end_offset"] = method.endOffset;
            row["source_id"] = info.source->source->path + ':' + std::to_string(method.nameOffset);
            row["pure"] = method.pure;
            row["async"] = method.asynchronous;
            row["static"] = method.staticMethod;
            row["constructor"] = method.constructor;
            row["rename_safe"] = info.renameSafe;
            row["implementations"] = implementationLocations(method.dispatchTargets);
            append(methodRows_, std::move(row));
        }
    }

    const native::Token* tokenAt(std::size_t offset) const {
        const auto index = tokenIndex(*tokens_, offset);
        return index == tokens_->size() ? nullptr : &tokens_->at(index);
    }

    std::size_t fieldNameOffset(const Field& field) const {
        std::size_t result = field.offset;
        for (const auto& token : *tokens_) {
            if (token.start < field.offset)
                continue;
            if (token.literal == "=" || token.literal == ";")
                break;
            if (token.kind == "identifier" && token.literal == field.name)
                result = token.start;
        }
        return result;
    }

    void add(const std::string& name, const Type& type, const std::string& kind,
             std::size_t offset, bool declaration, const Binding& binding,
             const nlohmann::json& extra = nlohmann::json::object()) {
        const auto* token = tokenAt(offset);
        if (!token || token->kind != "identifier" || token->literal != name || type.name.empty())
            return;

        const auto typeName = type.displayText();
        const auto identity = std::make_tuple(source_->path, source_->name, offset, kind,
            typeName, declaration, extra.value("callable", std::string{}));
        if (!seen_.insert(identity).second)
            return;

        nlohmann::json symbol{{"name", name}, {"type", typeName}, {"kind", kind},
            {"offset", offset}, {"length", token->end - token->start},
            {"path", source_->path}, {"module", source_->name},
            {"declaration", declaration}, {"scope_start", binding.scopeStart},
            {"scope_end", binding.scopeEnd}, {"declaration_offset", binding.offset},
            {"declaration_path", binding.path}};
        if (auto definition = typeDefinition(type); !definition.is_null())
            symbol["type_definition"] = std::move(definition);

        symbol.update(extra);
        append(symbols_, std::move(symbol));
    }

    std::size_t callToken(const Expr& call, const MethodInfo& info) const {
        if (call->children.empty())
            return tokens_->size();

        auto index = tokenIndex(*tokens_, call->children.front()->offset);
        if (index == tokens_->size())
            return index;

        if (tokens_->at(index).literal == "base" && index + 2 < tokens_->size() &&
            tokens_->at(index + 1).literal == ".")
            index += 2;

        if (info.method->constructor && tokens_->at(index).literal == "new") {
            ++index;
            while (index + 2 < tokens_->size() && tokens_->at(index + 1).literal == ".")
                index += 2;
        }
        return tokens_->at(index).kind == "identifier" && tokens_->at(index).literal == info.name
            ? index : tokens_->size();
    }

    static std::size_t expressionStart(const Expr& value) {
        auto result = value->offset;
        for (const auto& child : value->children)
            result = std::min(result, expressionStart(child));

        return result;
    }

    nlohmann::json callRanges(const Expr& call, std::size_t name) const {
        auto open = name + 1;
        if (open < tokens_->size() && tokens_->at(open).literal == "<") {
            std::size_t depth = 0;
            do {
                const auto& text = tokens_->at(open).literal;
                if (text == "<")
                    ++depth;
                else if (text == ">")
                    --depth;
                ++open;
            } while (open < tokens_->size() && depth != 0);
        }
        if (open >= tokens_->size() || tokens_->at(open).literal != "(")
            return nullptr;

        std::size_t depth = 1;
        auto close = open + 1;
        for (; close < tokens_->size(); ++close) {
            const auto& token = tokens_->at(close);
            if (token.kind == "text")
                continue;
            if (token.literal == "(")
                ++depth;
            else if (token.literal == ")" && --depth == 0)
                break;
        }
        if (close == tokens_->size())
            return nullptr;

        std::vector<std::size_t> separators{open};
        for (std::size_t argument = 2; argument < call->children.size(); ++argument) {
            const auto start = expressionStart(call->children[argument]);
            auto next = std::lower_bound(tokens_->begin(), tokens_->end(), start,
                [](const native::Token& token, std::size_t offset) { return token.start < offset; });
            auto separator = static_cast<std::size_t>(next - tokens_->begin());
            while (separator > separators.back() + 1) {
                --separator;
                if (tokens_->at(separator).literal == "," && tokens_->at(separator).kind != "text")
                    break;
            }
            if (separator <= separators.back() || tokens_->at(separator).literal != ",")
                return nullptr;

            separators.push_back(separator);
        }
        separators.push_back(close);

        auto arguments = nlohmann::json::array();
        if (call->children.size() > 1) {
            for (std::size_t argument = 1; argument < separators.size(); ++argument) {
                const auto begin = separators[argument - 1] + 1;
                const auto end = separators[argument];
                if (begin >= end)
                    return nullptr;

                arguments.push_back({{"offset", tokens_->at(begin).start},
                    {"length", tokens_->at(end - 1).end - tokens_->at(begin).start}});
            }
        }
        return {{"open_paren", tokens_->at(open).start}, {"close_paren", tokens_->at(close).start},
                {"end_offset", tokens_->at(close).end}, {"arguments", std::move(arguments)}};
    }

    void call(const Expr& value) {
        const auto selected = methods_.find(value->callable);
        if (selected == methods_.end())
            return;

        const auto& info = selected->second;
        const auto name = callToken(value, info);
        if (name == tokens_->size())
            return;

        const auto& token = tokens_->at(name);
        auto row = location(*source_, token, value->callable);
        row.erase("id");
        row["name"] = token.literal;
        row["caller"] = caller_;
        row["callable"] = value->callable;
        row["targets"] = value->targets;
        row["definition"] = info.location;
        row["signature"] = info.signature;
        row["implementations"] = implementationLocations(value->targets);
        if (auto ranges = callRanges(value, name); !ranges.is_null())
            row.update(ranges);

        const auto identity = std::make_tuple(source_->path, token.start, caller_, value->callable);
        if (!seenCalls_.insert(identity).second)
            return;

        const Binding binding{info.method->nameOffset, info.method->offset,
            info.method->endOffset, info.source->source->path};
        add(token.literal, info.method->result, "method", token.start, false, binding,
            {{"callable", value->callable}, {"targets", value->targets}, {"definition", info.location},
             {"signature", info.signature}, {"rename_safe", info.renameSafe}});
        append(calls_, std::move(row));
    }

    void expression(const Expr& value, const Bindings& bindings) {
        if (!value)
            return;

        if (!value->callable.empty() && (value->kind == "call" || value->kind == "new_construct"))
            call(value);

        if ((value->kind == "name" || value->kind == "model_member") && value->resolution == "local") {
            if (const auto found = bindings.find(value->text); found != bindings.end())
                add(value->text, value->type, "local", value->offset, false, found->second);
        }
        for (const auto& child : value->children)
            expression(child, bindings);
    }

    void local(const Statement& statement, Bindings& bindings,
               std::size_t scopeStart, std::size_t scopeEnd) {
        const Binding binding{statement.nameOffset, scopeStart, scopeEnd, source_->path};
        add(statement.name, statement.type, "local", statement.nameOffset, true, binding);
        const auto nameToken = std::lower_bound(tokens_->begin(), tokens_->end(), statement.nameOffset,
            [](const native::Token& token, std::size_t position) { return token.start < position; });
        if (nameToken != tokens_->begin() && nameToken != tokens_->end()) {
            const auto& keyword = *std::prev(nameToken);
            if (statement.inferredType && keyword.literal == "var")
                add("var", statement.type, "inferred-type", keyword.start, false, binding);
        }
        bindings[statement.name] = binding;
    }

    Bindings statements(const std::vector<Statement>& body, Bindings bindings,
                        std::size_t scopeStart, std::size_t scopeEnd) {
        for (const auto& statement : body) {
            if (statement.kind == "for") {
                auto header = statements(statement.initialization, bindings,
                                         statement.offset, statement.endOffset);
                expression(statement.expression, header);
                statements(statement.body, header, statement.offset, statement.endOffset);
                statements(statement.iteration, std::move(header), statement.offset, statement.endOffset);
                continue;
            }

            expression(statement.expression, bindings);
            expression(statement.target, bindings);
            if (statement.kind == "local") {
                local(statement, bindings, statement.nameOffset, scopeEnd);
                continue;
            }

            auto nested = bindings;
            if (statement.kind == "foreach" || statement.kind == "parallel_foreach")
                local(statement, nested, statement.offset, statement.endOffset);
            statements(statement.body, std::move(nested), statement.offset, statement.endOffset);

            auto recovery = bindings;
            if (statement.kind == "try" && !statement.name.empty())
                local(statement, recovery, statement.offset, statement.endOffset);
            statements(statement.alternative, std::move(recovery), statement.offset, statement.endOffset);
            statements(statement.cleanup, bindings, statement.offset, statement.endOffset);
        }
        (void)scopeStart;
        return bindings;
    }

    const std::vector<ModuleSource>& sources_;
    std::map<Origin, SourceTokens> sourceTokens_;
    std::map<TypeAnchor, nlohmann::json> typeAnchors_;
    std::map<std::string, nlohmann::json> nominalTypes_;
    std::map<std::string, MethodInfo> methods_;
    const ModuleSource* source_ = nullptr;
    const std::vector<native::Token>* tokens_ = nullptr;
    std::string caller_;
    nlohmann::json symbols_ = nlohmann::json::array();
    nlohmann::json methodRows_ = nlohmann::json::array();
    nlohmann::json typeRows_ = nlohmann::json::array();
    nlohmann::json calls_ = nlohmann::json::array();
    std::set<std::tuple<std::string, std::string, std::size_t, std::string,
                        std::string, bool, std::string>> seen_;
    std::set<std::tuple<std::string, std::size_t, std::string, std::string>> seenCalls_;
    std::size_t symbolBytes_ = 0;
};

} // namespace

nlohmann::json indexProjectSources(const std::vector<ModuleSource>& sources) {
    if (sources.empty() || sources.size() > resourceBudget().sourceFragments)
        throw Diagnostic("index source fragment resource budget exceeded; configure source_fragments", 0);

    auto fragments = nlohmann::json::array();
    std::size_t bytes = 0;
    std::set<std::string> paths;
    for (const auto& source : sources) {
        if (source.path.empty() || source.path.size() > sourcePathLimit ||
            !paths.insert(source.path).second)
            throw Diagnostic("index source paths must be bounded, nonempty and unique", 0);
        if (budgetExceeded(bytes, source.source.size(), resourceBudget().inputBytes))
            throw Diagnostic("indexed input_bytes resource budget exceeded; configure input_bytes", 0);
        bytes += source.source.size();
        try {
            const auto header = parseModuleHeader(source.source);
            const auto name = header.module.empty()
                ? moduleIdentityFromStem(std::filesystem::path(source.path).stem().string())
                : header.module;
            fragments.push_back({{"path", source.path}, {"name", name}, {"imports", header.imports}});
        } catch (Diagnostic& diagnostic) {
            diagnostic.path = source.path;
            throw;
        }
    }
    nlohmann::json response{{"protocol", "pipelang.native-model-index.v1"}, {"ok", true},
        {"fragments", std::move(fragments)}, {"diagnostics", nlohmann::json::array()}};
    OutputBudgetStream counter(resourceBudget().outputBytes - 1);
    std::ostream output(&counter);
    output << response;
    if (!output || counter.exceeded())
        throw Diagnostic("index output_bytes resource budget exceeded; configure output_bytes", 0);
    return response;
}

nlohmann::json analyzeProjectModules(const std::vector<ModuleSource>& modules, const std::string& entry) {
    const auto checked = CheckedProgram::project(modules, entry);
    auto response = SemanticSymbols(modules).collect(checked.definition());
    response["protocol"] = "pipelang.native-model-analysis.v1";
    response["ok"] = true;
    response["diagnostics"] = nlohmann::json::array();
    OutputBudgetStream counter(resourceBudget().outputBytes - 1);
    std::ostream output(&counter);
    output << response;
    if (!output || counter.exceeded())
        throw Diagnostic("semantic output_bytes resource budget exceeded; configure output_bytes", 0);
    return response;
}

} // namespace pipelang::models
