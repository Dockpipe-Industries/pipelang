// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pipelang/sqlite/Connection.hpp>
#include <filesystem>
#include <stdexcept>

struct TestContext {
    std::filesystem::path root;
    explicit TestContext(std::filesystem::path directory) : root(std::move(directory)) {
        if (!root.is_absolute() || std::filesystem::exists(root))
            throw std::runtime_error("tests require a fresh absolute fixture directory");
        std::filesystem::create_directories(root);
    }
    std::string Path(const std::string& name) const {
        return (root / name).string();
    }
    std::shared_ptr<pipelang::sqlite::Connection> Open(const std::string& name,
        bool readOnly = false, pipelang::sqlite::Options options = {}) const {
        const auto opened = pipelang::sqlite::Connection::Open(Path(name), readOnly, options);
        if (!opened.is_success())
            throw std::runtime_error(opened.error());
        return opened.value();
    }
};
