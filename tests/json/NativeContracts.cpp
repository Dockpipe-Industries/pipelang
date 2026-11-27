// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/json/Reader.hpp>

#include <array>
#include <iostream>
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<class Function>
void refusal(const std::string& code, Function&& function) {
    try {
        std::forward<Function>(function)();
    } catch (const pipelang::json::ReadFailure& failure) {
        require(failure.what() == code, "unexpected native refusal code");
        return;
    }
    throw std::runtime_error("expected native refusal: " + code);
}

void append(pipelang::json::Reader& reader, std::int64_t document, const std::string& text) {
    reader.append(document, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

// An empty deque may allocate eagerly or lazily in the selected standard
// library. Require the same bounded refusal and cleanup at either allocation
// point, rather than making Begin depend on that implementation detail.
void tinyArenaFailure(pipelang::json::Reader& reader, pipelang::json::ReadLimits limits) {
    limits.retainedBytes = 1;
    std::int64_t document = 0;
    refusal("json.memory_limit", [&] {
        document = reader.begin(limits);
        append(reader, document, "null");
        reader.complete(document);
    });
    if (document != 0) {
        require(reader.retainedCapacity(document) == static_cast<std::size_t>(limits.inputBytes),
                "tiny-arena failure retained tree storage");
        require(reader.inputLength(document) == 4, "tiny-arena failure discarded captured input");
        reader.close(document);
    }
    require(reader.liveDocuments() == 0, "tiny-arena failure leaked a document");
}

void allocationAndCleanup() {
    using namespace pipelang::json;
    Reader reader;
    ReadLimits limits;
    limits.inputBytes = 4096;
    limits.retainedBytes = 4096;
    for (int iteration = 0; iteration < 64; ++iteration) {
        const auto document = reader.begin(limits);
        // Retained DOM storage has no upstream fallback. Exhaustion is expected
        // and recoverable; failed parsing must still release its entire arena.
        require(reader.retainedCapacity(document) == 8192, "retained allocation cap changed");
        std::string input = "[";
        for (int index = 0; index < 1000; ++index)
            input += "0,";
        input += "0]";
        append(reader, document, input);
        refusal("json.memory_limit", [&] { reader.complete(document); });
        require(reader.retainedCapacity(document) == 4096, "failed parse retained its tree arena");
        refusal("json.not_complete", [&] { reader.kind({document, document * 2097152}); });
        reader.close(document);
        require(reader.liveDocuments() == 0, "failed document leaked its arena");
    }
    tinyArenaFailure(reader, limits);
    require(reader.liveDocuments() == 0, "failed begin retained a document");
}

void cancellationAndFragments() {
    using namespace pipelang::json;
    Reader reader;
    ReadLimits limits;
    limits.inputBytes = 1048576 + 4;
    limits.retainedBytes = 4096;
    const auto document = reader.begin(limits);
    append(reader, document, "[0,1,2,3]");
    int checkpoints = 0;
    refusal("json.cancelled", [&] {
        reader.complete(document, [&] { return ++checkpoints == 4; });
    });
    require(checkpoints == 4, "parser cancellation checkpoint was skipped");
    reader.close(document);

    const auto captured = reader.begin(limits);
    std::string input(1048576, ' ');
    append(reader, captured, input);
    append(reader, captured, "null");
    const auto root = reader.complete(captured);
    require(reader.kind(root) == Kind::Null, "chunked root changed");
    require(reader.inputSlice(captured, 1048576, 4) ==
            std::vector<std::uint8_t>({'n', 'u', 'l', 'l'}), "exact captured tail changed");
    refusal("json.slice", [&] { reader.inputSlice(captured, 0, 1048577); });
    refusal("json.slice", [&] { reader.inputSlice(captured, -1, 1); });
    reader.close(captured);
}

void allHardCeilings() {
    using namespace pipelang::json;
    Reader reader;
    const std::array<std::int64_t, 6> ceilings = {4194304, 2097152, 64, 4194304, 1048576, 536870912};
    for (int field = 0; field < 6; ++field) {
        ReadLimits limits;
        std::array<std::int64_t*, 6> values = {&limits.inputBytes, &limits.nodes,
            &limits.depth, &limits.decodedBytes, &limits.nameBytes, &limits.retainedBytes};
        *values[field] = ceilings[field] + 1;
        refusal("json.limits", [&] { reader.begin(limits); });
        *values[field] = -1;
        refusal("json.limits", [&] { reader.begin(limits); });
    }
    ReadLimits documentLimits;
    documentLimits.inputBytes = 64;
    documentLimits.retainedBytes = 4096;
    std::vector<std::int64_t> documents;
    for (int index = 0; index < 8; ++index)
        documents.push_back(reader.begin(documentLimits));
    refusal("json.document_limit", [&] { reader.begin(documentLimits); });
    for (auto document : documents)
        reader.close(document);
    require(reader.liveDocuments() == 0, "live-document cap cleanup changed");
}
void aggregateReservations() {
    using namespace pipelang::json;
    require(Reader::aggregateRetainedBytes() == 0, "prior fixture leaked a reservation");
    ReadLimits limits;
    limits.retainedBytes = 4096;
    // Input reservations are intentionally not populated: this proves the
    // process ledger across Readers without a 640MiB resident-memory campaign.
    std::vector<std::unique_ptr<Reader>> readers;
    std::size_t admitted = 0;
    bool refused = false;
    for (int owner = 0; owner < 10; ++owner) {
        readers.push_back(std::make_unique<Reader>(ReaderOptions{16}));
        for (int slot = 0; slot < 16; ++slot) {
            try {
                readers.back()->begin(limits);
                ++admitted;
            } catch (const ReadFailure& error) {
                require(std::string(error.what()) == "json.retained_limit", "aggregate refusal code changed");
                refused = true;
                break;
            }
        }
        if (refused)
            break;
    }
    const std::size_t perDocument = 4194304 + 4096;
    require(refused && admitted == Reader::maximumAggregateRetainedBytes / perDocument,
            "aggregate reservation ceiling is not exact");
    require(Reader::aggregateRetainedBytes() == admitted * perDocument,
            "failed begin consumed a reservation");
    readers.clear();
    require(Reader::aggregateRetainedBytes() == 0, "Reader destruction leaked reservations");

    Reader reader;
    limits.inputBytes = 64;
    const auto document = reader.begin(limits);
    append(reader, document, "[");
    refusal("json.syntax", [&] { reader.complete(document); });
    require(Reader::aggregateRetainedBytes() == 64, "parse failure did not release its arena reservation");
    require(reader.inputLength(document) == 1, "parse failure discarded captured input");
    reader.close(document);
    require(Reader::aggregateRetainedBytes() == 0, "close leaked input reservation");
    tinyArenaFailure(reader, limits);
    require(Reader::aggregateRetainedBytes() == 0, "allocation failure leaked a reservation");
    refusal("json.limits", [] { Reader invalid(ReaderOptions{0}); });
    refusal("json.limits", [] { Reader invalid(ReaderOptions{17}); });
}
} // namespace

int main() {
    try {
        allocationAndCleanup();
        cancellationAndFragments();
        allHardCeilings();
        aggregateReservations();
        std::cout << "JSON native allocation, cancellation, fragment and ceiling contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
