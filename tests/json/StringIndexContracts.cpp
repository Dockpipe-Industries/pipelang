// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/json/Reader.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <memory>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace pipelang::json;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template<class Function>
void refusal(const std::string& expected, Function&& function,
             std::source_location location = std::source_location::current()) {
    const std::string context = std::string(location.function_name()) + ":" + std::to_string(location.line());
    try {
        std::forward<Function>(function)();
    } catch (const ReadFailure& error) {
        if (error.what() != expected)
            throw std::runtime_error("string-index refusal at " + context + ": expected " +
                                     expected + ", actual " + error.what());

        return;
    }

    throw std::runtime_error("missing string-index refusal at " + context + ": expected " + expected);
}

NodeHandle parse(Reader& reader, const std::string& input, ReadLimits limits) {
    const auto document = reader.begin(limits);
    for (std::size_t offset = 0; offset < input.size(); offset += 65536) {
        const auto count = std::min<std::size_t>(65536, input.size() - offset);
        reader.append(document, std::span(reinterpret_cast<const std::uint8_t*>(input.data() + offset), count));
    }

    return reader.complete(document);
}

NodeHandle parse(Reader& reader, const std::string& input) {
    ReadLimits limits;
    limits.inputBytes = static_cast<std::int64_t>(input.size());
    // Leave room for the parsed nodes as well as the decoded input without
    // retaining a default-sized arena for every small document.
    limits.retainedBytes = std::max<std::int64_t>(2 * 1024 * 1024, 4 * limits.inputBytes);
    return parse(reader, input, limits);
}

std::string uniqueArray(std::size_t count) {
    std::string input = "[";
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0)
            input += ',';

        const auto digits = std::to_string(index);
        input += "\"k" + std::string(4 - digits.size(), '0') + digits + "\"";
    }

    input += ']';
    return input;
}

void equalityAndOwnership() {
    Reader reader;
    Reader other;
    const auto root = parse(reader, R"(["a","\u0061","a\u0000b","a\u0000c","a","A","é","e\u0301",7])");
    const auto foreign = parse(other, R"(["a"])");
    const auto second = parse(reader, R"(["a"])");
    const auto baseline = Reader::aggregateRetainedBytes();
    const auto rootCapacity = reader.retainedCapacity(root.document);
    const auto index = reader.beginStringIndex(root.document, {7, 4096});
    require(Reader::aggregateRetainedBytes() == baseline + 4096, "index arena was not charged");
    refusal("json.string_index_limit", [&] { reader.beginStringIndex(root.document, {7, 4096}); });
    require(reader.addString(index, reader.at(root, 0)), "first string is not new");
    require(!reader.addString(index, reader.at(root, 1)), "escaped equality differs");
    require(reader.addString(index, reader.at(root, 2)), "embedded NUL string missing");
    require(reader.addString(index, reader.at(root, 3)), "embedded NUL suffix was truncated");
    require(!reader.addString(index, reader.at(root, 4)), "duplicate value differs");
    require(reader.addString(index, reader.at(root, 5)), "case was folded");
    require(reader.addString(index, reader.at(root, 6)), "composed UTF8 missing");
    require(reader.addString(index, reader.at(root, 7)), "normalization was folded");
    refusal("json.kind", [&] { reader.addString(index, reader.at(root, 8)); });
    refusal("json.node", [&] { reader.addString(index, reader.at(second, 0)); });
    refusal("json.node", [&] { reader.addString(index, other.at(foreign, 0)); });
    refusal("json.node", [&] { reader.addString(index, {root.document, foreign.node}); });
    refusal("json.document", [&] { other.addString(index, other.at(foreign, 0)); });
    refusal("json.document", [&] { other.closeStringIndex(index); });
    refusal("json.string_index", [&] { reader.addString({root.document, index.generation + 1}, reader.at(root, 0)); });
    reader.closeStringIndex(index);
    reader.closeStringIndex(index);
    require(Reader::aggregateRetainedBytes() == baseline, "close did not release independent arena");
    refusal("json.string_index", [&] { reader.addString(index, reader.at(root, 0)); });
    const auto replacement = reader.beginStringIndex(root.document, {7, 4096});
    require(replacement.generation != index.generation, "index generation reused");
    refusal("json.string_index", [&] { reader.closeStringIndex(index); });
    require(reader.addString(replacement, reader.at(root, 0)), "reopened index retained entries");
    reader.close(root.document);
    require(Reader::aggregateRetainedBytes() == baseline - rootCapacity,
            "document close did not release its index and document");
    refusal("json.document", [&] { reader.closeStringIndex(replacement); });
}

void crossDocumentMembership() {
    Reader reader;
    Reader other;
    const auto root = parse(reader, R"(["a","é"])");
    const auto candidates = parse(reader, R"(["\u0061","e\u0301","a\u0000",7])");
    const auto foreign = parse(other, R"(["a"])");
    const auto ownerNode = reader.at(root, 0);
    const auto candidate = reader.at(candidates, 0);
    const auto index = reader.beginStringIndex(root.document, {2, 4096});
    const auto reserved = Reader::aggregateRetainedBytes();
    require(!reader.containsString(index, candidate), "empty index contains a key");
    require(reader.addString(index, ownerNode), "membership inserted a key");
    require(reader.addString(index, reader.at(root, 1)), "second key did not fit");
    require(reader.containsString(index, candidate), "cross-document decoded spelling differs");
    require(!reader.containsString(index, reader.at(candidates, 1)), "lookup normalized Unicode");
    require(!reader.containsString(index, reader.at(candidates, 2)), "lookup truncated an embedded NUL");
    require(Reader::aggregateRetainedBytes() == reserved, "lookup changed retained credit");
    refusal("json.kind", [&] { reader.containsString(index, reader.at(candidates, 3)); });
    refusal("json.document", [&] { reader.containsString(index, other.at(foreign, 0)); });
    refusal("json.cancelled", [&] { reader.containsString(index, candidate, [] { return true; }); });
    refusal("json.node", [&] { reader.addString(index, candidate); });
    reader.close(candidates.document);
    refusal("json.document", [&] { reader.containsString(index, candidate); });
    require(reader.containsString(index, ownerNode), "closing the candidate affected the existing index");
    reader.closeStringIndex(index);
    refusal("json.string_index", [&] { reader.containsString(index, ownerNode); });
}

void cardinalityAndBalance() {
    Reader reader;
    const auto root = parse(reader, uniqueArray(4098));
    const auto baseline = Reader::aggregateRetainedBytes();
    for (int order = 0; order < 3; ++order) {
        const auto index = reader.beginStringIndex(root.document, {4097, 512 * 1024});
        for (std::int64_t position = 0; position < 4097; ++position) {
            std::int64_t element = position;
            if (order == 1)
                element = 4096 - position;
            if (order == 2) {
                if (position % 2 == 0)
                    element = position / 2;
                else
                    element = 4096 - position / 2;
            }

            std::size_t checkpoints = 0;
            require(reader.addString(index, reader.at(root, element), [&] {
                ++checkpoints;
                return false;
            }), "distinct cardinality insertion differed");
            // Short keys use <=2 checkpoints per comparison. This independent
            // callback witness rejects a linear-height tree on sorted/reversed
            // and alternating insertion, including both single/double rotations.
            const auto bound = static_cast<std::size_t>(
                4 * std::bit_width(static_cast<std::uint64_t>(position + 2)) + 4);
            require(checkpoints <= bound, "ordered index lost logarithmic search height");
            if (position == 4095)
                require(reader.kind(reader.at(root, element)) == Kind::String, "4096 boundary changed node");
        }

        require(!reader.addString(index, reader.at(root, 4096)), "duplicate in full index refused");
        refusal("json.string_index_entry_limit", [&] { reader.addString(index, reader.at(root, 4097)); });
        require(!reader.addString(index, reader.at(root, 0)), "entry failure changed prior entries");
        require(reader.kind(root) == Kind::Array, "entry failure poisoned document");
        reader.closeStringIndex(index);
        require(Reader::aggregateRetainedBytes() == baseline, "index cycle accumulated arena reservation");
    }
}

void limitsAndMemoryFailure() {
    Reader reader;
    const auto root = parse(reader, uniqueArray(16));
    const auto baseline = Reader::aggregateRetainedBytes();
    const std::vector<StringIndexLimits> invalid = {
        {0, 4096}, {-1, 4096}, {131073, 4096},
        {1, 0}, {1, -1}, {1, static_cast<std::int64_t>(Reader::maximumAggregateRetainedBytes) + 1}
    };
    for (const auto limits : invalid)
        refusal("json.limits", [&] { reader.beginStringIndex(root.document, limits); });

    const auto maximumAdmission = reader.beginStringIndex(root.document, {131072, 4096});
    reader.closeStringIndex(maximumAdmission);

    // One byte cannot hold even a borrowed view plus child links. This witnesses
    // actual arena allocation failure without relying on std::deque/set eager
    // allocation behavior or a particular standard-library node size.
    const auto tiny = reader.beginStringIndex(root.document, {16, 1});
    refusal("json.memory_limit", [&] { reader.addString(tiny, reader.at(root, 0)); });
    refusal("json.memory_limit", [&] { reader.addString(tiny, reader.at(root, 0)); });
    require(reader.kind(root) == Kind::Array, "index allocation failure poisoned document");
    reader.closeStringIndex(tiny);
    require(Reader::aggregateRetainedBytes() == baseline, "failed tiny arena leaked reservation");

    const auto small = reader.beginStringIndex(root.document, {16, 128});
    std::int64_t inserted = 0;
    bool exhausted = false;
    for (; inserted < 16; ++inserted) {
        try {
            reader.addString(small, reader.at(root, inserted));
        } catch (const ReadFailure& error) {
            require(std::string(error.what()) == "json.memory_limit", "small arena failure was not allocation");
            exhausted = true;
            break;
        }
    }

    require(inserted > 0 && exhausted, "fixture did not witness allocation failure after successful insertion");
    for (std::int64_t element = 0; element < inserted; ++element)
        require(!reader.addString(small, reader.at(root, element)), "allocation failure changed earlier entry");

    reader.closeStringIndex(small);
    const auto active = reader.beginStringIndex(root.document, {1, 4096});
    require(reader.addString(active, reader.at(root, 0)), "explicit one-entry admission failed");
    require(!reader.addString(active, reader.at(root, 0)), "one-entry full duplicate refused");
    refusal("json.string_index_entry_limit", [&] { reader.addString(active, reader.at(root, 1)); });
    reader.closeStringIndex(active);
    ReadLimits incompleteLimits;
    incompleteLimits.inputBytes = 64;
    incompleteLimits.retainedBytes = 4096;
    const auto incomplete = reader.begin(incompleteLimits);
    refusal("json.not_complete", [&] { reader.beginStringIndex(incomplete, {1, 4096}); });
    reader.close(incomplete);

    const auto empty = parse(reader, "[]");
    const auto emptyIndex = reader.beginStringIndex(empty.document, {16, 4096});
    reader.closeStringIndex(emptyIndex);
    reader.close(empty.document);
}

void longComparisonCancellation() {
    Reader reader;
    const std::string prefix(1048577, 'p');
    const auto root = parse(reader, "[\"" + prefix + "a\",\"" + prefix + "b\"]");
    const auto first = reader.at(root, 0);
    const auto second = reader.at(root, 1);
    require(reader.scalarLength(first) > 1048576, "long fixture fits one source string");
    const auto index = reader.beginStringIndex(root.document, {2, 4096});
    refusal("json.cancelled", [&] { reader.addString(index, first, [] { return true; }); });
    require(reader.addString(index, first), "cancelled first insertion changed set");
    std::size_t checkpoints = 0;
    refusal("json.cancelled", [&] {
        reader.addString(index, second, [&] { return ++checkpoints == 10; });
    });
    require(checkpoints == 10, "long comparison did not poll cancellation");
    const auto candidates = parse(reader, "[\"" + prefix + "b\"]");
    const auto candidate = reader.at(candidates, 0);
    checkpoints = 0;
    refusal("json.cancelled", [&] {
        reader.containsString(index, candidate, [&] { return ++checkpoints == 10; });
    });
    require(checkpoints == 10, "long membership comparison did not poll cancellation");
    require(!reader.containsString(index, candidate), "cancelled membership inserted a value");
    reader.close(candidates.document);
    require(!reader.addString(index, first), "cancelled comparison changed prior entry");
    require(reader.addString(index, second), "cancelled comparison inserted the new value");
    require(!reader.addString(index, second), "long equality differs");
    reader.closeStringIndex(index);

    // Empty search checks cancellation exactly before work and before commit.
    // A third callback would observe cancellation, so Success witnesses that
    // the committed insertion is never reclassified by a postcommit poll.
    const auto committed = reader.beginStringIndex(root.document, {2, 4096});
    checkpoints = 0;
    require(reader.addString(committed, first, [&] { return ++checkpoints > 2; }), "committed insertion returned cancellation");
    require(checkpoints == 2, "insertion polled after commit");
    reader.closeStringIndex(committed);
    reader.close(root.document);
}

void lifetimeAndAggregate() {
    require(Reader::aggregateRetainedBytes() == 0, "prior fixture leaked reservations");
    {
        Reader reader;
        ReadLimits limits;
        limits.inputBytes = 256;
        limits.retainedBytes = 4096;
        const auto root = parse(reader, R"(["x","y"])", limits);
        const auto baseline = Reader::aggregateRetainedBytes();
        for (int cycle = 0; cycle < 128; ++cycle) {
            const auto index = reader.beginStringIndex(root.document, {2, 4096});
            require(reader.addString(index, reader.at(root, 0)), "cycle retained old entry");
            reader.closeStringIndex(index);
            require(Reader::aggregateRetainedBytes() == baseline, "cycle retained index memory");
        }

        const auto index = reader.beginStringIndex(root.document, {2, 4096});
        require(reader.addString(index, reader.at(root, 1)), "destructor fixture did not insert");
    }
    require(Reader::aggregateRetainedBytes() == 0, "destruction retained index reservation");

    Reader owner;
    ReadLimits limits;
    limits.inputBytes = 256;
    limits.retainedBytes = 4096;
    const auto root = parse(owner, R"(["x"])", limits);
    const auto index = owner.beginStringIndex(root.document, {1, 4096});
    require(owner.addString(index, owner.at(root, 0)), "aggregate owner did not insert");
    std::vector<std::unique_ptr<Reader>> readers;
    ReadLimits reservation;
    reservation.retainedBytes = 4096;
    bool refused = false;
    for (int outer = 0; outer < 10 && !refused; ++outer) {
        readers.push_back(std::make_unique<Reader>(ReaderOptions{16}));
        for (int slot = 0; slot < 16; ++slot) {
            try {
                readers.back()->begin(reservation);
            } catch (const ReadFailure& error) {
                require(std::string(error.what()) == "json.retained_limit", "aggregate admission failed unexpectedly");
                refused = true;
                break;
            }
        }
    }

    require(refused, "aggregate fixture never reached the existing ceiling");
    const auto before = Reader::aggregateRetainedBytes();
    owner.closeStringIndex(index);
    require(Reader::aggregateRetainedBytes() == before - 4096, "index close did not return aggregate credit");
    const auto remaining = Reader::maximumAggregateRetainedBytes - Reader::aggregateRetainedBytes();
    refusal("json.retained_limit", [&] {
        owner.beginStringIndex(root.document, {1, static_cast<std::int64_t>(remaining + 1)});
    });
    require(Reader::aggregateRetainedBytes() == before - 4096, "failed index reservation consumed credit");
    const auto replacement = owner.beginStringIndex(root.document, {1, 4096});
    require(owner.addString(replacement, owner.at(root, 0)), "aggregate failure poisoned prior document");
    readers.clear();
    owner.close(root.document);
    require(Reader::aggregateRetainedBytes() == 0, "aggregate cleanup leaked index/document reservation");
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view selection = argc == 2 ? argv[1] : "all";
        require(argc == 1 || (argc == 2 && (selection == "operations" || selection == "aggregate")),
                "expected no argument, operations, or aggregate");
        if (selection != "aggregate") {
            equalityAndOwnership();
            crossDocumentMembership();
            cardinalityAndBalance();
            limitsAndMemoryFailure();
            longComparisonCancellation();
        }
        if (selection != "operations")
            lifetimeAndAggregate();
        std::cout << "JSON decoded-string index contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
