// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../lexer/lexer.hpp"
#include <initializer_list>
#include <string>
#include <vector>

namespace pipelang::model_view {

// Projects the bounded typed XML dialect into the existing view/style checker.
// Every projected token retains an authored XML byte range.
class XmlViewParser {
public:
    explicit XmlViewParser(const std::string &source);
    native::LexResult parse();

private:
    struct Attribute {
        std::string name;
        std::string value;
        std::size_t start = 0;
        std::size_t end = 0;
    };

    struct OpeningTag {
        std::string name;
        std::vector<Attribute> attributes;
        std::size_t start = 0;
        std::size_t end = 0;
        bool empty = false;
    };

    const std::string &source_;
    std::size_t position_ = 0;
    std::size_t nodes_ = 0;
    std::vector<native::Token> tokens_;

    [[noreturn]] void fail(const std::string &message, std::size_t start, std::size_t end) const;
    bool starts(const std::string &text) const;
    void expect(const std::string &text);
    bool whitespace();
    void trivia();
    std::string name();
    Attribute attribute();
    OpeningTag openingTag(bool declaration = false);
    void closingTag(const OpeningTag &opening);
    void validateCharacters() const;
    void declaration();
    void attributes(const OpeningTag &tag, std::initializer_list<const char *> required,
                    std::initializer_list<const char *> optional = {}) const;
    const Attribute *find(const OpeningTag &tag, const std::string &name) const;
    const Attribute &required(const OpeningTag &tag, const std::string &name) const;
    void token(const std::string &kind, const std::string &literal, std::size_t start, std::size_t end);
    void token(const std::string &kind, const std::string &literal, const OpeningTag &tag);
    void token(const std::string &kind, const std::string &literal, const Attribute &attribute);
    std::vector<native::Token> expression(const Attribute &attribute, bool braced) const;
    void identifier(const Attribute &attribute);
    void reference(const Attribute &attribute, bool binding);
    void property(const Attribute &attribute);
    void element(const OpeningTag &tag, std::size_t depth);
    void style(const OpeningTag &tag);
    void styleMember(const OpeningTag &tag);
};

}
