// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Options.hpp"
#include "Row.hpp"
#include <pipelang/model/runtime.hpp>
#include <memory>

namespace pipelang::sqlite {
struct ConnectionState;

class Connection final {
public:
    using Token = pipelang::model::CancellationToken;
    template<class T> using Result = pipelang::model::Result<T, std::string>;
    static Result<std::shared_ptr<Connection>> Open(const std::string& path, bool readOnly, Options options = {});
    // Opens only an existing database; never creates a missing filesystem entry.
    static Result<std::shared_ptr<Connection>> OpenExisting(const std::string& path, bool readOnly, Options options = {});
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Result<std::int64_t> Execute(const std::string& sql, const std::vector<Value>& parameters = {}, Token cancellation = {});
    Result<std::vector<Row>> Query(const std::string& sql, const std::vector<Value>& parameters,
                                  std::size_t maximumRows, Token cancellation = {});
    Result<void> Begin(Token cancellation = {});
    Result<void> Commit(Token cancellation = {});
    Result<void> Rollback(Token cancellation = {});
    Result<void> Close(Token cancellation = {});
    Result<void> BackupTo(const std::string& newDestination, Token cancellation = {});
private:
    static Result<std::shared_ptr<Connection>> OpenWithMode(const std::string& path, bool readOnly,
                                                         bool allowCreate, Options options);
    explicit Connection(std::unique_ptr<ConnectionState> state);
    std::unique_ptr<ConnectionState> state_;
};
}
