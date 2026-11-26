// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Model.hpp"
#include <pipelang/io/ExclusiveOutputAdapter.hpp>
#include <pipelang/io/FileAdapter.hpp>

#include <array>
#include <filesystem>
#include <string_view>

namespace api = pipelang_output_tests::source::PipeLang::IO;
namespace crypto = pipelang_output_tests::source::PipeLang::Crypto;
namespace tests = pipelang_output_tests::source::PipeLang::IO::OutputTests;
using Bytes = pipelang::model::ByteBuffer;
using Token = pipelang::model::CancellationToken;
using Writer = pipelang::io::ExclusiveOutputAdapter<api::ExclusiveFileOutput, api::FileDigest,
    api::FileIdentity, api::IoFailure, api::OutputEffect, crypto::Sha256Streams, crypto::CryptoFailure,
    crypto::Sha256Digest>;
using Reader = pipelang::io::FileAdapter<api::FileStreams, api::CapturedFile, api::FileDigest,
    api::FileIdentity, api::IoFailure, api::OutputEffect, crypto::Sha256Streams, crypto::CryptoFailure,
    crypto::Sha256Digest>;

template<class Value>
using FixtureResult = pipelang::model::Result<Value, std::string>;

class Fixture final : public tests::OutputFixture {
public:
    explicit Fixture(std::filesystem::path root);
    ~Fixture() override;
    Token cancelled() const { return preCancelled_.token(); }
    Token probe(std::size_t index) const { return tokens_.at(index).token(); }

    FixtureResult<void> Arm(std::string relative, std::string fault, std::int64_t scope) override;
    FixtureResult<void> Restore() override;
    FixtureResult<std::int64_t> Mode(std::string relative) override;
    FixtureResult<Bytes> Block(std::int64_t length) override;
    FixtureResult<std::int64_t> Effects() override;
    FixtureResult<std::int64_t> OpenOperations() override;

    void created(const char* name, int descriptor);
    bool beforeWrite(int descriptor, std::size_t& count, ssize_t& failure);
    void afterWrite(int descriptor, ssize_t count);
    bool beforeSync(int descriptor);
    void afterSync(int descriptor, int result);
    bool afterClose(int descriptor, int result);

private:
    std::filesystem::path root_;
    std::string target_;
    std::string fault_;
    std::string failure_;
    int observedDescriptor_ = -1;
    std::size_t scope_ = 0;
    std::int64_t effects_ = 0;
    bool shortWritten_ = false;
    pipelang::model::CancellationSource preCancelled_;
    std::array<pipelang::model::CancellationSource,3> tokens_;

    static bool validPath(std::string_view relative);
};
