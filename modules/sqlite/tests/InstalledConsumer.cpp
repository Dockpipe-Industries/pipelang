// SPDX-License-Identifier: Apache-2.0
#include <pipelang/sqlite/Connection.hpp>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const auto opened = pipelang::sqlite::Connection::Open(argv[1], false);
    if (!opened.is_success())
        return 3;
    const auto rows = opened.value()->Query("SELECT sqlite_version()", {}, 1);
    if (!rows.is_success() || rows.value()[0].values[0].text != "3.54.1")
        return 4;
    if (!opened.value()->Close().is_success())
        return 5;
    const auto existing = pipelang::sqlite::Connection::OpenExisting(argv[1], false);
    if (!existing.is_success() ||
        !existing.value()->Execute("CREATE TABLE installed_consumer(value INTEGER)").is_success())
        return 6;
    std::cout << "Installed PipeLang::Sqlite consumer passed\n";
}
