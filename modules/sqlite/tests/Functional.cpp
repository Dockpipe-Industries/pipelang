// SPDX-License-Identifier: Apache-2.0
#include "Tests.hpp"
#include <limits>

using pipelang::sqlite::Value;

void Functional(TestContext& context) {
    using pipelang::sqlite::Connection;
    for (bool readOnly : {false, true}) {
        const auto missing = Connection::OpenExisting(context.Path("missing.sqlite"), readOnly);
        Require(missing.is_failure(), "existing-only open rejects missing database");
        Require(!std::filesystem::exists(context.Path("missing.sqlite")), "existing-only open creates no file");
    }
    auto database = context.Open("typed.sqlite");
    Require(database->Execute("CREATE TABLE typed(n,i,r,t,b,e)").is_success(), "create typed table");
    const std::vector<Value> expected{
        Value::Null(), Value::Integer(std::numeric_limits<std::int64_t>::min()), Value::Real(1.25),
        Value::Text(std::string("a\0é", 4)), Value::Blob({0, 255, 128}), Value::Blob({})};
    const auto inserted = database->Execute("INSERT INTO typed VALUES(?,?,?,?,?,?)", expected);
    Require(inserted.is_success() && inserted.value() == 1, "typed insert");
    const auto selected = database->Query("SELECT n,i,r,t,b,e FROM typed", {}, 1);
    Require(selected.is_success() && selected.value().size() == 1, "typed query");
    Require(selected.value()[0].values == expected, "typed NUL/blob/int64 roundtrip");
    Require(database->Begin().is_success(), "begin");
    Require(database->Execute("INSERT INTO typed(i) VALUES(?)", {Value::Integer(7)}).is_success(), "transaction insert");
    Require(database->Rollback().is_success(), "rollback");
    Require(database->Query("SELECT count(*) FROM typed", {}, 1).value()[0].values[0].integer == 1, "rollback retained prior data");
    Require(database->Begin().is_success(), "begin commit");
    Require(database->Execute("INSERT INTO typed(i) VALUES(?)", {Value::Integer(8)}).is_success(), "commit insert");
    Require(database->Commit().is_success(), "commit");
    Require(database->Execute("CREATE VIRTUAL TABLE documents USING fts5(content)").is_success(), "FTS5 create");
    Require(database->Execute("INSERT INTO documents VALUES(?)", {Value::Text("offline persistent knowledge")}).is_success(), "FTS5 insert");
    Require(database->Close().is_success(), "close");
    auto existing = Connection::OpenExisting(context.Path("typed.sqlite"), false);
    Require(existing.is_success(), "existing-only read/write open");
    database = std::move(existing.value());
    Require(database->Execute("UPDATE typed SET i=i WHERE i=8").is_success(), "existing-only connection permits writes");
    const auto matches = database->Query("SELECT content FROM documents WHERE documents MATCH ?", {Value::Text("persistent")}, 10);
    Require(matches.is_success() && matches.value().size() == 1, "FTS5 persistence after reopening");
    const auto version = database->Query("SELECT sqlite_version(), sqlite_compileoption_used('OMIT_LOAD_EXTENSION')", {}, 1);
    Require(version.is_success() && version.value()[0].values[0].text == "3.54.1", "exact maintained SQLite version");
    Require(version.value()[0].values[1].integer == 1, "extension loading omitted");
    auto readOnly = context.Open("typed.sqlite", true);
    Require(readOnly->Query("SELECT count(*) FROM typed", {}, 1).is_success(), "read-only read");
    const auto rejectedWrite = readOnly->Execute("INSERT INTO typed(i) VALUES(99)");
    Require(rejectedWrite.is_failure() && rejectedWrite.error().starts_with("sqlite.read_only:"), "read-only write refused");
    Require(database->Begin().is_success(), "RAII begin");
    Require(database->Execute("INSERT INTO typed(i) VALUES(42)").is_success(), "RAII pending write");
    database.reset();
    auto afterDestruction = context.Open("typed.sqlite");
    Require(afterDestruction->Query("SELECT count(*) FROM typed", {}, 1).value()[0].values[0].integer == 2, "destruction rolled back");
}
