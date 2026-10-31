// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "package.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>

namespace pipelang::tools {
namespace {
constexpr std::size_t memberLimit = 64U << 20;
constexpr std::size_t archiveLimit = 128U << 20;
using Block = std::array<char, 512>;
void field(Block& block, std::size_t offset, std::size_t capacity, const std::string& value) {
    require(value.size() < capacity, "archive field capacity exceeded");
    std::copy(value.begin(), value.end(), block.begin() + static_cast<std::ptrdiff_t>(offset));
}
void octal(Block& block, std::size_t offset, std::size_t capacity, std::uint64_t value) {
    std::array<char, 32> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value, 8);
    require(result.ec == std::errc{}, "cannot encode archive number");
    const std::string text(digits.data(), result.ptr);
    require(text.size() < capacity, "archive number capacity exceeded");
    field(block, offset, capacity, std::string(capacity - 1 - text.size(), '0') + text);
}
unsigned checksum(const Block& block) {
    unsigned result = 0;
    for (std::size_t index = 0; index < block.size(); ++index)
        result += index >= 148 && index < 156 ? 32 : static_cast<unsigned char>(block[index]);
    return result;
}
Block header(const std::string& name, std::size_t bytes, unsigned mode) {
    Block result{};
    if (name.size() < 100)
        field(result, 0, 100, name);
    else {
        const auto slash = name.rfind('/', 155);
        require(slash != std::string::npos && slash < 155 && name.size() - slash - 1 < 100, "archive path exceeds USTAR capacity");
        field(result, 0, 100, name.substr(slash + 1));
        field(result, 345, 155, name.substr(0, slash));
    }
    octal(result, 100, 8, mode);
    octal(result, 108, 8, 0);
    octal(result, 116, 8, 0);
    octal(result, 124, 12, bytes);
    octal(result, 136, 12, 0);
    result[156] = '0';
    field(result, 257, 6, "ustar");
    result[263] = '0';
    result[264] = '0';
    octal(result, 148, 8, checksum(result));
    return result;
}
std::string stringField(const Block& block, std::size_t offset, std::size_t capacity) {
    const auto first = block.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto last = first + static_cast<std::ptrdiff_t>(capacity);
    const auto end = std::find(first, last, '\0');
    require(end != last, "archive field is not terminated");
    require(std::all_of(end, last, [](char value) { return value == 0; }), "archive field has noncanonical trailing bytes");
    return {first, end};
}
std::uint64_t numberField(const Block& block, std::size_t offset, std::size_t capacity) {
    const auto value = stringField(block, offset, capacity);
    require(!value.empty(), "archive number is empty");
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, 8);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(), "invalid archive number");
    return result;
}
std::vector<fs::path> filesUnder(const fs::path& prefix) {
    require(prefix.is_absolute() && fs::weakly_canonical(prefix) == prefix && fs::is_directory(prefix), "package prefix must be an absolute nonsymbolic directory");
    std::vector<fs::path> files;
    std::size_t entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(prefix)) {
        require(++entries <= 8192 && !entry.is_symlink(), "package entry capacity exceeded or symbolic input");
        if (entry.is_directory())
            continue;
        regularFile(entry.path());
        portablePath(entry.path().lexically_relative(prefix).generic_string());
        require(files.size() < 4096, "package file capacity exceeded");
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}
void freshOutput(const fs::path& output) {
    require(output.is_absolute() && fs::weakly_canonical(output) == output && !fs::exists(fs::symlink_status(output)), "package output must be a fresh absolute nonsymbolic path");
}
std::string nativeTarget() {
#if defined(__linux__) && defined(__x86_64__)
    return "linux/amd64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux/arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "darwin/amd64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "darwin/arm64";
#elif defined(_WIN32) && defined(_M_X64)
    return "windows/amd64";
#else
    throw std::runtime_error("native SDK packaging target is unqualified");
#endif
}
}
void packageSdk(const fs::path& prefix, const fs::path& manifest, const std::string& expected, const fs::path& output) {
    verifyManifest(prefix, manifest, expected);
    freshOutput(output);
    require(output.lexically_relative(prefix).empty() || output.lexically_relative(prefix).begin()->string() == "..", "package output must be outside SDK inputs");
    const auto paths = filesUnder(prefix);
    fs::create_directories(output);
    const auto archive = output / "pipelang-native-sdk.tar";
    const auto temporary = output / ".archive.partial";
    std::ofstream stream(temporary, std::ios::binary);
    require(stream.good(), "cannot create package archive");
    Json files = Json::array();
    std::size_t total = 1024;
    const Block zero{};
    for (const auto& path : paths) {
        const auto name = path.lexically_relative(prefix).generic_string();
        const auto bytes = readFile(path, memberLimit);
        const auto padding = (512 - bytes.size() % 512) % 512;
        require(bytes.size() + padding + 512 <= archiveLimit - total, "SDK archive exceeds capacity");
        total += 512 + bytes.size() + padding;
        const auto executable = (fs::status(path).permissions() & fs::perms::owner_exec) != fs::perms::none;
        const unsigned mode = executable ? 0755 : 0644;
        const auto block = header(name, bytes.size(), mode);
        stream.write(block.data(), block.size());
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.write(zero.data(), static_cast<std::streamsize>(padding));
        files.push_back({{"path", name}, {"bytes", bytes.size()}, {"sha256", sha256(bytes)}, {"mode", mode}});
    }
    stream.write(zero.data(), zero.size());
    stream.write(zero.data(), zero.size());
    stream.close();
    require(stream.good(), "failed to write SDK archive");
    verifyManifest(prefix, manifest, expected);
    const auto finalPaths = filesUnder(prefix);
    require(finalPaths == paths, "SDK package input inventory changed during packaging");
    for (const auto& record : files)
        require(fileHash(prefix / record.at("path").get<std::string>()) == record.at("sha256"), "SDK package input bytes changed during packaging");
    fs::rename(temporary, archive);
    const Json package{{"protocol", "pipelang.native-sdk-package.v1"}, {"target", nativeTarget()}, {"format", "ustar"},
        {"qualification", "local-native-sdk-checkpoint"}, {"sdk_manifest", manifest.lexically_relative(prefix).generic_string()},
        {"sdk_manifest_sha256", expected}, {"archive", {{"file", archive.filename().string()}, {"bytes", total}, {"sha256", fileHash(archive)}}}, {"files", files}};
    atomicWrite(output / "sdk-package.json", canonical(package));
}
void extractSdk(const fs::path& manifest, const std::string& expected, const fs::path& output) {
    exactHash(expected);
    const auto metadataBytes = readFile(manifest, 16U << 20);
    require(sha256(metadataBytes) == expected, "SDK package metadata checksum differs");
    const auto metadata = parseJson(metadataBytes);
    exactObject(metadata, {"protocol", "target", "format", "qualification", "sdk_manifest", "sdk_manifest_sha256", "archive", "files"}, {}, "SDK package");
    require(metadata.at("protocol") == "pipelang.native-sdk-package.v1" && metadata.at("format") == "ustar", "unsupported SDK package format");
    require(metadata.at("target") == nativeTarget() && metadata.at("qualification") == "local-native-sdk-checkpoint", "SDK package target or qualification mismatch");
    const auto manifestName = text(metadata.at("sdk_manifest"), "SDK manifest path");
    portablePath(manifestName);
    const auto sdkHash = text(metadata.at("sdk_manifest_sha256"), "SDK manifest hash");
    exactHash(sdkHash);
    const auto& archiveRecord = metadata.at("archive");
    exactObject(archiveRecord, {"file", "bytes", "sha256"}, {}, "archive");
    require(archiveRecord.at("file") == "pipelang-native-sdk.tar", "unexpected SDK archive name");
    const auto archive = manifest.parent_path() / "pipelang-native-sdk.tar";
    regularFile(archive);
    require(archiveRecord.at("bytes").is_number_unsigned() && archiveRecord.at("bytes").get<std::uint64_t>() <= archiveLimit &&
        fs::file_size(archive) == archiveRecord.at("bytes").get<std::uint64_t>(), "SDK archive size differs");
    exactHash(text(archiveRecord.at("sha256"), "archive hash"));
    require(fileHash(archive) == archiveRecord.at("sha256"), "SDK archive checksum differs");
    const auto& records = metadata.at("files");
    require(records.is_array() && !records.empty() && records.size() <= 4096, "invalid SDK package file inventory");
    std::map<std::string, Json> files;
    for (const auto& record : records) {
        exactObject(record, {"path", "bytes", "sha256", "mode"}, {}, "package file");
        const auto name = text(record.at("path"), "package path");
        portablePath(name);
        require(files.emplace(name, record).second, "duplicate SDK package path");
        require(record.at("bytes").is_number_unsigned() && record.at("bytes").get<std::uint64_t>() <= memberLimit, "package member size exceeds capacity");
        require(record.at("mode") == 0644 || record.at("mode") == 0755, "unsupported SDK package member permissions");
        exactHash(text(record.at("sha256"), "package file hash"));
    }
    require(files.contains(manifestName) && files.at(manifestName).at("sha256") == sdkHash, "package omits its exact SDK manifest");
    freshOutput(output);
    fs::create_directories(output.parent_path());
    const auto stage = output.parent_path() / (".pipelang-extract-" + uniqueName());
    fs::create_directory(stage);
    try {
        std::ifstream stream(archive, std::ios::binary);
        require(stream.good(), "cannot open SDK archive");
        std::set<std::string> seen;
        while (true) {
            Block block{};
            stream.read(block.data(), block.size());
            require(stream.gcount() == static_cast<std::streamsize>(block.size()), "truncated SDK archive header");
            if (block == Block{}) {
                stream.read(block.data(), block.size());
                require(stream.gcount() == static_cast<std::streamsize>(block.size()) && block == Block{} && stream.peek() == std::char_traits<char>::eof(), "invalid SDK archive end");
                break;
            }
            require(numberField(block, 148, 8) == checksum(block), "SDK archive header checksum differs");
            require(stringField(block, 257, 6) == "ustar" && block[263] == '0' && block[264] == '0' && block[156] == '0', "SDK archive is not a regular USTAR member");
            auto name = stringField(block, 0, 100);
            const auto prefix = stringField(block, 345, 155);
            if (!prefix.empty())
                name = prefix + '/' + name;
            portablePath(name);
            require(files.contains(name) && seen.insert(name).second, "unlisted or duplicate SDK archive member");
            const auto& record = files.at(name);
            const auto count = numberField(block, 124, 12);
            require(count == record.at("bytes").get<std::uint64_t>(), "SDK archive member size differs");
            const auto mode = record.at("mode").get<unsigned>();
            require(numberField(block, 100, 8) == mode && block == header(name, static_cast<std::size_t>(count), mode), "SDK archive header is not canonical");
            std::string bytes(static_cast<std::size_t>(count), '\0');
            stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            require(stream.gcount() == static_cast<std::streamsize>(bytes.size()) && sha256(bytes) == record.at("sha256"), "SDK archive member checksum differs");
            const auto padding = (512 - bytes.size() % 512) % 512;
            Block pad{};
            stream.read(pad.data(), static_cast<std::streamsize>(padding));
            require(stream.gcount() == static_cast<std::streamsize>(padding) && pad == Block{}, "invalid SDK archive padding");
            atomicWrite(stage / name, bytes);
            fs::permissions(stage / name, static_cast<fs::perms>(mode), fs::perm_options::replace);
        }
        require(seen.size() == files.size(), "SDK archive is missing listed members");
        verifyManifest(stage, stage / manifestName, sdkHash);
        require(fileHash(archive) == archiveRecord.at("sha256") && fileHash(manifest) == expected, "SDK package changed during extraction");
        freshOutput(output);
        fs::rename(stage, output);
    } catch (...) {
        fs::remove_all(stage);
        throw;
    }
}
}
