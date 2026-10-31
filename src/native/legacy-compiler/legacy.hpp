// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../support/support.hpp"
#include "../lexer/lexer.hpp"
#include <memory>

namespace pipelang::legacy {
using namespace pipelang::tools;
struct SourceSpan {
    std::string file;
    std::size_t start = 0;
    std::size_t end = 0;
};
struct RelatedSpan {
    SourceSpan span;
    std::string message;
};
struct Value {
    std::string type;
    Json data;
    std::string string() const;
};
struct Expression {
    std::string kind;
    std::string text;
    Value literal;
    std::vector<std::shared_ptr<Expression>> children;
    std::string file;
    std::size_t start = 0;
    std::size_t end = 0;
};
using Expr = std::shared_ptr<Expression>;
struct Annotation {
    std::string name;
    Value value;
};
struct Field {
    std::string name;
    std::string type;
    bool visible = true;
    Expr initial;
    std::vector<Annotation> annotations;
    SourceSpan span{};
    SourceSpan typeSpan{};
};
struct Method {
    std::string name;
    std::string result;
    bool visible = true;
    std::vector<Field> parameters;
    Expr body;
    std::vector<Annotation> annotations;
    SourceSpan span{};
    SourceSpan resultSpan{};
};
struct Declaration {
    std::string name;
    bool contract = false;
    bool visible = true;
    std::string implements;
    std::vector<Field> fields;
    std::vector<Method> methods;
    std::vector<Annotation> annotations;
    SourceSpan span{};
    SourceSpan nameSpan{};
    SourceSpan implementsSpan{};
};
struct Program {
    std::vector<Declaration> declarations;
    SourceSpan span{};
};
struct Diagnostic : std::runtime_error {
    Diagnostic(std::string code, std::string message, std::string file, std::size_t start, std::size_t end, std::vector<RelatedSpan> related = {})
        : std::runtime_error(std::move(message)), code(std::move(code)), file(std::move(file)), start(start), end(end), related(std::move(related)) {}
    Diagnostic(std::string code, std::string message, const SourceSpan& span, std::vector<RelatedSpan> related = {})
        : Diagnostic(std::move(code), std::move(message), span.file, span.start, span.end, std::move(related)) {}
    std::string code;
    std::string file;
    std::size_t start;
    std::size_t end;
    std::vector<RelatedSpan> related;
};
struct Artifacts {
    std::string entry;
    std::string workflow;
    std::string bindings;
    std::string environment;
};
std::vector<Diagnostic> analyze(const std::map<std::string, std::string>& sources);
std::string diagnosticsJson(const std::map<std::string, std::string>& sources, const std::vector<Diagnostic>& diagnostics);
std::string diagnosticText(const std::map<std::string, std::string>& sources, const Diagnostic& diagnostic);
Program parse(const std::string& source, const std::string& file);
Program merge(const std::map<std::string, std::string>& sources);
void check(const Program& program);
Value evaluate(const Expr& expression, const std::map<std::string, Value>& values);
std::string infer(const Expr& expression, const std::map<std::string, std::string>& types);
std::string expressionText(const Expr& expression);
std::string projection(const Program& program, const std::string& path);
Artifacts compile(const Program& program, const std::string& entry);
std::string resultJson(const Json& value);
Value invoke(const Program& program, const std::string& owner, const std::string& method, const std::vector<std::string>& arguments);
Value argument(const std::string& type, const std::string& value);
fs::path moduleRoot(const fs::path& start);
std::map<std::string, std::string> readSources(const fs::path& root);
void publishArtifacts(const fs::path& directory, const std::string& base, const Artifacts& artifacts);
std::vector<fs::path> workflowRoots(const fs::path& project);
std::vector<fs::path> existingRoots(const std::vector<fs::path>& requested);
std::size_t materialize(const std::vector<fs::path>& roots, bool force, const fs::path& output);
void operation(const std::string& unit, std::map<std::string, std::string>& ids, const std::function<void()>& action);
int command(const std::vector<std::string>& arguments, std::istream& input, std::ostream& output);
}
