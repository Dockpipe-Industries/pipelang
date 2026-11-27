// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ApplicationLink.hpp"
#include "../../sdk-tools/manifest.hpp"

#include <array>
#include <fstream>
#include <span>
#include <stdexcept>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif

namespace pipelang::models::native_ir {
namespace {

using namespace tools;

struct LinkInputs {
    std::span<const char* const> startup;
    std::span<const char* const> libraries;
    std::span<const char* const> termination;
    std::string_view directory;
};

constexpr std::array linuxStartup{"crt1.o", "crti.o", "crtbeginT.o"};
constexpr std::array linuxLibraries{"libpipelang_native_app_runtime.a", "libstdc++.a", "libgcc.a", "libgcc_eh.a", "libc.a"};
constexpr std::array linuxTermination{"crtend.o", "crtn.o"};
constexpr std::array windowsStartup{"crt2.o", "crtbegin.o"};
constexpr auto windowsLibraries(const char* builtins)
{
    return std::array{"libpipelang_native_app_runtime.a", "libc++.a", "libmingw32.a", builtins,
                      "libunwind.a", "libmoldname.a", "libmingwex.a", "libmsvcrt.a", "libpthread.a",
                      "libadvapi32.a", "libshell32.a", "libuser32.a", "libkernel32.a"};
}
constexpr auto windowsX64Libraries = windowsLibraries("libclang_rt.builtins-x86_64.a");
constexpr auto windowsArm64Libraries = windowsLibraries("libclang_rt.builtins-aarch64.a");
constexpr std::array windowsTermination{"crtend.o"};
constexpr std::array macLibraries{"libpipelang_native_app_runtime.a"};

LinkInputs inputsFor(const Target& target)
{
    if (target.format == ObjectFormat::Coff) {
        const auto& libraries = target.architecture == Architecture::Arm64 ? windowsArm64Libraries : windowsX64Libraries;
        return {windowsStartup, libraries, windowsTermination, "link"};
    }
    if (target.format == ObjectFormat::MachO)
        return {{}, macLibraries, {}, ""};
    return {linuxStartup, linuxLibraries, linuxTermination, ""};
}

class LinkStage {
public:
    explicit LinkStage(fs::path location) : path(std::move(location))
    {
        require(fs::create_directory(path), "cannot reserve native publication path: " + path.string());
    }

    ~LinkStage()
    {
        if (preserved_)
            return;
        std::error_code error;
        fs::remove_all(path, error);
    }

    LinkStage(const LinkStage&) = delete;
    LinkStage& operator=(const LinkStage&) = delete;
    void preserve() noexcept { preserved_ = true; }
    const fs::path path;

private:
    bool preserved_ = false;
};

class LinkDirectory {
public:
    explicit LinkDirectory(const fs::path& directory) : previous_(fs::current_path())
    {
        fs::current_path(directory);
    }

    ~LinkDirectory()
    {
        std::error_code error;
        fs::current_path(previous_, error);
    }

    LinkDirectory(const LinkDirectory&) = delete;
    LinkDirectory& operator=(const LinkDirectory&) = delete;

private:
    fs::path previous_;
};

fs::path publicationPath(const fs::path& output, bool directory = false)
{
    const auto path = fs::absolute(output).lexically_normal();
    require(!path.filename().empty() && fs::is_directory(path.parent_path()),
            "native output requires an existing parent directory");
    const auto destination = fs::canonical(path.parent_path()) / path.filename();
    if (fs::exists(destination) || symbolicPath(destination)) {
        if (directory)
            require(!symbolicPath(destination) && fs::is_directory(destination),
                    "native symbols must replace an ordinary directory: " + destination.string());
        else
            regularFile(destination);
    }
    return destination;
}

bool matchesGeneratedDsymMember(const fs::directory_entry& member, const fs::path& bundle,
                                const fs::path& generated)
{
    if (generated.empty() || symbolicPath(member.path()))
        return false;
    const auto counterpart = generated / member.path().lexically_relative(bundle);
    return !symbolicPath(counterpart) &&
        ((member.is_directory() && fs::is_directory(counterpart)) ||
         (member.is_regular_file() && fs::is_regular_file(counterpart)));
}

void validateDsymDestination(const fs::path& bundle, const fs::path& executableName,
                             const fs::path& generated = {})
{
    if (!fs::exists(bundle) && !symbolicPath(bundle))
        return;

    const auto contents = bundle / "Contents";
    const auto resources = contents / "Resources";
    const auto dwarf = resources / "DWARF";
    for (const auto& directory : {bundle, contents, resources, dwarf}) {
        if (fs::exists(directory) || symbolicPath(directory))
            require(!symbolicPath(directory) && fs::is_directory(directory),
                    "native dSYM requires an ordinary directory: " + directory.string());
    }

    const auto propertyList = contents / "Info.plist";
    const auto debugFile = dwarf / executableName;
    bool hasSymbols = false;
    for (const auto& member : {propertyList, debugFile}) {
        if (fs::exists(member) || symbolicPath(member)) {
            regularFile(member);
            hasSymbols = true;
        }
    }
    // The initial check has no staged bundle for comparison. Recheck all
    // existing members while holding the publication reservations.
    if (hasSymbols && generated.empty())
        return;

    for (const auto& entry : fs::recursive_directory_iterator(bundle)) {
        const auto& path = entry.path();
        require(path == contents || path == resources || path == dwarf ||
                path == propertyList || path == debugFile || matchesGeneratedDsymMember(entry, bundle, generated),
                "native dSYM contains an unexpected file: " + path.string());
    }
}

bool matchesMachOHeader(std::string_view bytes, const Target& target, std::string_view fileType)
{
    const auto cpu = target.architecture == Architecture::Arm64 ?
        std::string_view("\x0c\0\0\x01", 4) : std::string_view("\x07\0\0\x01", 4);
    return bytes.starts_with("\xcf\xfa\xed\xfe") && bytes.substr(4, 4) == cpu &&
           bytes.substr(12, 4) == fileType;
}

void checkExecutable(const fs::path& executable, const Target& target)
{
    regularFile(executable);
    require(fs::file_size(executable) <= (64U << 20), "native executable exceeds its size limit");
    std::array<char, 64> header{};
    std::ifstream stream(executable, std::ios::binary);
    require(static_cast<bool>(stream.read(header.data(), header.size())),
            "native linker produced a truncated executable");
    const std::string_view bytes(header.data(), header.size());
    if (target.format == ObjectFormat::Coff) {
        require(bytes.starts_with("MZ"), "native linker did not produce a Windows executable");
        std::uint32_t offset = 0;
        for (unsigned index = 0; index < 4; ++index)
            offset |= static_cast<std::uint32_t>(static_cast<unsigned char>(header[60 + index])) << (8 * index);
        std::array<char, 26> pe{};
        stream.seekg(offset);
        const auto machine = target.architecture == Architecture::Arm64 ?
            std::string_view("PE\0\0\x64\xaa", 6) : std::string_view("PE\0\0\x64\x86", 6);
        require(static_cast<bool>(stream.read(pe.data(), pe.size())) &&
                std::string_view(pe.data(), 6) == machine &&
                (static_cast<unsigned char>(pe[22]) & 2) != 0 &&
                std::string_view(pe.data() + 24, 2) == std::string_view("\x0b\x02", 2),
                "native linker did not produce a PE32+ executable for " + std::string(target.name));
    } else if (target.format == ObjectFormat::MachO) {
        require(matchesMachOHeader(bytes, target, std::string_view("\x02\0\0\0", 4)),
                "native linker did not produce a Mach-O executable for " + std::string(target.name));
    } else {
        const auto machine = target.architecture == Architecture::Arm64 ?
            std::string_view("\xb7\x00", 2) : std::string_view("\x3e\x00", 2);
        require(bytes.starts_with("\x7f" "ELF\x02\x01\x01") &&
                bytes.substr(16, 2) == std::string_view("\x02\x00", 2) && bytes.substr(18, 2) == machine,
                "native linker did not produce an ELF64 executable for " + std::string(target.name));
    }
}

void publishWithSymbols(const fs::path& executable, const fs::path& output,
                        const fs::path& symbols, const fs::path& symbolOutput, bool directory)
{
    // The reservations serialize publishers of either member of the pair.
    auto executableLock = output;
    executableLock += ".pipelang-publish";
    auto symbolLock = symbolOutput;
    symbolLock += ".pipelang-publish";
    const LinkStage executableReservation(std::move(executableLock));
    LinkStage symbolReservation(std::move(symbolLock));
    if (directory)
        validateDsymDestination(symbolOutput, output.filename(), symbols);
    const auto previousSymbols = symbolReservation.path / "previous";
    const auto newSymbols = symbolReservation.path / "new";
    const bool replacingSymbols = fs::exists(symbolOutput);
    if (directory) {
        fs::rename(symbols, newSymbols);
        if (replacingSymbols)
            fs::rename(symbolOutput, previousSymbols);
    } else {
        fs::copy_file(symbols, newSymbols);
        if (replacingSymbols) {
            regularFile(symbolOutput);
            fs::copy_file(symbolOutput, previousSymbols);
        }
    }
    bool publishedSymbols = false;
    try {
        fs::rename(newSymbols, symbolOutput);
        publishedSymbols = true;
        fs::rename(executable, output);
    } catch (...) {
        if (!directory && !publishedSymbols)
            throw;
        std::error_code rollback;
        if (directory && publishedSymbols)
            fs::rename(symbolOutput, newSymbols, rollback);
        if (!rollback && replacingSymbols)
            fs::rename(previousSymbols, symbolOutput, rollback);
        else if (!directory && publishedSymbols)
            fs::remove(symbolOutput, rollback);
        if (rollback) {
            symbolReservation.preserve();
            throw std::runtime_error("could not restore symbol publication at " + symbolOutput.string() +
                "; staged symbols remain at " + symbolReservation.path.string() + ": " + rollback.message());
        }
        throw;
    }
}

void normalizeObjectTime(const fs::path& path)
{
    constexpr std::int64_t objectTime = 946684800;
#ifdef _WIN32
    __utimbuf64 time{objectTime, objectTime};
    const auto result = _wutime64(path.c_str(), &time);
#else
    const utimbuf time{objectTime, objectTime};
    const auto result = utime(path.c_str(), &time);
#endif
    require(result == 0, "cannot normalize Mach-O debug map input time: " + path.string());
}

}

ApplicationLink::ApplicationLink(const Target& target, const fs::path& sdk, std::string digest,
                                 const fs::path& linker, const fs::path& output, const fs::path& symbols,
                                 MacApplicationTools mac)
    : target_(target), digest_(std::move(digest)), output_(publicationPath(output)), mac_(std::move(mac))
{
    require(!target.runtimeTriple.empty(), "--executable is unavailable for " + std::string(target.name));
    require(sdk.is_absolute() && linker.is_absolute(),
            "--link-sdk and --linker must be absolute paths");
    if (target_.format == ObjectFormat::Coff) {
        require(output_.extension() == ".exe", "Windows executable output must use .exe");
        symbols_ = publicationPath(symbols.empty() ? fs::path(output_).replace_extension(".pdb") : symbols);
        require(symbols_.extension() == ".pdb", "--pdb output must use .pdb");
    } else {
        require(symbols.empty(), "--pdb requires Windows executable output");
    }
    if (target_.format == ObjectFormat::MachO) {
        require(mac_.sysroot.is_absolute() && mac_.dsymutil.is_absolute(),
                "macOS requires absolute --macos-sysroot and --dsymutil paths");
        require(!mac_.sdkVersion.empty() && mac_.sdkVersion.size() <= 24 &&
                mac_.sdkVersion.find_first_not_of("0123456789.") == std::string::npos &&
                mac_.sdkVersion.front() != '.' && mac_.sdkVersion.back() != '.' &&
                mac_.sdkVersion.find("..") == std::string::npos,
                "macOS requires a numeric --macos-sdk-version such as 15.5");
        mac_.sysroot = fs::canonical(mac_.sysroot);
        regularFile(fs::canonical(mac_.sysroot / "usr/lib/libSystem.tbd"));
        regularFile(fs::canonical(mac_.sysroot / "usr/lib/libc++.tbd"));
        mac_.dsymutil = nativeExecutable(mac_.dsymutil);
        regularFile(fs::canonical(mac_.dsymutil));
        auto bundle = output_;
        bundle += ".dSYM";
        symbols_ = publicationPath(bundle, true);
        validateDsymDestination(symbols_, output_.filename());
    } else {
        require(mac_.sysroot.empty() && mac_.sdkVersion.empty() && mac_.dsymutil.empty(),
                "--macos-sysroot, --macos-sdk-version and --dsymutil require a macOS target");
    }
    sdk_ = fs::canonical(sdk);
    // LLVM selects its linker flavor using the invoked filename.
    linker_ = nativeExecutable(linker);
    require(fs::is_directory(sdk_), "--link-sdk must identify a directory");
    regularFile(fs::canonical(linker_));
    hasLinuxMath_ = verifySdk();
}

bool ApplicationLink::verifySdk() const
{
    const auto manifest = verifyManifest(sdk_, sdk_ / "link-sdk.json", digest_);
    std::set<std::string> files;
    for (const auto& entry : manifest.at("files"))
        files.insert(entry.at("path").get<std::string>());
    const auto inputs = inputsFor(target_);
    for (const auto group : {inputs.startup, inputs.libraries, inputs.termination}) {
        for (const auto* name : group) {
            const auto path = (fs::path(inputs.directory) / name).generic_string();
            require(files.contains(path), "native link SDK manifest omits " + path);
        }
    }
    if (target_.format == ObjectFormat::Elf) {
        std::array<char, 64> header{};
        std::ifstream startup(sdk_ / linuxStartup.front(), std::ios::binary);
        require(static_cast<bool>(startup.read(header.data(), header.size())) &&
                matchesObjectTarget(std::string_view(header.data(), header.size()), target_),
                "native link SDK crt1.o does not match " + std::string(target_.name));
    }
    return target_.format == ObjectFormat::Elf && files.contains("libm.a");
}

void ApplicationLink::publish(std::string_view object, const std::function<void()>& verifySources) const
{
    const LinkStage stage(output_.parent_path() / (".pipelang-link-" + uniqueName()));
    const bool windows = target_.format == ObjectFormat::Coff;
    const bool mac = target_.format == ObjectFormat::MachO;
    const auto objectName = windows ? "app.obj" : "app.o";
    std::string executableName = windows ? "app.exe" : "app";
    if (mac)
        executableName = (fs::path("output") / output_.filename()).generic_string();
    const auto executable = stage.path / executableName;
    const auto objectPath = stage.path / objectName;
    atomicWrite(objectPath, object);

    std::vector<std::string> command{linker_.string()};
    const auto inputs = inputsFor(target_);
    fs::path sdkInputs = sdk_ / inputs.directory;
    if (windows) {
        const auto symbolName = symbols_.filename().string();
        const auto machine = target_.architecture == Architecture::Arm64 ? "arm64pe" : "i386pep";
        command.insert(command.end(), {"-m", machine, "-Bstatic", "--nxcompat", "--dynamicbase", "--no-insert-timestamp",
                                       "--pdb=" + symbolName, "--Xlink=/pdbaltpath:" + symbolName,
                                       "--Xlink=/pdbsourcepath:C:/pipelang-source"});
        // Relative paths keep the PDB independent of the build and SDK locations.
        fs::create_directory(stage.path / "sdk");
        for (const auto group : {inputs.startup, inputs.libraries, inputs.termination}) {
            for (const auto* name : group) {
                const auto from = sdkInputs / name;
                const auto to = stage.path / "sdk" / name;
                std::error_code error;
                fs::create_hard_link(from, to, error);
                if (error)
                    fs::copy_file(from, to);
            }
        }
        sdkInputs = "sdk";
    } else if (mac) {
        fs::create_directory(executable.parent_path());
        fs::create_directory(stage.path / "sdk");
        const auto archive = stage.path / "sdk" / macLibraries.front();
        fs::copy_file(sdkInputs / macLibraries.front(), archive);
        normalizeObjectTime(objectPath);
        normalizeObjectTime(archive);
        sdkInputs = archive.parent_path();
        const auto architecture = target_.architecture == Architecture::Arm64 ? "arm64" : "x86_64";
        command.insert(command.end(), {"-arch", architecture, "-platform_version", "macos", "11.0", mac_.sdkVersion,
                                       "-syslibroot", mac_.sysroot.string(), "-oso_prefix", stage.path.string() + "/"});
    } else {
        const auto machine = target_.architecture == Architecture::Arm64 ? "aarch64linux" : "elf_x86_64";
        command.insert(command.end(), {"-m", machine, "-static", "-z", "noexecstack", "-z", "relro"});
    }

    command.insert(command.end(), {"-o", executableName});
    const auto appendInputs = [&](auto group) {
        for (const auto* name : group)
            command.push_back((sdkInputs / name).generic_string());
    };
    appendInputs(inputs.startup);
    if (mac) {
        command.push_back(objectPath.string());
        appendInputs(inputs.libraries);
        command.insert(command.end(), {"-lc++", "-lSystem"});
    } else {
        command.push_back(objectName);
        command.push_back("--start-group");
        appendInputs(inputs.libraries);
        if (hasLinuxMath_)
            command.push_back((sdkInputs / "libm.a").generic_string());
        command.push_back("--end-group");
    }
    appendInputs(inputs.termination);

    const auto result = [&] {
        // This command-line driver owns the process cwd. Restore it before
        // verifying relative source paths or publishing outputs.
        const LinkDirectory directory(stage.path);
        return process(command);
    }();
    if (result.exit != 0) {
        auto message = "native executable link failed for " + std::string(target_.name) +
            " (exit " + std::to_string(result.exit) + ")";
        if (!result.diagnostic.empty())
            message += ": " + result.diagnostic;
        throw std::runtime_error(message);
    }
    require(result.diagnostic.empty(), "native linker reported a diagnostic: " + result.diagnostic);
    checkExecutable(executable, target_);
    const auto dsym = stage.path / "symbols.dSYM";
    if (mac) {
        const auto symbols = process({mac_.dsymutil.string(), "--oso-prepend-path", stage.path.string(),
                                      executable.string(), "--out", dsym.string()});
        require(symbols.exit == 0 && symbols.diagnostic.empty(),
                "native dSYM generation failed (exit " + std::to_string(symbols.exit) + "): " + symbols.diagnostic);
        const auto dwarf = dsym / "Contents/Resources/DWARF" / output_.filename();
        regularFile(dsym / "Contents/Info.plist");
        regularFile(dwarf);
        require(fs::file_size(dwarf) <= (64U << 20), "native DWARF companion exceeds its size limit");
        std::array<char, 16> header{};
        std::ifstream stream(dwarf, std::ios::binary);
        require(static_cast<bool>(stream.read(header.data(), header.size())) &&
                matchesMachOHeader(std::string_view(header.data(), header.size()), target_,
                                   std::string_view("\x0a\0\0\0", 4)),
                "native linker did not produce a Mach-O dSYM for " + std::string(target_.name));
    }
    verifySdk();
    verifySources();
    if (windows) {
        const auto symbols = stage.path / symbols_.filename();
        regularFile(symbols);
        require(fs::file_size(symbols) <= (64U << 20), "native PDB exceeds its size limit");
        std::array<char, 32> signature{};
        std::ifstream stream(symbols, std::ios::binary);
        require(static_cast<bool>(stream.read(signature.data(), signature.size())) &&
                std::string_view(signature.data(), signature.size()) ==
                    std::string_view("Microsoft C/C++ MSF 7.00\r\n\x1a" "DS\0\0\0", 32),
                "native linker did not produce a valid PDB header");
        stream.close();
        publishWithSymbols(executable, output_, symbols, symbols_, false);
    } else if (mac) {
        publishWithSymbols(executable, output_, dsym, symbols_, true);
    } else {
        fs::rename(executable, output_);
    }
}

}
