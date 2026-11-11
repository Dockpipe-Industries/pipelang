// SPDX-License-Identifier: Apache-2.0
#include "Statement.hpp"
#include <cmath>
#include <memory>

namespace pipelang::sqlite {
Statement::Statement(sqlite3* database, const std::string& sql, const std::vector<Value>& parameters, Operation& operation) {
    if (sql.empty() || sql.size() > 1048576 || sql.find('\0') != std::string::npos)
        throw Failure("sqlite.invalid_sql: SQL must contain 1..1048576 bytes without NUL");
    pipelang::model::validate(sql);
    const char* tail = nullptr;
    sqlite3_stmt* prepared = nullptr;
    const int code = sqlite3_prepare_v3(database, sql.data(), static_cast<int>(sql.size()), 0, &prepared, &tail);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> owner(prepared, sqlite3_finalize);
    operation.CheckCode(code);
    if (!prepared)
        throw Failure("sqlite.invalid_sql: an executable statement is required");
    sqlite3_stmt* extra = nullptr;
    const int extraCode = sqlite3_prepare_v3(database, tail, static_cast<int>(sql.data() + sql.size() - tail), 0, &extra, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> extraOwner(extra, sqlite3_finalize);
    operation.CheckCode(extraCode);
    if (extra)
        throw Failure("sqlite.invalid_sql: exactly one statement is allowed");
    const int count = sqlite3_bind_parameter_count(prepared);
    if (parameters.size() > 256 || static_cast<std::size_t>(count) != parameters.size())
        throw Failure("sqlite.invalid_bind: supplied values must match SQLite's highest parameter slot");
    std::size_t ownedBytes = 0;
    for (int index = 1; index <= count; ++index) {
        const auto& value = parameters[static_cast<std::size_t>(index - 1)];
        if (value.text.size() > 1048576 || value.blob.size() > 1048576)
            throw Failure("sqlite.resource_limit: bound value exceeds 1 MiB");
        ownedBytes += sizeof(Value) + value.text.size() + value.blob.size();
        if (ownedBytes > 8 * 1048576)
            throw Failure("sqlite.resource_limit: parameters exceed 8 MiB owned data");
        int bound = SQLITE_MISUSE;
        switch (value.kind) {
        case Value::Kind::Null:
            bound = sqlite3_bind_null(prepared, index);
            break;
        case Value::Kind::Integer:
            bound = sqlite3_bind_int64(prepared, index, value.integer);
            break;
        case Value::Kind::Real:
            if (std::isnan(value.real))
                throw Failure("sqlite.invalid_value: SQLite cannot preserve NaN as Real");
            bound = sqlite3_bind_double(prepared, index, value.real);
            break;
        case Value::Kind::Text:
            pipelang::model::validate(value.text);
            bound = sqlite3_bind_text64(prepared, index, value.text.data(), value.text.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
            break;
        case Value::Kind::Blob:
            bound = value.blob.empty() ? sqlite3_bind_zeroblob64(prepared, index, 0)
                : sqlite3_bind_blob64(prepared, index, value.blob.data(), value.blob.size(), SQLITE_TRANSIENT);
            break;
        }
        operation.CheckCode(bound);
    }
    statement_ = owner.release();
}
Statement::~Statement() {
    sqlite3_finalize(statement_);
}
}
