// SPDX-License-Identifier: Apache-2.0
#include <pipelang/workspace/WorkspaceIo.hpp>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace pipelang::workspace;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void require(bool condition, const std::string& name) {
    if (!condition)
        throw std::runtime_error(name);
    ++checks;
}
template<class Operation>
void rejected(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const WorkspaceError& error) {
        require(error.what() == "workspace." + code, "unexpected typed failure");
        return;
    }
    throw std::runtime_error("missing expected rejection");
}
std::vector<std::uint8_t> bytes(const std::string& text) {
    return {text.begin(), text.end()};
}
void externalWrite(const fs::path& path, const std::string& value) {
    std::ofstream file(path, std::ios::binary);
    file << value;
    require(static_cast<bool>(file), "fixture write");
}
class Fixture final {
public:
    fs::path base;
    fs::path home;
    Fixture() {
        char pattern[] = "/tmp/pipelang-workspace-io.XXXXXX";
        char* result = ::mkdtemp(pattern);
        if (!result)
            throw std::runtime_error("fixture root");
        base = result;
        home = base / "home";
        fs::create_directory(home);
        ::chmod(home.c_str(), 0700);
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(base, ignored);
    }
};
void filesAndPaging(Fixture& fixture) {
    FileIo files(fixture.home.string());
    files.makeDirectory("documents/nested");
    files.makeDirectory("documents/nested");
    files.writeNew("documents/a.md", bytes("alpha"));
    auto first = files.read("documents/a.md");
    require(first.bytes == bytes("alpha") && first.hash == Hashing::text("alpha") && !first.identity.empty(), "exact bytes and revision");
    rejected([&] { files.writeNew("documents/a.md", bytes("collision")); }, "already_exists");
    rejected([&] { files.replace("documents/a.md", Hashing::text("stale"), bytes("lost")); }, "stale_revision");
    rejected([&] { files.replace("documents/missing.md", first.hash, bytes("lost")); }, "not_found");
    files.replace("documents/a.md", first.hash, bytes(std::string("\0\xff", 2)));
    require(files.read("documents/a.md").bytes == bytes(std::string("\0\xff", 2)), "binary bytes preserved");
    files.writeNew("documents/max.bin", std::vector<std::uint8_t>(1024 * 1024, 42));
    require(files.read("documents/max.bin").bytes.size() == 1024 * 1024, "maximum file");
    rejected([&] { files.writeNew("oversize", std::vector<std::uint8_t>(1024 * 1024 + 1)); }, "limit");
    externalWrite(fixture.home / "oversize", std::string(1024 * 1024 + 1, 'x'));
    rejected([&] { (void)files.read("oversize"); }, "limit");
    for (const std::string path : {"../escape", "/absolute", "documents/../a", "documents//a", ".git/config", ".GiT/config", "a\\b", "a/./b", "a/"})
        rejected([&] { (void)files.read(path); }, "invalid_path");
    rejected([&] { (void)files.read(std::string("a\0b", 3)); }, "invalid_path");
    fs::create_symlink(fixture.base, fixture.home / "link");
    rejected([&] { files.writeNew("link/escape", bytes("bad")); }, "invalid_path");
    fs::create_symlink(fixture.home / "documents/a.md", fixture.home / "file-link");
    rejected([&] { (void)files.read("file-link"); }, "not_regular");
    fs::create_hard_link(fixture.home / "documents/a.md", fixture.base / "external-link");
    rejected([&] { (void)files.read("documents/a.md"); }, "not_regular");
    fs::remove(fixture.base / "external-link");
    ::mkfifo((fixture.home / "fifo").c_str(), 0600);
    rejected([&] { (void)files.read("fifo"); }, "not_regular");
    files.writeNew("documents/b.md", bytes("beta"));
    DirectoryPager pager(files);
    auto page = pager.list("documents", "", 1);
    require(page.entries.size() == 1 && !page.complete && !page.nextAfter.empty(), "bounded first page");
    auto second = pager.list("documents", page.nextAfter, 1);
    require(second.entries.size() == 1 && second.entries[0].name > page.entries[0].name, "cursor ordering");
    files.writeNew("documents/c.md", bytes("changed"));
    rejected([&] { (void)pager.list("documents", page.nextAfter, 1); }, "stale_cursor");
    rejected([&] { (void)pager.list("documents", "", 151); }, "limit");
    require(files.resolve("documents") == (fixture.home / "documents").string(), "verified local directory");
    rejected([&] { (void)files.resolve("file-link"); }, "invalid_path");
    std::atomic<bool> cancel{true};
    FileIo cancelled(fixture.home.string(), [&] { return cancel.load(); });
    rejected([&] { (void)cancelled.read("documents/a.md"); }, "cancelled");
    FileIo competing(fixture.home.string());
    auto revision = files.read("documents/a.md").hash;
    std::atomic<int> successes{0};
    std::atomic<int> stale{0};
    auto edit = [&](FileIo& selected, const std::string& body) {
        try {
            selected.replace("documents/a.md", revision, bytes(body));
            ++successes;
        } catch (const WorkspaceError& error) {
            if (std::string(error.what()) == "workspace.stale_revision")
                ++stale;
        }
    };
    std::thread left([&] { edit(files, "left"); });
    std::thread right([&] { edit(competing, "right"); });
    left.join();
    right.join();
    require(successes == 1 && stale == 1, "cooperating writers reject stale edit");
    fs::rename(fixture.home, fixture.base / "moved");
    fs::create_directory(fixture.home);
    ::chmod(fixture.home.c_str(), 0700);
    rejected([&] { (void)files.read("documents/a.md"); }, "root_changed");
    fs::remove(fixture.home);
    fs::rename(fixture.base / "moved", fixture.home);
}
void watcher(Fixture& fixture) {
    FileIo files(fixture.home.string());
    files.makeDirectory("watch");
    ChangeFeed feed(files);
    feed.watch("watch");
    feed.watch("watch");
    externalWrite(fixture.home / "watch/editor.tmp", "direct editor save");
    fs::rename(fixture.home / "watch/editor.tmp", fixture.home / "watch/note.md");
    auto changes = feed.changes(256, 100);
    bool observed = false;
    for (const auto& event : changes.events)
        observed = observed || (event.path == "watch/note.md" && event.kind == "created");
    require(observed, "direct editor atomic-save event");
    for (int i = 0; i < 350; ++i)
        externalWrite(fixture.home / ("watch/burst-" + std::to_string(i)), "x");
    auto burst = feed.changes(256, 0);
    require(burst.overflow && burst.events.size() <= 256, "bounded native queue overflow requests rescan");
    rejected([&] { (void)feed.changes(257, 0); }, "limit");
    rejected([&] { (void)feed.changes(1, 1001); }, "limit");
    fs::rename(fixture.home / "watch", fixture.base / "watch-moved-out");
    require(feed.changes(256, 100).overflow, "moved-out watched directory invalidates watch");
    fs::rename(fixture.home, fixture.base / "home-moved");
    fs::create_directory(fixture.home);
    ::chmod(fixture.home.c_str(), 0700);
    rejected([&] { (void)feed.changes(1, 0); }, "root_changed");
    fs::remove(fixture.home);
    fs::rename(fixture.base / "home-moved", fixture.home);
}
void repository(Fixture& fixture) {
    auto checkout = fixture.base / "checkout";
    fs::create_directories(checkout / ".git/objects");
    fs::create_directories(checkout / ".git/refs/heads");
    externalWrite(checkout / ".git/HEAD", "ref: refs/heads/main\n");
    externalWrite(checkout / ".git/config", "[core]\nrepositoryformatversion = 0\nbare = false\n[remote \"origin\"]\nurl = https://fixture-user:fixture-secret@example.invalid/team/repo.git\n");
    RepositoryInspector inspector({{"checkout", checkout.string()}});
    auto signals = inspector.inspect("checkout");
    require(signals.alias == "checkout" && !signals.checkoutIdentity.empty() && !signals.commonIdentity.empty(), "read-only repository signals");
    require(signals.origin == "https://fixture-user:fixture-secret@example.invalid/team/repo.git", "raw origin signal retained for caller redaction");
    rejected([&] { (void)inspector.inspect("not-admitted"); }, "repository_not_admitted");
    RepositoryInspector slow({{"checkout", checkout.string()}}, {}, "/proc/self/exe", std::chrono::milliseconds(50));
    auto begin = std::chrono::steady_clock::now();
    rejected([&] { (void)slow.inspect("checkout"); }, "repository_timeout");
    require(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500), "bounded repository timeout");
    std::atomic<bool> cancel{false};
    RepositoryInspector cancelled({{"checkout", checkout.string()}}, [&] { return cancel.load(); }, "/proc/self/exe");
    std::jthread trigger([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); cancel = true; });
    rejected([&] { (void)cancelled.inspect("checkout"); }, "cancelled");
    int status = 0;
    require(::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "repository children reaped");
}
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--no-pager") {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return 0;
    }
    try {
        require(Hashing::text("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "maintained SHA capability");
        auto id = Uuid::create();
        require(id.size() == 36 && id[14] == '4' && id != Uuid::create(), "OS random UUID");
        Fixture fixture;
        filesAndPaging(fixture);
        watcher(fixture);
        repository(fixture);
        ::chmod(fixture.home.c_str(), 0755);
        rejected([&] { FileIo insecure(fixture.home.string()); }, "insecure_root");
        std::cout << "workspace-io contracts passed checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace-io contract failure: " << error.what() << '\n';
        return 1;
    }
}
