// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"
#include <charconv>
#include <cmath>

namespace pipelang::legacy {
namespace {
void admitUtf8(const std::string& source, const std::string& file) {
    require(source.size() <= 65536, "legacy source exceeds its byte limit");
    for (std::size_t offset = 0; offset < source.size();) {
        const auto first = static_cast<unsigned char>(source[offset]);
        if (first < 0x80) {
            ++offset;
            continue;
        }
        std::size_t width = 0;
        if (first >= 0xc2 && first <= 0xdf)
            width = 2;
        else if (first >= 0xe0 && first <= 0xef)
            width = 3;
        else if (first >= 0xf0 && first <= 0xf4)
            width = 4;
        bool valid = width != 0 && width <= source.size() - offset;
        for (std::size_t index = 1; valid && index < width; ++index) {
            const auto continuation = static_cast<unsigned char>(source[offset + index]);
            valid = continuation >= 0x80 && continuation <= 0xbf;
            if (index == 1) {
                valid = valid && !(first == 0xe0 && continuation < 0xa0) && !(first == 0xed && continuation > 0x9f) &&
                    !(first == 0xf0 && continuation < 0x90) && !(first == 0xf4 && continuation > 0x8f);
            }
        }
        if (!valid)
            throw Diagnostic("PL0001", "source is not valid UTF-8", file, offset, offset + 1);
        offset += width;
    }
}
class Parser {
public:
    Parser(const std::string& source, std::string file) : file_(std::move(file)), sourceBytes_(source.size()) {
        require(source.size() <= 65536, "legacy source exceeds its byte limit");
        auto result = native::lex(source, native::NumericSyntax::Legacy);
        if (result.failure)
            throw Diagnostic(result.failure->code, result.failure->message, file_, result.failure->start, result.failure->end);
        require(result.tokens.size() <= 32768, "legacy token bound exceeded");
        tokens_ = std::move(result.tokens);
    }
    Program program() {
        Program result;
        result.span = {file_, 0, sourceBytes_};
        while (peek().kind != "eof") {
            const auto start = peek().start;
            Declaration declaration;
            declaration.annotations = annotations();
            declaration.visible = visibility();
            declaration.contract = take("Interface");
            if (!declaration.contract && !take("Class") && !take("Struct"))
                fail("expected Interface, Class, or Struct");
            declaration.nameSpan = {file_, peek().start, peek().end};
            declaration.name = identifier();
            if (!declaration.contract && take(":")) {
                declaration.implementsSpan = {file_, peek().start, peek().end};
                declaration.implements = identifier();
            }
            expect("{");
            while (!take("}"))
                member(declaration);
            declaration.span = {file_, start, tokens_.at(position_ - 1).end};
            result.declarations.push_back(std::move(declaration));
            require(result.declarations.size() <= 128, "legacy declaration bound exceeded");
        }
        return result;
    }
private:
    const native::Token& peek() const { return tokens_.at(position_); }
    [[noreturn]] void fail(const std::string& message) const {
        throw Diagnostic("PL2001", message, file_, peek().start, peek().end);
    }
    bool take(const std::string& literal) {
        if (peek().kind == "eof" || peek().kind == "text" || peek().literal != literal)
            return false;
        ++position_;
        return true;
    }
    void expect(const std::string& literal) {
        if (!take(literal))
            throw Diagnostic("PL2002", "expected " + literal, file_, peek().start, peek().end);
    }
    std::string identifier() {
        if (peek().kind != "identifier")
            fail("expected identifier");
        return tokens_.at(position_++).literal;
    }
    bool visibility() {
        if (take("private"))
            return false;
        take("public");
        return true;
    }
    Value literal(bool annotation = false) {
        const auto token = peek();
        ++position_;
        if (token.kind == "text")
            return {"string", token.literal};
        if (token.kind == "boolean")
            return {"bool", token.literal == "true"};
        if (token.kind == "integer") {
            std::int64_t value = 0;
            const auto result = std::from_chars(token.literal.data(), token.literal.data() + token.literal.size(), value);
            if (result.ec != std::errc{} || result.ptr != token.literal.data() + token.literal.size())
                throw Diagnostic("PL1002", "invalid integer literal", file_, token.start, token.end);
            return {"int", value};
        }
        if (token.kind == "float") {
            double value = 0;
            const auto result = std::from_chars(token.literal.data(), token.literal.data() + token.literal.size(), value);
            if (result.ec != std::errc{} || result.ptr != token.literal.data() + token.literal.size() || !std::isfinite(value))
                throw Diagnostic("PL1002", "invalid float literal", file_, token.start, token.end);
            return {"float", value};
        }
        --position_;
        fail(annotation ? "expected literal annotation value" : "unexpected token " + Json(token.literal).dump() + " in expression");
    }
    std::vector<Annotation> annotations() {
        std::vector<Annotation> result;
        while (take("[")) {
            const auto name = identifier();
            expect("=");
            result.push_back({name, literal(true)});
            expect("]");
            require(result.size() <= 128, "legacy annotation bound exceeded");
        }
        return result;
    }
    void member(Declaration& owner) {
        const auto start = peek().start;
        auto attributes = annotations();
        const bool visible = visibility();
        const SourceSpan typeSpan{file_, peek().start, peek().end};
        const auto type = identifier();
        const auto name = identifier();
        if (take("(")) {
            Method method{name, type, visible, {}, {}, std::move(attributes)};
            method.resultSpan = typeSpan;
            if (!take(")")) {
                do {
                    const SourceSpan parameterTypeSpan{file_, peek().start, peek().end};
                    const auto parameterType = identifier();
                    const auto parameterName = identifier();
                    method.parameters.push_back({parameterName, parameterType, true, {}, {}});
                    method.parameters.back().typeSpan = parameterTypeSpan;
                    method.parameters.back().span = {file_, parameterTypeSpan.start, tokens_.at(position_ - 1).end};
                    require(method.parameters.size() <= 32, "legacy parameter bound exceeded");
                } while (take(","));
                expect(")");
            }
            if (!owner.contract) {
                expect("=>");
                method.body = expression();
            }
            expect(";");
            method.span = {file_, start, tokens_.at(position_ - 1).end};
            owner.methods.push_back(std::move(method));
        } else {
            Field field{name, type, visible, {}, std::move(attributes)};
            field.typeSpan = typeSpan;
            if (!owner.contract && take("="))
                field.initial = expression();
            expect(";");
            field.span = {file_, start, tokens_.at(position_ - 1).end};
            owner.fields.push_back(std::move(field));
        }
        require(owner.fields.size() + owner.methods.size() <= 256, "legacy member bound exceeded");
    }
    static int precedence(const std::string& op) {
        if (op == "||")
            return 1;
        if (op == "&&")
            return 2;
        if (op == "==" || op == "!=")
            return 3;
        if (op == "<" || op == "<=" || op == ">" || op == ">=")
            return 4;
        if (op == "+" || op == "-")
            return 5;
        if (op == "*" || op == "/")
            return 6;
        return 0;
    }
    Expr expression(int minimum = 1, unsigned depth = 0) {
        require(depth <= 48 && ++nodes_ <= 16384, "legacy expression bound exceeded");
        const auto start = peek();
        auto result = std::make_shared<Expression>();
        result->file = file_;
        result->start = start.start;
        if (take("!") || take("-")) {
            result->kind = "unary";
            result->text = start.literal;
            result->children = {expression(7, depth + 1)};
        } else if (take("(")) {
            result = expression(1, depth + 1);
            expect(")");
        } else if (start.kind == "identifier") {
            result->kind = "identifier";
            result->text = identifier();
        } else {
            result->kind = "literal";
            result->literal = literal();
        }
        result->start = start.start;
        result->end = tokens_.at(position_ - 1).end;
        while (peek().kind != "text" && precedence(peek().literal) >= minimum && precedence(peek().literal) != 0) {
            const auto operation = tokens_.at(position_++);
            auto binary = std::make_shared<Expression>();
            require(++nodes_ <= 16384, "legacy expression node limit exceeded");
            binary->kind = "binary";
            binary->text = operation.literal;
            binary->file = file_;
            binary->start = result->start;
            binary->children = {result, expression(precedence(operation.literal) + 1, depth + 1)};
            binary->end = binary->children.back()->end;
            result = std::move(binary);
        }
        return result;
    }
    std::string file_;
    std::size_t sourceBytes_;
    std::vector<native::Token> tokens_;
    std::size_t position_ = 0;
    std::size_t nodes_ = 0;
};
}
Program parse(const std::string& source, const std::string& file) {
    admitUtf8(source, file);
    return Parser(source, file).program();
}
Program merge(const std::map<std::string, std::string>& sources) {
    require(sources.size() <= 128, "legacy source-set bound exceeded");
    Program program;
    std::size_t bytes = 0;
    for (const auto& [file, source] : sources) {
        require(source.size() <= 65536 - bytes, "legacy aggregate source bound exceeded");
        bytes += source.size();
        auto parsed = parse(source, file);
        if (program.span.file.empty())
            program.span = parsed.span;
        program.declarations.insert(program.declarations.end(), std::make_move_iterator(parsed.declarations.begin()), std::make_move_iterator(parsed.declarations.end()));
        require(program.declarations.size() <= 128, "legacy aggregate declaration bound exceeded");
    }
    return program;
}
}
