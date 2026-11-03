// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "syntax.hpp"

#include <cctype>
#include <utility>

namespace pipelang::models {

std::string Type::text() const {
    if (!sourceName.empty())
        return sourceName;
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

std::string nullableDisplayName(std::string identity) {
    auto start = identity.find("Optional<");
    while (start != std::string::npos && start != 0) {
        const auto previous = static_cast<unsigned char>(identity[start - 1]);
        if (!std::isalnum(previous) && previous != '_')
            break;
        start = identity.find("Optional<", start + 1);
    }
    if (start == std::string::npos)
        return identity;
    const auto opening = start + std::string("Optional").size();
    unsigned depth = 0;
    for (auto end = opening; end < identity.size(); ++end) {
        if (identity[end] == '<') {
            ++depth;
        } else if (identity[end] == '>' && --depth == 0) {
            const auto payload = nullableDisplayName(identity.substr(opening + 1, end - opening - 1));
            const auto remaining = nullableDisplayName(identity.substr(end + 1));
            return identity.substr(0, start) + payload + "?" + remaining;
        }
    }
    return identity;
}

} // namespace

std::string Type::displayText() const {
    if (name == "$null")
        return "null";
    if (name == "Optional" && arguments.size() == 1)
        return arguments[0].displayText() + "?";
    std::string result = nullableDisplayName(sourceName.empty() ? name : sourceName);
    if (!arguments.empty()) {
        result += '<';
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (index != 0)
                result += ',';
            result += arguments[index].displayText();
        }
        result += '>';
    }
    return result;
}

namespace {

class Parser {
public:
    Parser(std::vector<native::Token> tokens, bool objectSyntax, std::size_t parameterLimit, bool projectSyntax = false)
        : tokens_(std::move(tokens)), objectSyntax_(objectSyntax), parameterLimit_(parameterLimit),
          projectSyntax_(projectSyntax) {}
    std::string standaloneIdentifier() {
        const auto result = identifier();
        if (!atEnd())
            fail("expected one identifier");
        return result;
    }
    Program moduleHeader() {
        Program result;
        result.tokenCount = tokens_.size();
        if (take("module")) {
            result.module = qualifiedName();
            expect(";");
        }
        while (take("import")) {
            result.importOffsets.push_back(current().start);
            result.imports.push_back(qualifiedName());
            expect(";");
        }
        if (projectSyntax_) {
            if (result.module.size() > 192)
                fail("project module identity exceeds 192 bytes");
            for (const auto& dependency : result.imports) {
                if (dependency.size() > 192)
                    fail("project import identity exceeds 192 bytes");
            }
        }
        return result;
    }
    Program program() {
        auto result = moduleHeader();
        while (!atEnd()) {
            expect("public");
            Declaration declaration;
            declaration.offset = current().start;
            declaration.modelDeclared = projectSyntax_ && take("model");
            declaration.sourceInterface = take("source");
            declaration.kind = current().literal;
            if (declaration.kind != "Class" && declaration.kind != "Record" &&
                declaration.kind != "Enum" && declaration.kind != "Interface")
                fail("expected Class, Record, Enum or Interface");
            advance();
            if (declaration.modelDeclared && declaration.kind != "Class")
                fail("model modifier requires Class");
            if (declaration.sourceInterface && declaration.kind != "Interface")
                fail("source modifier requires Interface");
            declaration.name = identifier();
            declaration.module = result.module;
            declaration.typeParameters = typeParameters();
            if (take(":")) {
                if (declaration.kind != "Class" && !declaration.sourceInterface)
                    fail("only Class or source Interface declarations accept source interface bases");
                do {
                    declaration.interfaces.push_back(type());
                } while (take(","));
            }
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
        result.expressionCount = expressionCount_;
        return result;
    }
private:
    const native::Token& current() const { return tokens_.at(position_); }
    bool atEnd() const { return current().kind == "eof"; }
    bool is(const std::string& value) const {
        return current().kind != "text" && current().literal == value;
    }
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
            "float", "float32", "uint32", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
            "namespace", "new", "noexcept", "not", "null", "nullptr", "operator", "or", "private",
            "protected", "public", "register", "reinterpret_cast", "requires", "return",
            "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
            "string", "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef",
            "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
            "wchar_t", "while", "xor", "not_eq", "and_eq", "or_eq", "xor_eq",
            "char8_t", "char16_t", "char32_t", "std", "pipelang",
            "State", "snapshot", "poll", "pending", "foreach", "in", "finally", "parallel", "module", "import", "lock"
        };
        if (value.size() > 96 || (value.starts_with('_') || value.starts_with("pl_")) || value.find("__") != std::string::npos ||
            reserved.contains(value) || (objectSyntax_ && (value == "base" || value == "override")))
            fail("identifier is reserved or exceeds the identifier limit");
        advance();
        return value;
    }
    std::string qualifiedName() {
        std::string result = identifier();
        while (take("."))
            result += "." + identifier();
        return result;
    }
    std::vector<std::string> typeParameters() {
        std::vector<std::string> result;
        if (!take("<"))
            return result;
        do {
            result.push_back(identifier());
            if (result.size() > 12)
                fail("generic parameter limit exceeded");
        } while (take(","));
        expect(">");
        return result;
    }
    unsigned typeNesting(const Type& value) const {
        unsigned nesting = 0;
        for (const auto& argument : value.arguments) {
            const auto child = 1 + typeNesting(argument);
            if (child > nesting)
                nesting = child;
        }
        return nesting;
    }
    Type type(unsigned depth = 0) {
        if (depth > 12)
            fail("type nesting limit exceeded");
        if (current().kind != "identifier")
            fail("expected type");
        if (is("Optional"))
            fail("Optional<T> source syntax has been replaced by T?");
        Type result{current().literal, {}};
        advance();
        while (result.name != "string" && result.name != "uint32" && result.name != "int" && result.name != "float" && result.name != "float32" && !isSynchronizationHandle(result.name) && take("."))
            result.name += "." + identifier();
        if (take("<")) {
            do {
                result.arguments.push_back(type(depth + 1));
            } while (take(","));
            expect(">");
        }
        unsigned nullableDepth = depth;
        while (take("?")) {
            if (++nullableDepth > 12)
                fail("type nesting limit exceeded");
            result = Type{"Optional", {std::move(result)}};
            if (depth + typeNesting(result) > 12)
                fail("type nesting limit exceeded");
        }
        return result;
    }
    void member(Declaration& declaration) {
        const auto offset = current().start;
        bool visible = true;
        const bool protectedMember = objectSyntax_ && take("protected");
        if (protectedMember || take("private"))
            visible = false;
        else if (declaration.kind != "Interface")
            expect("public");
        else
            take("public");
        if (take("const")) {
            if (declaration.kind != "Class" || !declaration.typeParameters.empty())
                fail("const declarations require a nongeneric class");
            auto constantType = type();
            const auto name = identifier();
            expect("=");
            auto initializer = expression();
            expect(";");
            declaration.constants.push_back({std::move(constantType), name, visible, offset,
                std::move(initializer), protectedMember, declaration.name});
            return;
        }
        const bool virtualMethod = objectSyntax_ && take("virtual");
        const bool overrideMethod = objectSyntax_ && take("override");
        const bool pure = take("pure");
        const bool asynchronous = take("async");
        Method method;
        method.visible = visible;
        method.protectedMember = protectedMember;
        method.virtualMethod = virtualMethod;
        method.overrideMethod = overrideMethod;
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
        method.typeParameters = typeParameters();
        if (!take("(")) {
            if (!method.typeParameters.empty())
                fail("generic fields are not supported");
            if (pure || asynchronous || virtualMethod || overrideMethod || method.constructor || declaration.kind == "Interface")
                fail("invalid field declaration");
            Expr initializer;
            if (take("="))
                initializer = expression();
            expect(";");
            declaration.fields.push_back({method.result, method.name, visible, offset, initializer, protectedMember, {}});
            return;
        }
        if (!is(")")) {
            do {
                auto parameterType = type();
                const auto parameterOffset = current().start;
                auto name = identifier();
                method.parameters.push_back({std::move(parameterType), std::move(name), true, parameterOffset});
                if (method.parameters.size() > parameterLimit_)
                    fail("parameter limit exceeded");
            } while (take(","));
        }
        expect(")");
        if (objectSyntax_ && take(":")) {
            if (!method.constructor)
                fail("only constructors accept a base initializer");
            expect("base");
            expect("(");
            std::vector<Expr> arguments{make("name", "base", offset)};
            if (!is(")")) {
                do {
                    arguments.push_back(expression());
                } while (take(","));
            }
            expect(")");
            method.baseConstructor = make("call", "", offset, std::move(arguments));
        }
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
        if (take("lock")) {
            result.kind = "lock";
            expect("(");
            result.expression = expression();
            expect(")");
            result.body = is("{") ? block() : std::vector<Statement>{statement()};
            return result;
        }
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
        const bool parallel = take("parallel");
        if (parallel && !is("foreach"))
            fail("parallel requires foreach");
        if (take("foreach")) {
            result.kind = parallel ? "parallel_foreach" : "foreach";
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
        if (value == "|") return 3;
        if (value == "^") return 4;
        if (value == "&") return 5;
        if (value == "==" || value == "!=") return 6;
        if (value == "<" || value == ">" || value == "<=" || value == ">=") return 7;
        if (value == "<<" || value == ">>") return 8;
        if (value == "+" || value == "-") return 9;
        if (value == "*" || value == "/" || value == "%") return 10;
        return 0;
    }
    Expr make(std::string kind, std::string value, std::size_t offset, std::vector<Expr> children = {}) {
        if (++expressionCount_ > 16384)
            fail("expression node limit exceeded");
        return std::make_shared<Expression>(Expression{std::move(kind), std::move(value), offset,
                                                      std::move(children), {}, {}});
    }
    std::string binaryOperator() const {
        // Keep angle tokens separate for nested generic type closings. Only
        // expression parsing joins adjacent, contiguous glyphs into a shift.
        if ((is("<") || is(">")) && position_ + 1 < tokens_.size()) {
            const auto& next = tokens_[position_ + 1];
            if (next.literal == current().literal && current().end == next.start)
                return current().literal + next.literal;
        }
        return current().literal;
    }
    Expr expression(int minimum = 1) {
        if (++expressionDepth_ > 96)
            fail("expression nesting limit exceeded");
        auto left = unary();
        for (;;) {
            if (current().kind == "text")
                break;
            const auto operationText = binaryOperator();
            const auto priority = precedence(operationText);
            if (priority < minimum)
                break;
            auto operation = current();
            operation.literal = operationText;
            advance();
            if (operationText == "<<" || operationText == ">>")
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
        if (is("!") || is("~") || is("-") || is("await")) {
            const auto operation = current();
            advance();
            return make(operation.literal == "await" ? "await" : "unary", operation.literal,
                        operation.start, {expression(precedence("*") + 1)});
        }
        Expr result;
        const auto token = current();
        if (take("new")) {
            const auto recordType = type();
            const auto name = recordType.name;
            if (take("(")) {
                std::vector<Expr> arguments{make("name", name, token.start)};
                if (!is(")")) {
                    do {
                        arguments.push_back(expression());
                    } while (take(","));
                }
                expect(")");
                result = make("new_construct", name, token.start, std::move(arguments));
            } else {
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
            }
            result->type = recordType;
        } else if (objectSyntax_ && take("base")) {
            auto receiver = make("base", "base", token.start);
            expect(".");
            result = make("member", identifier(), token.start, {std::move(receiver)});
        } else if (take("null")) {
            result = make("null", "null", token.start);
        } else if (take("this")) {
            result = make("this", "this", token.start);
        } else if (take("(")) {
            result = expression();
            expect(")");
        } else if (token.kind == "text" || token.kind == "integer" || token.kind == "uint32" || token.kind == "float" || token.kind == "float32" || token.kind == "boolean") {
            advance();
            result = make(token.kind, token.literal, token.start);
        } else if (is("Result") || is("Optional") || is("List") || is("Atomic") || is("Array") ||
                   is("Dictionary") || is("HashSet") || is("Queue") || is("Stack") ||
                   is("KeyValuePair") || is("string") || is("uint32") || is("int") || is("float") || is("float32") || is("Lock") || is("SemaphoreSlim") ||
                   isConcurrentCollection(token.literal)) {
            const auto resultType = type();
            result = make("type", resultType.text(), token.start);
            result->type = resultType;
        } else {
            result = make("name", identifier(), token.start);
        }
        for (;;) {
            if (is("<") && (result->kind == "name" || result->kind == "member")) {
                const auto saved = position_;
                try {
                    advance();
                    std::vector<Type> arguments;
                    do {
                        arguments.push_back(type());
                    } while (take(","));
                    expect(">");
                    if (is("(")) {
                        result->kind = result->kind == "name" ? "generic_name" : "generic_member";
                        result->typeArguments = std::move(arguments);
                        continue;
                    }
                } catch (const Diagnostic&) {
                    // The enclosing expression parser owns the refusal.
                }
                position_ = saved;
            }
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
    bool objectSyntax_ = false;
    std::size_t parameterLimit_ = 32;
    bool projectSyntax_ = false;
    std::size_t position_ = 0;
    std::size_t expressionCount_ = 0;
    unsigned blockDepth_ = 0;
    unsigned statementDepth_ = 0;
    unsigned expressionDepth_ = 0;
};

}

Program parse(const std::string& source, bool objectSyntax, std::size_t parameterLimit, bool projectSyntax) {
    if (parameterLimit != 32 && parameterLimit != 64)
        throw Diagnostic("unrecognized parser parameter bound", 0);
    if (source.size() > sourceLimit)
        throw Diagnostic("source byte limit exceeded", 0);
    auto lexed = native::lex(source, native::NumericSyntax::Binary64);
    if (lexed.failure)
        throw Diagnostic(lexed.failure->message, lexed.failure->start);
    if (lexed.tokens.size() > 32768)
        throw Diagnostic("token limit exceeded", 0);
    return Parser(std::move(lexed.tokens), objectSyntax, parameterLimit, projectSyntax).program();
}

std::string moduleIdentityFromStem(const std::string& stem) {
    try {
        auto lexed = native::lex(stem, native::NumericSyntax::Binary64);
        if (lexed.failure)
            throw Diagnostic(lexed.failure->message, lexed.failure->start);
        const auto identity = Parser(std::move(lexed.tokens), true, 32, true).standaloneIdentifier();
        if (identity != stem)
            throw Diagnostic("filename stem must exactly match one identifier", 0);
        return identity;
    } catch (const Diagnostic& diagnostic) {
        throw Diagnostic("filename stem cannot be an implicit module; declare an explicit module: " +
                         std::string(diagnostic.what()), 0);
    }
}

Program parseModuleHeader(const std::string& source) {
    if (source.size() > sourceLimit)
        throw Diagnostic("source byte limit exceeded while indexing project", 0);
    auto lexed = native::lex(source, native::NumericSyntax::Binary64);
    if (lexed.failure)
        throw Diagnostic(lexed.failure->message, lexed.failure->start);
    if (lexed.tokens.size() > 32768)
        throw Diagnostic("token limit exceeded while indexing project", 0);
    return Parser(std::move(lexed.tokens), true, 32, true).moduleHeader();
}

}
