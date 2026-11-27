// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/sdk-tools/package.hpp"
#include <iostream>
#include "windows_junction.hpp"

namespace {
using namespace pipelang::tools;
Json cases = Json::array();
template<class Action>
void refuses(const std::string& name, Action action) {
    bool refused = false;
    try {
        action();
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "package oracle expected refusal: " + name);
    cases.push_back({{"id", "package/" + name}, {"outcome", "refused"}});
}
void test(const fs::path& output) {
    const auto sdk = output / "sdk";
    atomicWrite(sdk / "bin/tool", std::string("native\0binary", 13));
    fs::permissions(sdk / "bin/tool", fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);
    atomicWrite(sdk / "include/empty.hpp", "");
    atomicWrite(sdk / "share/license", "fixture license\n");
    const auto sdkManifest = sdk / "share/sdk-manifest.json";
    recordManifest(sdk, sdkManifest, Json::array({"bin", "include", "share"}));
    const auto sdkHash = fileHash(sdkManifest);
    const auto first = output / "first";
    const auto second = output / "second";
    packageSdk(sdk, sdkManifest, sdkHash, first);
    packageSdk(sdk, sdkManifest, sdkHash, second);
    const auto archive = first / "pipelang-native-sdk.tar";
    const auto manifest = first / "sdk-package.json";
    require(fileHash(archive) == fileHash(second / archive.filename()) && fileHash(manifest) == fileHash(second / manifest.filename()), "native SDK packages are not deterministic");
    const auto original = readFile(archive, 1U << 20);
    const auto metadata = parseJson(readFile(manifest, 1U << 20));
    const auto installed = output / "installed";
    extractSdk(manifest, fileHash(manifest), installed);
    verifyManifest(installed, installed / "share/sdk-manifest.json", sdkHash);
    require(readFile(installed / "bin/tool", 128) == std::string("native\0binary", 13) && fs::file_size(installed / "include/empty.hpp") == 0, "package round trip changed binary/empty files");
    require((fs::status(installed / "bin/tool").permissions() & fs::perms::owner_exec) != fs::perms::none, "package lost executable mode");
    cases.push_back({{"id", "package/deterministic-round-trip"}, {"outcome", "passed"}});
    refuses("existing-extraction-output", [&] { extractSdk(manifest, fileHash(manifest), installed); });
    require(readFile(installed / "bin/tool", 128) == std::string("native\0binary", 13), "refused extraction changed existing output");
    refuses("existing-package-output", [&] { packageSdk(sdk, sdkManifest, sdkHash, first); });
    refuses("wrong-metadata-pin", [&] { extractSdk(manifest, std::string(64, '0'), output / "wrong-pin"); });
    const auto badManifest = first / "bad-package.json";
    const auto changed = [&](const std::string& name, const Json& value) {
        atomicWrite(badManifest, canonical(value));
        refuses(name, [&] { extractSdk(badManifest, fileHash(badManifest), output / ("refused-" + name)); });
        require(!fs::exists(output / ("refused-" + name)), "refused extraction published output");
    };
    auto bad = metadata;
    bad["files"].push_back(bad["files"].front());
    changed("duplicate-member", bad);
    bad = metadata;
    bad["files"][0]["path"] = "../escape";
    changed("path-traversal", bad);
    bad = metadata;
    bad["files"][0]["mode"] = 04755;
    changed("special-permissions", bad);
    bad = metadata;
    bad["files"][0]["bytes"] = (64U << 20) + 1;
    changed("member-capacity", bad);
    bad = metadata;
    bad["files"][0]["sha256"] = std::string(64, '0');
    changed("member-checksum", bad);
    bad = metadata;
    bad["target"] = "unqualified/target";
    changed("target-identity", bad);
    bad = metadata;
    bad["format"] = "zip";
    changed("archive-format", bad);
    bad = metadata;
    bad["archive"]["bytes"] = (128U << 20) + 1;
    changed("archive-capacity", bad);
    bad = metadata;
    bad["archive"]["sha256"] = std::string(64, '0');
    changed("archive-checksum", bad);
    auto damaged = original;
    damaged[0] = damaged[0] == 'x' ? 'y' : 'x';
    atomicWrite(archive, damaged);
    bad = metadata;
    bad["archive"]["sha256"] = sha256(damaged);
    changed("header-checksum", bad);
    atomicWrite(archive, original + std::string(512, '\0'));
    bad = metadata;
    bad["archive"]["bytes"] = original.size() + 512;
    bad["archive"]["sha256"] = fileHash(archive);
    changed("trailing-data", bad);
    atomicWrite(archive, original);
#ifdef _WIN32
    const auto junctionTarget = output / "junction-target";
    atomicWrite(junctionTarget / "protected", "outside package roots");
    pipelang::tests::DirectoryJunction junction(sdk / "bin/alias", junctionTarget);
#else
    fs::create_symlink(sdk / "bin/tool", sdk / "bin/alias");
#endif
    refuses("symbolic-input", [&] { packageSdk(sdk, sdkManifest, sdkHash, output / "symbolic"); });
#ifdef _WIN32
    junction.remove();
    require(readFile(junctionTarget / "protected", 128) == "outside package roots", "junction fixture changed its target");
#else
    fs::remove(sdk / "bin/alias");
#endif
    refuses("output-within-sdk", [&] { packageSdk(sdk, sdkManifest, sdkHash, sdk / "package"); });
    atomicWrite(sdk / "bin/tool", "altered SDK executable");
    refuses("altered-sdk", [&] { packageSdk(sdk, sdkManifest, sdkHash, output / "altered"); });
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"cases", cases},
        {"archive_format", "deterministic-ustar"}, {"native_package_round_trip", true}}));
}
}
int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        require(argc == 2, "package tests require a fresh absolute proof directory");
        const fs::path output = argv[1];
        require(output.is_absolute() && !fs::exists(output), "package proof must be fresh and absolute");
        fs::create_directories(output);
        test(output);
        std::cout << cases.size() << " C++ package cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "package oracle: " << error.what() << '\n';
        return 1;
    }
}
