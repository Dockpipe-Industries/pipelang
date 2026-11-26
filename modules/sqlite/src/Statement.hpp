// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Operation.hpp"
#include <pipelang/sqlite/Value.hpp>

namespace pipelang::sqlite {
class Statement final {
public:
    Statement(sqlite3* database, const std::string& sql, const std::vector<Value>& parameters, Operation& operation);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* Get() const {
        return statement_;
    }
private:
    sqlite3_stmt* statement_ = nullptr;
};
}
