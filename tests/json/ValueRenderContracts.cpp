// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/json/Reader.hpp>

#include <algorithm>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace pipelang::json;

void require(bool matched, const char* reason) {
    if (!matched)
        throw std::runtime_error(reason);
}

template<class Operation>
void refuse(Operation op, std::string_view expected) {
    try {
        op();
    } catch (const ReadFailure& error) {
        require(error.what() == expected, "unexpected rendering refusal");
        return;
    }
    throw std::runtime_error("expected rendering refusal");
}

NodeHandle parse(Reader& reader, std::string_view text) {
    auto document = reader.begin({});
    auto remaining = std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    while (!remaining.empty()) {
        const auto bytes = std::min(remaining.size(), Reader::maximumFragmentBytes);
        reader.append(document, remaining.first(bytes));
        remaining = remaining.subspan(bytes);
    }
    return reader.complete(document);
}

void formatting() {
    Reader reader;
    const auto root = parse(reader, R"({"z":0.0100,"\u00e4":"\u0000\u20ac\ud83d\ude00","a":[false,null,{},[],{"b":true}]})");
    std::string output;
    const JsonByteSink collect = [&](std::span<const std::uint8_t> fragment) {
        output.append(reinterpret_cast<const char*>(fragment.data()), fragment.size());
    };
    const std::string compact = R"({"a":[false,null,{},[],{"b":true}],"z":0.0100,")" "\xc3\xa4" R"(":"\u0000)" "\xe2\x82\xac\xf0\x9f\x98\x80" R"("})";
    auto bytes = reader.render(root, {compact.size()}, collect);
    require(output == compact && bytes == compact.size(), "compact UTF-8 sorts decoded object keys");
    output.clear();
    refuse([&] { reader.render(root, {compact.size() - 1}, collect); }, "json.render.byte_limit");
    require(output.empty(), "an unflushed prefix remains private");

    bytes = reader.render(root, {4096, RenderLayout::Spaced, true}, collect);
    require(output == R"({"a": [false, null, {}, [], {"b": true}], "z": 0.0100, "\u00e4": "\u0000\u20ac\ud83d\ude00"})" &&
            bytes == output.size(), "spaced ASCII escapes BMP and supplemental scalar values");
    output.clear();

    auto subroot = reader.at(*reader.find(root, "a"), 4);
    reader.render(subroot, {1024, RenderLayout::Indented, true, 2}, collect);
    require(output == "{\n      \"b\": true\n    }", "pretty fragment uses its caller-provided indentation");
    reader.close(root.document);
    refuse([&] { reader.render(root, {4096}, collect); }, "json.document");
}

void fragmentsAndCancellation() {
    Reader reader;
    const std::string text = "\"" + std::string(1024 * 1024, 'x') + "\"";
    const auto root = parse(reader, text);
    std::size_t received = 0;
    const JsonByteSink collect = [&](std::span<const std::uint8_t> fragment) {
        const auto expected = std::string_view(text).substr(received, fragment.size());
        const std::string_view actual(reinterpret_cast<const char*>(fragment.data()), fragment.size());
        require(actual == expected && fragment.size() <= 4096, "large value delivers bounded successive fragments");
        received += fragment.size();
    };
    refuse([&] { reader.render(root, {text.size()}, collect, [] { return true; }); }, "json.cancelled");
    require(received == 0, "pre-cancel has no effect");

    refuse([&] { reader.render(root, {text.size()}, collect, [&] { return received != 0; }); }, "json.cancelled");
    require(received == 4096, "post-delivery cancellation retains the exact staged prefix");

    received = 0;
    refuse([&] { reader.render(root, {text.size() - 16}, collect); }, "json.render.byte_limit");
    require(received != 0 && received < text.size(), "bounded failure leaves only earlier fragments");

    received = 0;
    const auto bytes = reader.render(root, {text.size()}, collect);
    require(received == text.size() && bytes == text.size(), "successful rerender has no stale writer state");
    reader.close(root.document);
}

void exactIdentityAndReentry() {
    Reader reader;
    Reader foreign;
    const auto root = parse(reader, "null");
    const auto other = parse(foreign, "null");
    const JsonByteSink rejectUse = [&](std::span<const std::uint8_t>) {
        refuse([&] { reader.close(root.document); }, "json.render.in_use");
        refuse([&] { reader.render(root, {4}, [](std::span<const std::uint8_t>) {}); }, "json.render.in_use");
    };
    refuse([&] { reader.render(root, {0}, rejectUse); }, "json.render.options");
    refuse([&] { reader.render(root, {4}, {}); }, "json.render.options");
    refuse([&] { reader.render(other, {4}, rejectUse); }, "json.document");
    require(reader.render(root, {4}, rejectUse) == 4, "retains source document during delivery");

    try {
        reader.render(root, {4}, [](std::span<const std::uint8_t>) { throw std::runtime_error("sink failure"); });
        throw std::logic_error("expected sink failure");
    } catch (const std::runtime_error& error) {
        require(std::string_view(error.what()) == "sink failure", "propagates sink error");
    }
    reader.close(root.document);
    foreign.close(other.document);
    require(reader.liveDocuments() == 0 && foreign.liveDocuments() == 0, "all render sources released");
}
} // namespace

int main() {
    try {
        formatting();
        fragmentsAndCancellation();
        exactIdentityAndReentry();
        std::cout << "JSON value rendering format, bounds, cancellation and custody passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
