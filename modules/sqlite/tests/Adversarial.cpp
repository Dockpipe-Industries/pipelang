// SPDX-License-Identifier: Apache-2.0
#include "Tests.hpp"
#include <chrono>
#include <cmath>
#include <thread>

using pipelang::sqlite::Connection;
using pipelang::sqlite::Options;
using pipelang::sqlite::Value;

void Adversarial(TestContext& context) {
    auto database = context.Open("hostile.sqlite");
    Require(database->Execute("CREATE TABLE data(value)").is_success(), "hostile table");
    const std::string injection = "'; DROP TABLE data; --";
    Require(database->Execute("INSERT INTO data VALUES(?)", {Value::Text(injection)}).is_success(), "injection value insert");
    const auto selected = database->Query("SELECT value FROM data WHERE value=?", {Value::Text(injection)}, 1);
    Require(selected.is_success() && selected.value()[0].values[0].text == injection, "parameters remain data");
    Require(database->Execute("INSERT INTO data VALUES(1); DROP TABLE data").is_failure(), "multiple SQL refused");
    Require(database->Query("SELECT count(*) FROM data", {}, 1).value()[0].values[0].integer == 1, "multiple SQL was not executed");
    const auto reused = database->Query("SELECT ?1,?2 WHERE ?1=?1 AND EXISTS(SELECT 1 WHERE ?2=?2)",
        {Value::Text(injection), Value::Text("principal")}, 1);
    Require(reused.is_success() && reused.value().size() == 1, "numbered slots reuse across nested predicates");
    Require(reused.value()[0].values[0].text == injection && reused.value()[0].values[1].text == "principal", "numbered values remain data");
    const auto named = database->Query("SELECT :tenant,:tenant,@principal,$policy,? WHERE EXISTS(SELECT 1 WHERE :tenant=?1 AND @principal=?2)",
        {Value::Text(injection), Value::Text("principal"), Value::Integer(42), Value::Blob({0, 255})}, 1);
    Require(named.is_success() && named.value().size() == 1, "named reused and mixed slots");
    Require(named.value()[0].values == std::vector<Value>{Value::Text(injection), Value::Text(injection),
        Value::Text("principal"), Value::Integer(42), Value::Blob({0, 255})}, "SQLite slot order is authoritative");
    const auto sparse = database->Query("SELECT ?3", {Value::Integer(1), Value::Integer(2), Value::Integer(3)}, 1);
    Require(sparse.is_success() && sparse.value()[0].values[0].integer == 3, "every slot supplied for sparse numbering");
    Require(database->Query("SELECT ?1,?1", {Value::Integer(1), Value::Integer(2)}, 1).is_failure(), "excess assignment for reused slot refused");
    Require(database->Query("SELECT :same,:same", {}, 1).is_failure(), "missing named slot refused");
    Require(database->Query("SELECT ?257", std::vector<Value>(257), 1).is_failure(), "variable slot limit retained");
    Require(database->Query("SELECT ?", {}, 1).is_failure(), "missing bind refused");
    Require(database->Query("SELECT ?", {Value::Integer(1), Value::Integer(2)}, 1).is_failure(), "extra bind refused");
    Require(database->Query("SELECT 1; -- permitted comment\n", {}, 1).is_success(), "trailing comment");
    Require(database->Query("SELECT 1 UNION ALL SELECT 2", {}, 1).is_failure(), "row limit fails instead of truncating");
    Require(database->Query("SELECT 1", {}, 257).is_failure(), "requested row budget checked");
    Require(database->Query("DELETE FROM data RETURNING value", {}, 1).is_failure(), "mutating Query rejected before execution");
    Require(database->Query("SELECT ?", {Value::Text(std::string("\xff", 1))}, 1).is_failure(), "invalid UTF8 text refused");
    Require(database->Query("SELECT ?", {Value::Real(std::nan(""))}, 1).is_failure(), "NaN does not silently become NULL");
    Require(database->Query("SELECT load_extension(?)", {Value::Text("untrusted")}, 1).is_failure(), "extension loading unavailable");
    const Value maximum = Value::Blob(std::vector<std::uint8_t>(1048576, 255));
    const auto maximumRoundtrip = database->Query("SELECT ?", {maximum}, 1);
    Require(maximumRoundtrip.is_success() && maximumRoundtrip.value()[0].values[0] == maximum, "1MiB BLOB roundtrip");
    Require(database->Query("SELECT ?", {Value::Blob(std::vector<std::uint8_t>(1048577))}, 1).is_failure(), "oversized bind refused");
    Require(database->Query("SELECT zeroblob(1048577)", {}, 1).is_failure(), "oversized returned value refused");
    pipelang::model::CancellationSource cancelled;
    cancelled.cancel();
    Require(database->Query("SELECT 1", {}, 1, cancelled.token()).is_cancelled(), "pre-cancelled operation");
    const std::string expensive = "WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<10000000) SELECT sum(x) FROM n";
    Options workOptions;
    workOptions.maximumVmSteps = 100000000;
    auto work = context.Open("hostile.sqlite", false, workOptions);
    pipelang::model::CancellationSource cancellation;
    std::thread cancelThread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cancellation.cancel();
    });
    const auto interrupted = work->Query(expensive, {}, 1, cancellation.token());
    cancelThread.join();
    Require(interrupted.is_cancelled(), "progress handler cancellation");
    Require(work->Query("SELECT 1", {}, 1).is_success(), "cancelled statement finalized and handlers reset");
    Options deadlineOptions;
    deadlineOptions.queryTimeoutMilliseconds = 1;
    deadlineOptions.maximumVmSteps = 100000000;
    const auto timed = context.Open("hostile.sqlite", false, deadlineOptions)->Query(expensive, {}, 1);
    Require(timed.is_failure() && timed.error().starts_with("sqlite.timeout:"), "operation deadline");
    Options budgetOptions;
    budgetOptions.maximumVmSteps = 1000;
    const auto budget = context.Open("hostile.sqlite", false, budgetOptions)->Query(expensive, {}, 1);
    Require(budget.is_failure() && budget.error().starts_with("sqlite.budget:"), "VM work budget");
    Require(database->Begin().is_success(), "writer lock begin");
    Options busyOptions;
    busyOptions.busyTimeoutMilliseconds = 20;
    auto blocked = context.Open("hostile.sqlite", false, busyOptions);
    const auto beforeBusy = std::chrono::steady_clock::now();
    const auto busy = blocked->Execute("INSERT INTO data VALUES(2)");
    Require(busy.is_failure() && busy.error().starts_with("sqlite.busy:"), "bounded busy callback");
    Require(std::chrono::steady_clock::now() - beforeBusy < std::chrono::seconds(1), "busy wait remained bounded");
    Require(database->Rollback().is_success(), "writer release");
    bool wrongThread = false;
    std::thread foreign([&] {
        const auto result = database->Query("SELECT 1", {}, 1);
        wrongThread = result.is_failure() && result.error().starts_with("sqlite.thread:");
    });
    foreign.join();
    Require(wrongThread, "foreign thread refused");
    Require(database->Close().is_success(), "close hostile connection");
    Require(database->Query("SELECT 1", {}, 1).is_failure(), "stale closed handle refused");
    Require(Connection::Open(context.Path("missing/absent.sqlite"), false).is_failure(), "disk/open error");
    Require(Connection::Open("../escape.sqlite", false).is_failure(), "unsafe relative path refused");
    std::filesystem::create_symlink(context.Path("hostile.sqlite"), context.Path("symlink.sqlite"));
    Require(Connection::Open(context.Path("symlink.sqlite"), false).is_failure(), "final symlink refused");
    auto full = context.Open("full.sqlite");
    Require(full->Execute("CREATE TABLE limited(value)").is_success(), "full table");
    Require(full->Execute("PRAGMA max_page_count=2").is_success(), "page-count disk budget");
    const auto diskFull = full->Execute("INSERT INTO limited VALUES(?)", {Value::Blob(std::vector<std::uint8_t>(16384))});
    Require(diskFull.is_failure() && diskFull.error().starts_with("sqlite.storage:"), "SQLite full/disk error");
    Require(full->Query("SELECT count(*) FROM limited", {}, 1).value()[0].values[0].integer == 0, "failed full insert rolled back");
}
