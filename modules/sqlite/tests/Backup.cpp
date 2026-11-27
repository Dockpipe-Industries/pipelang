// SPDX-License-Identifier: Apache-2.0
#include "Tests.hpp"

using pipelang::sqlite::Value;

void Backup(TestContext& context) {
    auto source = context.Open("backup-source.sqlite");
    Require(source->Execute("PRAGMA journal_mode=WAL").is_success(), "WAL setup");
    Require(source->Execute("CREATE TABLE records(id PRIMARY KEY, value)").is_success(), "backup schema");
    Require(source->Execute("INSERT INTO records VALUES(?,?)", {Value::Integer(1), Value::Text("before backup")}).is_success(), "backup source insert");
    Require(source->Begin().is_success(), "backup active transaction");
    Require(source->BackupTo(context.Path("not-created.sqlite")).is_failure(), "backup rejects open caller transaction");
    Require(!std::filesystem::exists(context.Path("not-created.sqlite")), "rejected backup retained no file");
    Require(source->Rollback().is_success(), "backup transaction release");
    pipelang::model::CancellationSource cancellation;
    cancellation.cancel();
    Require(source->BackupTo(context.Path("cancelled.sqlite"), cancellation.token()).is_cancelled(), "cancelled backup");
    Require(!std::filesystem::exists(context.Path("cancelled.sqlite")), "cancelled backup retained no file");
    Require(source->BackupTo(context.Path("snapshot.sqlite")).is_success(), "online SQLite backup");
    Require(source->Execute("INSERT INTO records VALUES(?,?)", {Value::Integer(2), Value::Text("after backup")}).is_success(), "source writes continue after snapshot");
    auto snapshot = context.Open("snapshot.sqlite", true);
    const auto rows = snapshot->Query("SELECT id,value FROM records ORDER BY id", {}, 10);
    Require(rows.is_success() && rows.value().size() == 1, "snapshot consistent prior state");
    Require(rows.value()[0].values[1].text == "before backup", "snapshot preserved source data");
    Require(source->BackupTo(context.Path("snapshot.sqlite")).is_failure(), "existing destination never overwritten");
    Require(snapshot->Query("SELECT count(*) FROM records", {}, 1).value()[0].values[0].integer == 1, "existing snapshot unchanged");
    std::filesystem::copy_file(context.Path("snapshot.sqlite"), context.Path("fresh-restore.sqlite"));
    auto restored = context.Open("fresh-restore.sqlite");
    Require(restored->Execute("INSERT INTO records VALUES(?,?)", {Value::Integer(3), Value::Text("independent restored write")}).is_success(), "fresh restored database writable");
    const auto original = source->Query("SELECT id FROM records ORDER BY id", {}, 10);
    const auto recovered = restored->Query("SELECT id FROM records ORDER BY id", {}, 10);
    Require(original.value()[1].values[0].integer == 2 && recovered.value()[1].values[0].integer == 3, "restore and source are independent");
}
