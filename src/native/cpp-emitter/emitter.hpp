#pragma once

#include "core_json.hpp"

#include <map>
#include <string>

namespace pipelang::native {

// This backend accepts already admitted Core, not source language text. The Go
// caller owns complete Core semantic validation before invoking this process.
class Emitter {
public:
    Json generate(const Json& program);

private:
    using Scope = std::map<int, std::string>;

    void append(const std::string& text);
    std::string temporary(const std::string& type, const std::string& expression);
    std::string expression(const Json& node, const Scope& scope);
    std::string declaration(const Json& function) const;
    void collectTypes(const Json& value);
    void emitRecords();

    std::string source_;
    std::size_t temporaryCount_ = 0;
    std::string returnType_;
    std::map<std::string, std::string> functions_;
    std::map<std::string, Json> records_;
};

}
