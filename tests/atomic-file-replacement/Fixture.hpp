// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Model.hpp"
#include <pipelang/io/AtomicReplacementAdapter.hpp>
#include <pipelang/io/ExclusiveOutputAdapter.hpp>
#include <pipelang/io/FileAdapter.hpp>

#include <filesystem>
#include <string_view>

namespace api = pipelang_report_tests::source::PipeLang::IO;
namespace crypto = pipelang_report_tests::source::PipeLang::Crypto;
namespace tests = pipelang_report_tests::source::PipeLang::IO::ReportTests;
using Bytes = pipelang::model::ByteBuffer;
using Token = pipelang::model::CancellationToken;
using Writer = pipelang::io::ExclusiveOutputAdapter<api::ExclusiveFileOutput, api::FileDigest,
    api::FileIdentity, api::IoFailure, api::OutputEffect, crypto::Sha256Streams, crypto::CryptoFailure,
    crypto::Sha256Digest>;
using Reader = pipelang::io::FileAdapter<api::FileStreams, api::CapturedFile, api::FileDigest,
    api::FileIdentity, api::IoFailure, api::OutputEffect, crypto::Sha256Streams, crypto::CryptoFailure,
    crypto::Sha256Digest>;
using Replace = pipelang::io::AtomicReplacementAdapter<api::AtomicFileReplacement,
    api::FileIdentity, api::IoFailure, api::OutputEffect>;

template<class T>
using FixtureResult = pipelang::model::Result<T,std::string>;

class Fixture final : public tests::ReportFixture {
public:
    explicit Fixture(std::filesystem::path root);
    ~Fixture() override;
    FixtureResult<void> Prepare(std::string target, std::string staged, bool existing, std::string fault) override;
    FixtureResult<void> ChangeStage() override;
    FixtureResult<Bytes> OldHandleBytes() override;
    FixtureResult<Bytes> Block(std::int64_t count) override;
    FixtureResult<std::int64_t> Renames() override;
    FixtureResult<void> Close() override;

    Token cancelled() const { return preCancelled_.token(); }
    Token renameToken() const { return postRename_.token(); }
    void renamed(const char* stage, const char* target, int result);
    bool refuseSync() const;

private:
    std::filesystem::path root_;
    std::string target_;
    std::string staged_;
    std::string fault_;
    pipelang::io::detail::Descriptor previous_;
    pipelang::model::CancellationSource preCancelled_;
    pipelang::model::CancellationSource postRename_;
    std::int64_t renames_ = 0;
    bool reachedRename_ = false;

    static bool valid(std::string_view path, std::string_view prefix);
};
