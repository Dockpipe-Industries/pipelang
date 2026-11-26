#include <pipelang/json/DocumentWriter.hpp>

#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace pipelang::json;

void require(bool expected, const char* label) {
    if (!expected)
        throw std::runtime_error(label);
}

template<class Effect>
void refuses(Effect effect, std::string_view expected) {
    try {
        effect();
    } catch (const ReadFailure& error) {
        require(std::string_view(error.what()) == expected, "unexpected structured output refusal");
        return;
    }
    throw std::runtime_error("expected structured output refusal");
}

void settersAndBounds(Reader& reader, const JsonByteSink& sink, std::string& result) {
    DocumentWriter exact(reader, {4}, sink);
    exact.integer(123);
    require(exact.finish() == 4 && result == "123\n", "exact output includes final newline");
    refuses([&] { exact.nullValue(); }, "json.writer.state");

    result.clear();
    DocumentWriter incomplete(reader, {64}, sink);
    incomplete.beginObject();
    incomplete.name("unfinished");
    refuses([&] { incomplete.endObject(); }, "json.writer.container");
    refuses([&] { incomplete.nullValue(); }, "json.writer.state");
    require(result == "{\"unfinished\":", "invalid structure retains only earlier effects");

    result.clear();
    DocumentWriter limited(reader, {4}, sink);
    limited.nullValue();
    refuses([&] { limited.finish(); }, "json.writer.byte_limit");
    require(result == "null", "newline cannot exceed aggregate bound");
    refuses([&] { limited.finish(); }, "json.writer.state");

    result.clear();
    DocumentWriter badNumber(reader, {64}, sink);
    refuses([&] { badNumber.finite(std::numeric_limits<double>::infinity()); }, "json.writer.number");
    require(result.empty(), "invalid number has no output");

    DocumentWriter badText(reader, {64}, sink);
    refuses([&] { badText.text(std::string_view("\xff", 1)); }, "json.writer.utf8");
    require(result.empty(), "malformed UTF-8 has no serialized value");
}

void borrowing(Reader& reader, const JsonByteSink& sink, std::string& output) {
    const auto document = reader.begin({});
    constexpr std::string_view input = "{\"b\":1,\"a\":[true,false]}";
    reader.append(document, std::span(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()));
    const auto source = reader.complete(document);

    DocumentWriter cancelled(reader, {128}, sink);
    refuses([&] { cancelled.borrowed(source, [] { return true; }); }, "json.cancelled");
    require(output.empty(), "pre-cancel produces no prefix");
    refuses([&] { cancelled.finish(); }, "json.writer.state");

    DocumentWriter completed(reader, {128, RenderLayout::Spaced}, sink);
    completed.beginArray();
    completed.borrowed(source);
    completed.endArray();
    require(completed.finish() == output.size() &&
            output == "[{\"a\": [true, false], \"b\": 1}]\n", "borrowed metadata retains typed values");
    reader.close(document);
}

void captures(Reader& reader, const JsonByteSink& sink, std::string& output) {
    output.clear();
    auto value = DocumentWriter::capture(reader, {2, RenderLayout::Compact, false, 1},
        [](DocumentWriter& writer) { writer.integer(42); });
    require(value.size() == 2, "exact captured bytes exclude a document newline");

    DocumentWriter composed(reader, {5}, sink);
    composed.beginArray();
    composed.captured(value);
    composed.endArray();
    require(composed.finish() == 5 && output == "[42]\n", "compose one complete preserved value");

    output.clear();
    DocumentWriter wrongDepth(reader, {64}, sink);
    refuses([&] { wrongDepth.captured(value); }, "json.writer.capture_placement");
    require(output.empty(), "wrong position emits no value");
    refuses([&] { wrongDepth.finish(); }, "json.writer.state");

    DocumentWriter wrongLayout(reader, {64, RenderLayout::Spaced}, sink);
    wrongLayout.beginArray();
    refuses([&] { wrongLayout.captured(value); }, "json.writer.capture_placement");
    require(output == "[", "wrong layout leaves only the already-written prefix");

    auto moved = std::move(value);
    DocumentWriter stale(reader, {64}, sink);
    stale.beginArray();
    refuses([&] { stale.captured(value); }, "json.writer.capture_placement");
    require(moved.size() == 2, "move transfers the complete value");

    refuses([&] {
        (void)DocumentWriter::capture(reader, {32}, [](DocumentWriter& writer) { writer.beginArray(); });
    }, "json.writer.incomplete");
    refuses([&] {
        (void)DocumentWriter::capture(reader, {1}, [](DocumentWriter& writer) { writer.integer(42); });
    }, "json.writer.byte_limit");
    refuses([&] {
        (void)DocumentWriter::capture(reader, {32, RenderLayout::Compact, false, 64},
            [](DocumentWriter& writer) { writer.beginObject(); });
    }, "json.writer.depth");
    refuses([&] {
        DocumentWriter invalid(reader, {32, RenderLayout::Compact, false, 1}, sink);
    }, "json.writer.options");

    bool ran = false;
    refuses([&] {
        (void)DocumentWriter::capture(reader, {32}, [&](DocumentWriter&) { ran = true; }, [] { return true; });
    }, "json.cancelled");
    require(!ran, "pre-cancel avoids capture work");

    auto large = DocumentWriter::capture(reader, {16384},
        [](DocumentWriter& writer) { writer.text(std::string(12000, 'a')); });
    output.clear();
    DocumentWriter cancelled(reader, {16384}, sink);
    refuses([&] { cancelled.captured(large, [&] { return output.size() >= 4096; }); }, "json.cancelled");
    require(output.size() == 4096, "captured transfer cancellation stops at a bounded fragment");
    refuses([&] { cancelled.finish(); }, "json.writer.state");
}

} // namespace

int main() {
    try {
        Reader reader;
        std::string bytes;
        const JsonByteSink sink = [&](std::span<const std::uint8_t> fragment) {
            require(fragment.size() <= 4096, "bounded sink fragment");
            bytes.append(reinterpret_cast<const char*>(fragment.data()), fragment.size());
        };
        settersAndBounds(reader, sink, bytes);
        borrowing(reader, sink, bytes);
        captures(reader, sink, bytes);
        require(reader.liveDocuments() == 0, "borrowed source released");
        std::cout << "Structured JSON native bounds, lifetime and failure contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
