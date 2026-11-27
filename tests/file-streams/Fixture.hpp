// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Model.hpp"
#include <pipelang/io/FileAdapter.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <dirent.h>

namespace api = pipelang_file_tests::source::PipeLang::IO;
namespace crypto_api = pipelang_file_tests::source::PipeLang::Crypto;
namespace tests = pipelang_file_tests::source::PipeLang::IO::Tests;
using Token = pipelang::model::CancellationToken;
using Bytes = pipelang::model::ByteBuffer;
template<class Value>
using FixtureResult = pipelang::model::Result<Value, std::string>;
using CaptureResult = pipelang::model::Result<api::CapturedFile, api::IoFailure>;
using FileAdapter = pipelang::io::FileAdapter<api::FileStreams, api::CapturedFile, api::FileDigest,
    api::FileIdentity, api::IoFailure, api::OutputEffect,
    crypto_api::Sha256Streams, crypto_api::CryptoFailure, crypto_api::Sha256Digest>;
using CryptoAdapter = pipelang::crypto::StreamAdapter<crypto_api::Sha256Streams,
    crypto_api::CryptoFailure, crypto_api::Sha256Digest>;

class Peer final : public api::FileStreams {
public:
    explicit Peer(std::string root) : root_(std::move(root)) { recreate(); }
    void recreate() {
        current_.reset();
        current_ = std::make_unique<FileAdapter>(root_, profile());
    }
    static pipelang::io::PathProfile profile() { return {4096, 32, 262144}; }
    CaptureResult CaptureSnapshot(std::string path, std::int64_t cap, Token token) override {
        return current_->CaptureSnapshot(std::move(path), cap, std::move(token));
    }
    pipelang::model::Result<Bytes, api::IoFailure> ReadSnapshot(std::int64_t id,
            std::int64_t offset, std::int64_t count) override {
        return current_->ReadSnapshot(id, offset, count);
    }
    pipelang::model::Result<void, api::IoFailure> CloseSnapshot(std::int64_t id) override {
        return current_->CloseSnapshot(id);
    }
    pipelang::model::Result<api::FileDigest, api::IoFailure> DigestFile(std::string path,
            std::int64_t cap, Token token) override {
        return current_->DigestFile(std::move(path), cap, std::move(token));
    }
private:
    std::string root_;
    std::unique_ptr<FileAdapter> current_;
};

class Inputs final : public tests::FileInputs {
public:
    Inputs(std::string estate, std::shared_ptr<Peer> peer);
    ~Inputs() override;
    Token probe(std::size_t index) const { return cancellation_.at(index).token(); }

    FixtureResult<void> Create(std::string path, std::int64_t count, std::string pattern) override;
    FixtureResult<void> MakeDirectory(std::string path) override;
    FixtureResult<void> MakeSymlink(std::string path, std::string target) override;
    FixtureResult<void> MakeFifo(std::string path) override;
    FixtureResult<void> MakeHardlink(std::string path, std::string target) override;
    FixtureResult<void> Arm(std::string boundary, std::string action, std::string path, std::int64_t value) override;
    FixtureResult<void> Restore() override;
    FixtureResult<Bytes> ReadFixture(std::string path, std::int64_t count) override;
    FixtureResult<tests::FileUsage> Usage() override;
    FixtureResult<void> CancelProbe(std::int64_t scope) override;
    pipelang::model::Result<api::FileDigest, api::IoFailure> DigestWithWorkLimit(std::string path,
        std::int64_t maximumBytes, std::int64_t systemCalls, Token token) override;
    FixtureResult<void> RecreatePeer() override;
    FixtureResult<void> StartPending(std::string path, std::int64_t cap) override;
    FixtureResult<void> AwaitPendingRead() override;
    CaptureResult FinishPending() override;

    // Called only by the test executable's link wrappers. The production API
    // contains no fault switch/hook. All ordinary operations delegate syscalls.
    bool beforeRead(int descriptor, std::size_t& count);
    bool afterClose(int descriptor);
    void afterRead(int descriptor, ssize_t count);
    void beforeOpen(const char* name);
    void afterOpen(const char* name, int descriptor);
    void provider(const char* boundary);

private:
    struct Schedule {
        std::string boundary;
        std::string action;
        std::string path;
        std::int64_t value = 0;
        std::int64_t remainingInterrupts = 0;
        bool armed = false;
    };

    std::string estate_;
    std::string root_;
    std::shared_ptr<Peer> peer_;
    std::array<pipelang::model::CancellationSource, 8> cancellation_;
    pipelang::model::CancellationSource pendingCancellation_;
    std::mutex mutex_;
    std::condition_variable condition_;
    Schedule schedule_;
    std::atomic<int> observedDescriptor_{-1};
    std::int64_t reads_ = 0;
    std::int64_t hashes_ = 0;
    std::int64_t finishes_ = 0;
    std::int64_t schedules_ = 0;
    std::int64_t opens_ = 0;
    std::string fixtureError_;
    bool paused_ = false;
    bool released_ = false;
    std::thread worker_;
    std::optional<CaptureResult> completed_;
    std::vector<std::pair<std::string, std::string>> renamed_;

    std::string file(const std::string& path) const;
    static void require(bool success, const char* operation);
    static void put(const std::string& path, std::int64_t count, const std::string& pattern);
    bool take(const char* boundary);
    void effect(std::unique_lock<std::mutex>& lock);
    void releaseAndJoin();
};
