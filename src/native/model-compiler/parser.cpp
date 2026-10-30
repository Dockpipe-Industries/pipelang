// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "syntax.hpp"

#include <cctype>
#include <utility>

namespace pipelang::models {

std::string Type::text() const {
    std::string result = name;
    if (!arguments.empty()) {
        result += '<';
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (index != 0)
                result += ',';
            result += arguments[index].text();
        }
        result += '>';
    }
    return result;
}

namespace {

class Parser {
public:
    explicit Parser(std::vector<native::Token> tokens) : tokens_(std::move(tokens)) {}
    Program program() {
        Program result;
        while (!atEnd()) {
            expect("public");
            Declaration declaration;
            declaration.offset = current().start;
            declaration.kind = current().literal;
            if (declaration.kind != "Class" && declaration.kind != "Record" &&
                declaration.kind != "Enum" && declaration.kind != "Interface")
                fail("expected Class, Record, Enum or Interface");
            advance();
            declaration.name = identifier();
            expect("{");
            if (declaration.kind == "Enum") {
                do {
                    declaration.values.push_back(identifier());
                } while (take(",") && !is("}"));
            } else {
                while (!is("}"))
                    member(declaration);
            }
            expect("}");
            result.declarations.push_back(std::move(declaration));
            if (result.declarations.size() > 128)
                fail("declaration limit exceeded");
        }
        return result;
    }
private:
    const native::Token& current() const { return tokens_.at(position_); }
    bool atEnd() const { return current().kind == "eof"; }
    bool is(const std::string& value) const { return current().literal == value; }
    void advance() {
        if (atEnd())
            fail("unexpected end of source");
        ++position_;
    }
    bool take(const std::string& value) {
        if (!is(value))
            return false;
        advance();
        return true;
    }
    void expect(const std::string& value) {
        if (!take(value))
            fail("expected '" + value + "'");
    }
    [[noreturn]] void fail(const std::string& message) const {
        throw Diagnostic(message, current().start);
    }
    std::string identifier() {
        if (current().kind != "identifier")
            fail("expected identifier");
        std::string value = current().literal;
        static const std::set<std::string> reserved = {
            "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool",
            "break", "case", "catch", "char", "class", "compl", "concept", "const",
            "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
            "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
            "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
            "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
            "namespace", "new", "noexcept", "not", "nullptr", "operator", "or", "private",
            "protected", "public", "register", "reinterpret_cast", "requires", "return",
            "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
            "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef",
            "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
            "wchar_t", "while", "xor", "not_eq", "and_eq", "or_eq", "xor_eq",
            "char8_t", "char16_t", "char32_t", "std", "pipelang",
            "State", "snapshot", "poll", "pending", "foreach", "in", "finally"
        };
        if (value.size() > 96 || (value.starts_with('_') || value.starts_with("pl_")) || value.find("__") != std::string::npos ||
            reserved.contains(value))
            fail("identifier is reserved or exceeds the identifier limit");
        advance();
        return value;
    }
    Type type(unsigned depth = 0) {
        if (depth > 12)
            fail("type nesting limit exceeded");
        if (current().kind != "identifier")
            fail("expected type");
        Type result{current().literal, {}};
        advance();
        if (take("<")) {
            do {
                result.arguments.push_back(type(depth + 1));
            } while (take(","));
            expect(">");
        }
        return result;
    }
    void member(Declaration& declaration) {
        const auto offset = current().start;
        bool visible = true;
        if (take("private"))
            visible = false;
        else if (declaration.kind != "Interface")
            expect("public");
        else
            take("public");
        const bool pure = take("pure");
        const bool asynchronous = take("async");
        Method method;
        method.visible = visible;
        method.pure = pure;
        method.asynchronous = asynchronous;
        method.offset = offset;
        if (is(declaration.name) && tokens_.at(position_ + 1).literal == "(") {
            method.name = identifier();
            method.constructor = true;
            method.result = {"void", {}};
        } else {
            method.result = type();
            method.name = identifier();
        }
        if (!take("(")) {
            if (pure || asynchronous || method.constructor || declaration.kind == "Interface")
                fail("invalid field declaration");
            Expr initializer;
            if (take("="))
                initializer = expression();
            expect(";");
            declaration.fields.push_back({method.result, method.name, visible, offset, initializer});
            return;
        }
        if (!is(")")) {
            do {
                auto parameterType = type();
                const auto parameterOffset = current().start;
                auto name = identifier();
                method.parameters.push_back({std::move(parameterType), std::move(name), true, parameterOffset});
                if (method.parameters.size() > 32)
                    fail("parameter limit exceeded");
            } while (take(","));
        }
        expect(")");
        if (declaration.kind == "Interface") {
            expect(";");
            method.asynchronous = method.result.name == "Task";
        } else if (take("=>")) {
            Statement statement;
            statement.kind = "return";
            statement.offset = current().start;
            statement.expression = expression();
            expect(";");
            method.body.push_back(std::move(statement));
        } else {
            method.body = block();
        }
        declaration.methods.push_back(std::move(method));
        if (declaration.methods.size() + declaration.fields.size() > 256)
            fail("member limit exceeded");
    }
    std::vector<Statement> block() {
        if (++blockDepth_ > 48)
            fail("block nesting limit exceeded");
        expect("{");
        std::vector<Statement> result;
        while (!is("}"))
            result.push_back(statement());
        expect("}");
        --blockDepth_;
        return result;
    }
    Statement statement() {
        struct DepthGuard {
            unsigned& depth;
            ~DepthGuard() { --depth; }
        };
        if (++statementDepth_ > 48)
            fail("statement nesting limit exceeded");
        DepthGuard depthGuard{statementDepth_};
        Statement result;
        result.offset = current().start;
        if (take("try")) {
            result.kind = "try";
            result.body = block();
            if (take("catch")) {
                expect("(");
                result.type = type();
                result.name = identifier();
                expect(")");
                result.alternative = block();
            }
            if (take("finally")) {
                result.hasFinally = true;
                result.cleanup = block();
            }
            if (result.name.empty() && !result.hasFinally)
                fail("try requires catch or finally");
            return result;
        }
        if (take("throw")) {
            result.kind = "throw";
            if (!is(";"))
                result.expression = expression();
            expect(";");
            return result;
        }
        if (is("break") || is("continue")) {
            result.kind = current().literal;
            advance();
            expect(";");
            return result;
        }
        if (take("while")) {
            result.kind = "while";
            expect("(");
            result.expression = expression();
            expect(")");
            result.body = is("{") ? block() : std::vector<Statement>{statement()};
            return result;
        }
        if (take("foreach")) {
            result.kind = "foreach";
            expect("(");
            result.type = type();
            result.name = identifier();
            expect("in");
            result.expression = expression();
            expect(")");
            result.body = is("{") ? block() : std::vector<Statement>{statement()};
            return result;
        }
        if (is("{")) {
            result.kind = "block";
            result.body = block();
            return result;
        }
        if (take("if")) {
            result.kind = "if";
            expect("(");
            result.expression = expression();
            expect(")");
            result.body = is("{") ? block() : std::vector<Statement>{statement()};
            if (take("else")) {
                if (is("if"))
                    result.alternative.push_back(statement());
                else
                    result.alternative = is("{") ? block() : std::vector<Statement>{statement()};
            }
            return result;
        }
        if (take("return")) {
            result.kind = "return";
            if (!is(";"))
                result.expression = expression();
            expect(";");
            return result;
        }
        const auto savedPosition = position_;
        if (current().kind == "identifier" && current().literal != "await") {
            // Type declarations are syntactically distinguished by a following
            // identifier. A speculative read never changes the expression AST.
            try {
                Type possibleType = type();
                if (current().kind == "identifier") {
                    result.kind = "local";
                    result.type = std::move(possibleType);
                    result.name = identifier();
                    expect("=");
                    result.expression = expression();
                    expect(";");
                    return result;
                }
            } catch (const Diagnostic&) {
                // Reparse as an expression for the owning diagnostic.
            }
            position_ = savedPosition;
        }
        result.target = expression();
        if (take(";")) {
            result.kind = "expression";
            result.expression = result.target;
            result.target.reset();
            return result;
        }
        expect("=");
        result.kind = "assign";
        result.expression = expression();
        expect(";");
        return result;
    }
    static int precedence(const std::string& value) {
        if (value == "||") return 1;
        if (value == "&&") return 2;
        if (value == "==" || value == "!=") return 3;
        if (value == "<" || value == ">" || value == "<=" || value == ">=") return 4;
        if (value == "+" || value == "-") return 5;
        if (value == "*" || value == "/") return 6;
        return 0;
    }
    Expr make(std::string kind, std::string value, std::size_t offset, std::vector<Expr> children = {}) {
        if (++expressionCount_ > 16384)
            fail("expression node limit exceeded");
        return std::make_shared<Expression>(Expression{std::move(kind), std::move(value), offset,
                                                      std::move(children), {}, {}});
    }
    Expr expression(int minimum = 1) {
        if (++expressionDepth_ > 96)
            fail("expression nesting limit exceeded");
        auto left = unary();
        for (;;) {
            const auto priority = precedence(current().literal);
            if (priority < minimum)
                break;
            const auto operation = current();
            advance();
            auto right = expression(priority + 1);
            left = make("binary", operation.literal, operation.start, {left, right});
        }
        if (minimum == 1 && is("?")) {
            const auto offset = current().start;
            advance();
            auto consequent = expression();
            expect(":");
            auto alternative = expression();
            left = make("conditional", "", offset, {left, consequent, alternative});
        }
        --expressionDepth_;
        return left;
    }
    Expr unary() {
        if (is("!") || is("-") || is("await")) {
            const auto operation = current();
            advance();
            return make(operation.literal == "await" ? "await" : "unary", operation.literal,
                        operation.start, {expression(7)});
        }
        Expr result;
        const auto token = current();
        if (take("new")) {
            const auto name = identifier();
            expect("{");
            std::vector<Expr> fields;
            if (!is("}")) {
                do {
                    const auto offset = current().start;
                    const auto fieldName = identifier();
                    expect("=");
                    fields.push_back(make("named_field", fieldName, offset, {expression()}));
                } while (take(",") && !is("}"));
            }
            expect("}");
            result = make("named_construct", name, token.start, std::move(fields));
        } else if (take("(")) {
            result = expression();
            expect(")");
        } else if (token.kind == "text" || token.kind == "integer" || token.kind == "float" || token.kind == "boolean") {
            advance();
            result = make(token.kind, token.literal, token.start);
        } else if (is("Result") || is("Optional") || is("List") || is("float")) {
            const auto resultType = type();
            result = make("type", resultType.text(), token.start);
            result->type = resultType;
        } else {
            result = make("name", identifier(), token.start);
        }
        for (;;) {
            if (take(".")) {
                const auto offset = current().start;
                result = make("member", identifier(), offset, {result});
            } else if (take("(")) {
                std::vector<Expr> arguments{result};
                if (!is(")")) {
                    do {
                        arguments.push_back(expression());
                    } while (take(","));
                }
                expect(")");
                result = make("call", "", token.start, std::move(arguments));
            } else {
                break;
            }
        }
        return result;
    }
    std::vector<native::Token> tokens_;
    std::size_t position_ = 0;
    std::size_t expressionCount_ = 0;
    unsigned blockDepth_ = 0;
    unsigned statementDepth_ = 0;
    unsigned expressionDepth_ = 0;
};

}

Program parse(const std::string& source) {
    if (source.size() > sourceLimit)
        throw Diagnostic("source byte limit exceeded", 0);
    auto lexed = native::lex(source, native::NumericSyntax::Binary64);
    if (lexed.failure)
        throw Diagnostic(lexed.failure->message, lexed.failure->start);
    if (lexed.tokens.size() > 32768)
        throw Diagnostic("token limit exceeded", 0);
    return Parser(std::move(lexed.tokens)).program();
}

}
