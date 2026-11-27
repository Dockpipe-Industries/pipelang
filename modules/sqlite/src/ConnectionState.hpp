// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pipelang/sqlite/Options.hpp>
#include <sqlite3.h>
#include <thread>

namespace pipelang::sqlite {
struct ConnectionState {
    sqlite3* database = nullptr;
    std::thread::id owner = std::this_thread::get_id();
    Options options;
    bool active = false;
    ~ConnectionState() {
        if (database) {
            if (!sqlite3_get_autocommit(database))
                sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
            sqlite3_close_v2(database);
        }
    }
};
}
