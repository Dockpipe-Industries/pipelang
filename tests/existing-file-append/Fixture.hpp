// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Model.hpp"
#include <pipelang/io/ExistingFileAppendAdapter.hpp>
#include <pipelang/io/FileAdapter.hpp>

#include <filesystem>
#include <string_view>

namespace api = pipelang_append_tests::source::PipeLang::IO;
namespace crypto = pipelang_append_tests::source::PipeLang::Crypto;
namespace tests = pipelang_append_tests::source::PipeLang::IO::AppendTests;
using Bytes = pipelang::model::ByteBuffer;
using Token = pipelang::model::CancellationToken;
using Append = pipelang::io::ExistingFileAppendAdapter<api::ExistingFileAppend, api::AppendFileState,
    api::FileIdentity, api::IoFailure, api::OutputEffect>;
using Reader = pipelang::io::FileAdapter<api::FileStreams, api::CapturedFile, api::FileDigest,
    api::FileIdentity, api::IoFailure, api::OutputEffect, crypto::Sha256Streams, crypto::CryptoFailure,
    crypto::Sha256Digest>;

template<class T>
using FixtureResult = pipelang::model::Result<T,std::string>;

class Fixture final : public tests::AppendFixture {
public:
    explicit Fixture(std::filesystem::path root);
    ~Fixture() override;
    FixtureResult<void> Prepare(std::string relative, std::string fault) override;
    FixtureResult<void> ChangeRevision() override;
    FixtureResult<void> Close() override;
    FixtureResult<std::int64_t> Effects() override;

    Token cancelled() const { return preCancelled_.token(); }
    Token writeToken() const { return postWrite_.token(); }
    void opened(const char* name, int flags, int descriptor);
    std::size_t beforeWrite(int descriptor, std::size_t length) const;
    void afterWrite(int descriptor, ssize_t count);
    void closed(int descriptor);

private:
    std::filesystem::path root_;
    std::string target_;
    std::string fault_;
    int descriptor_ = -1;
    std::int64_t effects_ = 0;
    pipelang::model::CancellationSource preCancelled_;
    pipelang::model::CancellationSource postWrite_;

    static bool valid(std::string_view relative);
};
