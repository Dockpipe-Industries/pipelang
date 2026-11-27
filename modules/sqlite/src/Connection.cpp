// SPDX-License-Identifier: Apache-2.0
#include <pipelang/sqlite/Connection.hpp>
#include "ConnectionState.hpp"
#include "Operation.hpp"
#include "Statement.hpp"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace pipelang::sqlite {
namespace {
template<class T, class Callback>
Connection::Result<T> Run(ConnectionState& state, Connection::Token cancellation, Callback callback) {
    using Result = Connection::Result<T>;
    try {
        Operation operation(state, std::move(cancellation));
        if constexpr (std::is_void_v<T>) {
            callback(operation);
            return Result::Success();
        } else {
            return Result::Success(callback(operation));
        }
    } catch (const pipelang::model::OperationCancelled& error) {
        return Result::Cancelled(error.what());
    } catch (const Failure& error) {
        return Result::Failure(error.what());
    } catch (const pipelang::model::ExecutionFailure& error) {
        return Result::Failure(std::string("sqlite.invalid_value: ") + error.what());
    } catch (const std::bad_alloc&) {
        return Result::Failure("sqlite.resource_limit: native allocation failed");
    } catch (const std::exception&) {
        return Result::Failure("sqlite.storage: unexpected native operation failure");
    }
}

void CheckPath(const std::string& path) {
    if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos || path.starts_with("file:"))
        throw Failure("sqlite.invalid_path: use an explicit filesystem path, not a URI");
    const std::filesystem::path filesystemPath(path);
    if (!filesystemPath.is_absolute())
        throw Failure("sqlite.invalid_path: an absolute filesystem path is required");
    for (const auto& component : filesystemPath) {
        if (component == "." || component == "..")
            throw Failure("sqlite.invalid_path: dot path components are not allowed");
    }
    pipelang::model::validate(path);
}

Value ReadValue(sqlite3_stmt* statement, int column) {
    const int type = sqlite3_column_type(statement, column);
    if (type == SQLITE_NULL)
        return Value::Null();
    if (type == SQLITE_INTEGER)
        return Value::Integer(sqlite3_column_int64(statement, column));
    if (type == SQLITE_FLOAT)
        return Value::Real(sqlite3_column_double(statement, column));
    const int bytes = sqlite3_column_bytes(statement, column);
    if (bytes < 0 || bytes > 1048576)
        throw Failure("sqlite.resource_limit: returned value exceeds 1 MiB");
    if (type == SQLITE_TEXT) {
        const auto* text = sqlite3_column_text(statement, column);
        if (!text && bytes != 0)
            throw Failure("sqlite.resource_limit: SQLite text allocation failed");
        std::string owned = bytes == 0 ? std::string{} : std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
        pipelang::model::validate(owned);
        return Value::Text(std::move(owned));
    }
    const auto* blob = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, column));
    if (!blob && bytes != 0)
        throw Failure("sqlite.resource_limit: SQLite BLOB allocation failed");
    return Value::Blob(bytes == 0 ? std::vector<std::uint8_t>{} : std::vector<std::uint8_t>(blob, blob + bytes));
}

void ExecuteControl(ConnectionState& state, const std::string& sql, Operation& operation) {
    Statement statement(state.database, sql, {}, operation);
    operation.CheckCode(sqlite3_step(statement.Get()));
}
}

Connection::Connection(std::unique_ptr<ConnectionState> state) : state_(std::move(state)) {}
Connection::~Connection() = default;

Connection::Result<std::shared_ptr<Connection>> Connection::Open(const std::string& path, bool readOnly, Options options) {
    return OpenWithMode(path, readOnly, true, options);
}

Connection::Result<std::shared_ptr<Connection>> Connection::OpenExisting(const std::string& path, bool readOnly, Options options) {
    return OpenWithMode(path, readOnly, false, options);
}

Connection::Result<std::shared_ptr<Connection>> Connection::OpenWithMode(
    const std::string& path, bool readOnly, bool allowCreate, Options options) {
    using Result = Connection::Result<std::shared_ptr<Connection>>;
    try {
        CheckPath(path);
        if (options.busyTimeoutMilliseconds < 0 || options.busyTimeoutMilliseconds > 5000 ||
            options.queryTimeoutMilliseconds < 1 || options.queryTimeoutMilliseconds > 10000 ||
            options.maximumVmSteps < 1000 || options.maximumVmSteps > 100000000)
            throw Failure("sqlite.invalid_options: select bounded positive operation budgets");
        if (sqlite3_libversion_number() != 3054001 || !sqlite3_compileoption_used("ENABLE_FTS5") ||
            !sqlite3_compileoption_used("THREADSAFE=1"))
            throw Failure("sqlite.version: linked SQLite identity differs from the pinned build");
        auto state = std::make_unique<ConnectionState>();
        state->options = options;
        const int flags = (readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE)
            | (!readOnly && allowCreate ? SQLITE_OPEN_CREATE : 0)
            | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
        const int code = sqlite3_open_v2(path.c_str(), &state->database, flags, nullptr);
        if (code != SQLITE_OK)
            throw Failure("sqlite.storage: cannot open database (" + std::to_string(code) + ")");
        sqlite3_extended_result_codes(state->database, 1);
        sqlite3_db_config(state->database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        sqlite3_limit(state->database, SQLITE_LIMIT_LENGTH, 2 * 1048576);
        sqlite3_limit(state->database, SQLITE_LIMIT_SQL_LENGTH, 1048576);
        sqlite3_limit(state->database, SQLITE_LIMIT_COLUMN, 32);
        sqlite3_limit(state->database, SQLITE_LIMIT_VARIABLE_NUMBER, 256);
        sqlite3_limit(state->database, SQLITE_LIMIT_EXPR_DEPTH, 100);
        sqlite3_limit(state->database, SQLITE_LIMIT_ATTACHED, 0);
        return Result::Success(std::shared_ptr<Connection>(new Connection(std::move(state))));
    } catch (const Failure& error) {
        return Result::Failure(error.what());
    } catch (const std::exception&) {
        return Result::Failure("sqlite.storage: invalid path or native allocation failure");
    }
}

Connection::Result<std::int64_t> Connection::Execute(const std::string& sql, const std::vector<Value>& parameters, Token cancellation) {
    return Run<std::int64_t>(*state_, std::move(cancellation), [&](Operation& operation) {
        Statement statement(state_->database, sql, parameters, operation);
        const auto before = sqlite3_total_changes64(state_->database);
        int code;
        do {
            code = sqlite3_step(statement.Get());
            operation.CheckCode(code);
        } while (code == SQLITE_ROW);
        return sqlite3_total_changes64(state_->database) == before ? 0 : sqlite3_changes64(state_->database);
    });
}

Connection::Result<std::vector<Row>> Connection::Query(const std::string& sql, const std::vector<Value>& parameters,
                                                      std::size_t maximumRows, Token cancellation) {
    return Run<std::vector<Row>>(*state_, std::move(cancellation), [&](Operation& operation) {
        if (maximumRows == 0 || maximumRows > 256)
            throw Failure("sqlite.resource_limit: maximumRows must be within 1..256");
        Statement statement(state_->database, sql, parameters, operation);
        if (!sqlite3_stmt_readonly(statement.Get()))
            throw Failure("sqlite.invalid_query: Query accepts read-only statements; use Execute for mutations");
        const int columns = sqlite3_column_count(statement.Get());
        if (columns > 32)
            throw Failure("sqlite.resource_limit: result exceeds 32 columns");
        std::vector<Row> rows;
        std::size_t ownedBytes = 0;
        for (;;) {
            const int code = sqlite3_step(statement.Get());
            operation.CheckCode(code);
            if (code == SQLITE_DONE)
                break;
            if (rows.size() == maximumRows)
                throw Failure("sqlite.resource_limit: query exceeds the requested row limit");
            Row row;
            ownedBytes += sizeof(Row);
            for (int column = 0; column < columns; ++column) {
                auto value = ReadValue(statement.Get(), column);
                ownedBytes += sizeof(Value) + value.text.size() + value.blob.size();
                if (ownedBytes > 8 * 1048576)
                    throw Failure("sqlite.resource_limit: result exceeds 8 MiB owned data");
                row.values.push_back(std::move(value));
            }
            rows.push_back(std::move(row));
        }
        return rows;
    });
}

Connection::Result<void> Connection::Begin(Token cancellation) {
    return Run<void>(*state_, std::move(cancellation), [&](Operation& operation) {
        ExecuteControl(*state_, "BEGIN IMMEDIATE", operation);
    });
}
Connection::Result<void> Connection::Commit(Token cancellation) {
    return Run<void>(*state_, std::move(cancellation), [&](Operation& operation) {
        ExecuteControl(*state_, "COMMIT", operation);
    });
}
Connection::Result<void> Connection::Rollback(Token cancellation) {
    return Run<void>(*state_, std::move(cancellation), [&](Operation& operation) {
        ExecuteControl(*state_, "ROLLBACK", operation);
    });
}

Connection::Result<void> Connection::Close(Token cancellation) {
    try {
        if (state_->owner != std::this_thread::get_id())
            throw Failure("sqlite.thread: connection belongs to another thread");
        if (!state_->database)
            throw Failure("sqlite.closed: connection is closed");
        if (state_->active)
            throw Failure("sqlite.busy: an operation is still active");
        if (cancellation.is_cancelled())
            return Result<void>::Cancelled("sqlite.cancelled: close cancelled");
        if (!sqlite3_get_autocommit(state_->database))
            sqlite3_exec(state_->database, "ROLLBACK", nullptr, nullptr, nullptr);
        const int code = sqlite3_close(state_->database);
        if (code != SQLITE_OK)
            throw Failure("sqlite.busy: close could not release the database");
        state_->database = nullptr;
        return Result<void>::Success();
    } catch (const Failure& error) {
        return Result<void>::Failure(error.what());
    }
}

Connection::Result<void> Connection::BackupTo(const std::string& newDestination, Token cancellation) {
    return Run<void>(*state_, std::move(cancellation), [&](Operation& operation) {
        CheckPath(newDestination);
        if (!sqlite3_get_autocommit(state_->database))
            throw Failure("sqlite.transaction: finish the current transaction before backup");
        const int descriptor = ::open(newDestination.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0)
            throw Failure("sqlite.storage: backup destination must be a new writable file");
        struct stat created{};
        const int statCode = fstat(descriptor, &created);
        ::close(descriptor);
        if (statCode != 0)
            throw Failure("sqlite.storage: cannot identify the new backup file");
        try {
            sqlite3* rawDestination = nullptr;
            const int code = sqlite3_open_v2(newDestination.c_str(), &rawDestination,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
            std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> destination(rawDestination, sqlite3_close_v2);
            operation.CheckCode(code);
            sqlite3_backup* rawBackup = sqlite3_backup_init(destination.get(), "main", state_->database, "main");
            if (!rawBackup)
                operation.CheckCode(sqlite3_errcode(destination.get()));
            if (!rawBackup)
                throw Failure("sqlite.storage: cannot initialize SQLite backup");
            std::unique_ptr<sqlite3_backup, decltype(&sqlite3_backup_finish)> backup(rawBackup, sqlite3_backup_finish);
            for (;;) {
                operation.Check();
                const int step = sqlite3_backup_step(backup.get(), 64);
                if (step == SQLITE_DONE)
                    break;
                if (step != SQLITE_OK && step != SQLITE_BUSY && step != SQLITE_LOCKED)
                    operation.CheckCode(step);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const int finished = sqlite3_backup_finish(backup.release());
            if (finished != SQLITE_OK)
                operation.CheckCode(finished);
            const int closed = sqlite3_close(destination.get());
            if (closed != SQLITE_OK)
                operation.CheckCode(closed);
            destination.release();
        } catch (...) {
            struct stat current{};
            if (lstat(newDestination.c_str(), &current) == 0 && current.st_dev == created.st_dev && current.st_ino == created.st_ino)
                ::unlink(newDestination.c_str());
            throw;
        }
    });
}
}
