// The self-update (core/self_update.hpp) without network nor game data:
// SHA-256, DEFLATE, zip and tar.gz, the choice of what is installed, and the
// whole install in a scratch folder, with its rollback.
// The archives are made by tests/make_update_fixtures.py.

#include "core/archive.hpp"
#include "core/news.hpp"
#include "core/self_update.hpp"
#include "core/sha256.hpp"
#include "update_fixtures.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace crusader;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// The same text as the script's.
std::string exe_text(const std::string& tag) {
    std::string s = tag + "\n";
    for (int i = 0; i < 120; ++i) s += "line " + std::to_string(i * i) + " of the new executable\n";
    return s;
}

std::string bytes_text(const std::vector<std::uint8_t>& v) { return std::string(v.begin(), v.end()); }

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

template <std::size_t N>
std::vector<std::uint8_t> vec(const std::uint8_t (&a)[N]) {
    return std::vector<std::uint8_t>(a, a + N);
}

std::map<std::string, ArchiveEntry> entries_of(const std::vector<std::uint8_t>& archive, ArchiveFormat format,
                                               std::string* error = nullptr) {
    std::map<std::string, ArchiveEntry> out;
    const ArchiveResult result = read_archive(archive, format, [&](ArchiveEntry& e) {
        out[e.path] = e;
        return true;
    });
    if (error) *error = result.ok ? "" : result.error;
    return out;
}

void check_sha256() {
    check(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256: empty");
    check(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256: abc");
    const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    check(sha256_hex(two_blocks.data(), two_blocks.size()) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256: two blocks");
    // A million 'a', fed in odd pieces.
    Sha256 hash;
    const std::string piece(997, 'a');
    std::size_t left = 1000000;
    while (left > 0) {
        const std::size_t n = std::min(left, piece.size());
        hash.update(piece.data(), n);
        left -= n;
    }
    check(hash.finish_hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256: a million a, in pieces");
}

void check_inflate() {
    std::string text;
    for (int i = 0; i < 400; ++i) {
        text += std::to_string(i) + ":";
        for (int k = 0; k < i % 7; ++k) text += "abc";
        text += "\n";
    }
    const auto out = inflate_raw(kDeflated, sizeof kDeflated, 1 << 20);
    check(out && bytes_text(*out) == text, "inflate: dynamic Huffman");
    const auto fixed = inflate_raw(kDeflatedFixed, sizeof kDeflatedFixed, 1000);
    check(fixed && bytes_text(*fixed) == "hello hello hello hello", "inflate: fixed Huffman, a copy over itself");
    const auto stored = inflate_raw(kDeflatedStored, sizeof kDeflatedStored, 1000);
    check(stored && bytes_text(*stored) == "stored block", "inflate: stored block");
    check(!inflate_raw(kDeflated, sizeof kDeflated, text.size() - 1), "inflate: bounded output");
    check(!inflate_raw(kDeflated, sizeof kDeflated / 2, 1 << 20), "inflate: cut stream");
    std::vector<std::uint8_t> broken = vec(kDeflated);
    for (std::size_t i = 4; i < broken.size(); i += 7) broken[i] ^= 0x5A;
    (void)inflate_raw(broken.data(), broken.size(), 1 << 20);  // must not crash, whatever it returns
    check(!inflate_raw(nullptr, 0, 100), "inflate: nothing");
    check(crc32(reinterpret_cast<const std::uint8_t*>("123456789"), 9) == 0xCBF43926u, "crc32");
}

void check_zip() {
    std::string error;
    auto zip = entries_of(vec(kZip), ArchiveFormat::kZip, &error);
    check(error.empty() && zip.size() == 7, "zip: entries");
    check(zip.count("pkg/") && zip["pkg/"].directory, "zip: a folder");
    check(zip["pkg/README.md"].data.size() == 9 && bytes_text(zip["pkg/README.md"].data) == "readme 2\n", "zip: deflated entry");
    check(bytes_text(zip["pkg/crusader-no-remorse.exe"].data) == exe_text("NEWEXE"), "zip: the executable");
    check(bytes_text(zip["pkg/data/PUT_GAME_DATA_HERE.txt"].data) == "put the data here\n", "zip: stored entry");
    check(zip["pkg/crusader-no-remorse.exe"].executable && !zip["pkg/README.md"].executable, "zip: modes");

    std::vector<std::uint8_t> corrupt = vec(kZip);
    corrupt[corrupt.size() / 3] ^= 0xFF;  // inside an entry
    check(!read_archive(corrupt, ArchiveFormat::kZip, [](ArchiveEntry&) { return true; }).ok, "zip: a corrupt entry is refused");
    check(!read_archive({}, ArchiveFormat::kZip, [](ArchiveEntry&) { return true; }).ok, "zip: empty");
    std::vector<std::uint8_t> cut(vec(kZip));
    cut.resize(cut.size() - 30);
    check(!read_archive(cut, ArchiveFormat::kZip, [](ArchiveEntry&) { return true; }).ok, "zip: cut");
    int seen = 0;
    check(!read_archive(vec(kZip), ArchiveFormat::kZip, [&](ArchiveEntry&) { return ++seen < 3; }).ok && seen == 3,
          "zip: stopped by the caller");
}

void check_targz() {
    std::string error;
    auto tar = entries_of(vec(kTarGz), ArchiveFormat::kTarGz, &error);
    check(error.empty() && tar.size() == 8, "tar.gz: entries");
    check(tar.count("pkg/") && tar["pkg/"].directory, "tar.gz: a folder");
    check(bytes_text(tar["pkg/crusader-no-remorse-editor.exe"].data) == exe_text("NEWEDITOR"), "tar.gz: an executable");
    check(tar["pkg/crusader-no-remorse.exe"].executable && !tar["pkg/README.md"].executable, "tar.gz: modes");
    const std::string long_path = "pkg/languages/" + std::string(90, 'd') + "/" + std::string(40, 'f') + ".txt";
    check(tar.count(long_path) && bytes_text(tar[long_path].data) == "abc", "tar.gz: a path split in prefix and name");
    std::vector<std::uint8_t> corrupt = vec(kTarGz);
    corrupt[corrupt.size() / 2] ^= 0x10;
    check(!read_archive(corrupt, ArchiveFormat::kTarGz, [](ArchiveEntry&) { return true; }).ok, "tar.gz: corrupt");
    check(!read_archive(vec(kZip), ArchiveFormat::kTarGz, [](ArchiveEntry&) { return true; }).ok, "tar.gz: not a gzip");
}

void check_selection() {
    check(install_path("pkg/crusader-no-remorse.exe") == "crusader-no-remorse.exe", "path: executable");
    check(install_path("pkg/README.md") == "README.md", "path: readme");
    check(install_path("pkg/languages/fr/port.txt") == "languages/fr/port.txt", "path: languages");
    check(install_path("pkg/ceiling_lights/map001.json") == "ceiling_lights/map001.json", "path: ceiling lights");
    check(!install_path("pkg/data/PUT_GAME_DATA_HERE.txt"), "path: data/ is never touched");
    check(!install_path("pkg/data/STATIC/x"), "path: no game data");
    check(!install_path("pkg/../evil.txt") && !install_path("../x/y.txt") && !install_path("pkg/languages/../../x"),
          "path: no '..'");
    check(!install_path("/etc/passwd") && !install_path("C:/Windows/x.dll") && !install_path("pkg/C:/x") &&
              !install_path("pkg\\..\\x"),
          "path: not absolute, no drive");
    check(!install_path("pkg/.update-backup.txt") && !install_path("pkg/.hidden") && !install_path("pkg/x.exe.old"),
          "path: no hidden, no .old");
    check(!install_path("pkg/") && !install_path("") && !install_path("file.txt") && !install_path("pkg//a"),
          "path: odd ones");
    check(!install_path("pkg/lights/x.json") && !install_path("pkg/other/dir/x"), "path: other folders are not installed");
    check(install_path("pkg\\languages\\fr\\port.txt") == "languages/fr/port.txt", "path: backslashes");

    check(is_trusted_download_url(std::string(kReleaseDownloadPrefix) + "v1.80.0/x.zip"), "url: trusted");
    check(!is_trusted_download_url("https://example.org/x.zip") &&
              !is_trusted_download_url(std::string(kReleaseDownloadPrefix) + "../../x") &&
              !is_trusted_download_url(std::string(kReleaseDownloadPrefix) + "v1/x.zip?token=1") &&
              !is_trusted_download_url("http://github.com/sebsebseb1982/crusader-no-remorse-decomp/releases/download/v1/x"),
          "url: untrusted");

    const std::string sums =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF  crusader-no-remorse-1.80.0-linux-x64.tar.gz\n"
        "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210 *crusader-no-remorse-1.80.0-windows-x64.zip\r\n"
        "short  x\n";
    check(checksum_of(sums, "crusader-no-remorse-1.80.0-linux-x64.tar.gz") ==
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "sums: text mode, lower case");
    check(checksum_of(sums, "crusader-no-remorse-1.80.0-windows-x64.zip") ==
              "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",
          "sums: binary mode, CRLF");
    check(!checksum_of(sums, "other.zip") && !checksum_of(sums, "x") && !checksum_of("", "x"), "sums: absent");

    const auto release = parse_latest_release(R"({"tag_name": "v1.80.0", "html_url": "https://example.org/r", "assets": [
        {"name": "crusader-no-remorse-1.80.0-windows-x64.zip", "size": 1778,
         "browser_download_url": "https://github.com/sebsebseb1982/crusader-no-remorse-decomp/releases/download/v1.80.0/crusader-no-remorse-1.80.0-windows-x64.zip"},
        {"name": "crusader-no-remorse-1.80.0-linux-x64.tar.gz", "size": 5,
         "browser_download_url": "https://evil.example/x.tar.gz"},
        {"name": "SHA256SUMS", "size": 300,
         "browser_download_url": "https://github.com/sebsebseb1982/crusader-no-remorse-decomp/releases/download/v1.80.0/SHA256SUMS"},
        {"name": "no-url"}, 7]})");
    check(release && release->assets.size() == 3 && release->assets[0].size == 1778, "release: assets");
    if (!release) return;
    check(pick_archive(*release, "-windows-x64.zip") && pick_archive(*release, "-windows-x64.zip")->size == 1778,
          "release: the archive of the system");
    check(!pick_archive(*release, "-linux-x64.tar.gz"), "release: an archive from elsewhere is not used");
    check(!pick_archive(*release, "") && !pick_archive(*release, "-aarch64.zip"), "release: no archive");
    check(pick_checksums(*release).has_value(), "release: the checksums");
    check(archive_format_of("x-windows-x64.zip") == ArchiveFormat::kZip &&
              archive_format_of("x-linux-x64.tar.gz") == ArchiveFormat::kTarGz,
          "release: formats");
}

// --- the whole install, in a scratch folder

struct Scratch {
    fs::path dir;
    Scratch() {
        dir = fs::temp_directory_path() / ("crusader_update_test_" + std::to_string(std::time(nullptr)) + "_" +
                                           std::to_string(reinterpret_cast<std::uintptr_t>(this) & 0xFFFF));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
};

// A release whose archive is `archive` (zip), with its sums.
struct FakeRelease {
    ReleaseInfo info;
    std::string sums;
    std::vector<std::uint8_t> archive;
    int fetches = 0, downloads = 0;
    bool fail_download = false, cancel_in_progress = false;
    UpdateHooks hooks;
    SelfUpdater* updater = nullptr;

    FakeRelease(std::vector<std::uint8_t> bytes, const std::string& claimed_digest = {}) : archive(std::move(bytes)) {
        const std::string name = "crusader-no-remorse-1.80.0-windows-x64.zip";
        const std::string base = std::string(kReleaseDownloadPrefix) + "v1.80.0/";
        info.version = SemVer{1, 80, 0};
        info.assets = {{name, base + name, archive.size()}, {"SHA256SUMS", base + "SHA256SUMS", 100}};
        sums = (claimed_digest.empty() ? sha256_hex(archive.data(), archive.size()) : claimed_digest) + "  " + name + "\n";
        hooks.fetch = [this](const std::string& url) -> std::optional<std::string> {
            ++fetches;
            if (url.ends_with("SHA256SUMS")) return sums;
            return std::nullopt;
        };
        hooks.download = [this](const std::string&, const fs::path& file, std::uint64_t,
                                const std::function<bool(std::uint64_t)>& progress) {
            ++downloads;
            if (fail_download) return false;
            std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(archive.data()),
                                                        static_cast<std::streamsize>(archive.size()));
            if (cancel_in_progress && updater) updater->cancel();
            return progress(archive.size());
        };
    }
};

void make_install(const fs::path& dir) {
    write_text(dir / "crusader-no-remorse.exe", "OLDEXE");
    write_text(dir / "crusader-no-remorse-editor.exe", "OLDEDITOR");
    write_text(dir / "README.md", "readme 1\n");
    write_text(dir / "languages/fr/port.txt", "OLD\n");
    write_text(dir / "languages/de/old_only.txt", "kept\n");
    write_text(dir / "data/STATIC", "game data");
    write_text(dir / "data/CRUSADER.CFG", "user options");
}

void check_install() {
    // The whole install.
    {
        Scratch scratch;
        make_install(scratch.dir);
        FakeRelease release(vec(kZip));
        SelfUpdater updater(release.hooks, scratch.dir, "crusader-no-remorse.exe", "-windows-x64.zip");
        check(updater.can_install(release.info), "install: possible");
        updater.start(release.info, true);
        const UpdateStatus status = updater.status();
        check(status.phase == UpdatePhase::kDone, "install: done");
        check(read_text(scratch.dir / "crusader-no-remorse.exe") == exe_text("NEWEXE"), "install: new executable");
        check(read_text(scratch.dir / "crusader-no-remorse-editor.exe") == exe_text("NEWEDITOR"), "install: new editor");
        check(read_text(scratch.dir / "README.md") == "readme 2\n", "install: new readme");
        check(read_text(scratch.dir / "languages/fr/port.txt").starts_with("NEW = NOUVEAU"), "install: new language file");
        check(read_text(scratch.dir / "ceiling_lights/map001.json") == "{\"lamps\": []}\n", "install: a new folder");
        check(read_text(scratch.dir / "languages/de/old_only.txt") == "kept\n", "install: other files are kept");
        check(read_text(scratch.dir / "data/STATIC") == "game data" && read_text(scratch.dir / "data/CRUSADER.CFG") == "user options" &&
                  !fs::exists(scratch.dir / "data/PUT_GAME_DATA_HERE.txt"),
              "install: data/ is not touched");
        check(read_text(scratch.dir / "crusader-no-remorse.exe.old") == "OLDEXE", "install: the old executable is kept aside");
        check(!fs::exists(scratch.dir / ".update"), "install: staging removed");
        const std::string list = read_text(scratch.dir / ".update-backup.txt");
        check(list.find("crusader-no-remorse.exe.old") != std::string::npos &&
                  list.find("languages/fr/port.txt.old") != std::string::npos,
              "install: the backups are listed");
        // The next start deletes them, and only them.
        write_text(scratch.dir / "mine.old", "not listed");
        check(clean_after_update(scratch.dir) == 0, "clean: nothing left");
        check(!fs::exists(scratch.dir / "crusader-no-remorse.exe.old") && !fs::exists(scratch.dir / ".update-backup.txt") &&
                  fs::exists(scratch.dir / "mine.old") && read_text(scratch.dir / "crusader-no-remorse.exe") == exe_text("NEWEXE"),
              "clean: the listed .old files go, not others");
    }
    // A wrong checksum: nothing changes.
    {
        Scratch scratch;
        make_install(scratch.dir);
        FakeRelease release(vec(kZip), std::string(64, '0'));
        SelfUpdater updater(release.hooks, scratch.dir, "crusader-no-remorse.exe", "-windows-x64.zip");
        updater.start(release.info, true);
        check(updater.status().phase == UpdatePhase::kFailed && updater.status().failure == UpdateFailure::kChecksum,
              "checksum: refused");
        check(read_text(scratch.dir / "crusader-no-remorse.exe") == "OLDEXE" && !fs::exists(scratch.dir / ".update") &&
                  !fs::exists(scratch.dir / "crusader-no-remorse.exe.old"),
              "checksum: nothing changed, nothing left");
    }
    // Download failed, or cancelled.
    {
        Scratch scratch;
        make_install(scratch.dir);
        FakeRelease release(vec(kZip));
        release.fail_download = true;
        SelfUpdater updater(release.hooks, scratch.dir, "crusader-no-remorse.exe", "-windows-x64.zip");
        updater.start(release.info, true);
        check(updater.status().failure == UpdateFailure::kDownload && !fs::exists(scratch.dir / ".update"), "download: failed");
    }
    {
        Scratch scratch;
        make_install(scratch.dir);
        FakeRelease release(vec(kZip));
        SelfUpdater updater(release.hooks, scratch.dir, "crusader-no-remorse.exe", "-windows-x64.zip");
        release.updater = &updater;
        release.cancel_in_progress = true;
        updater.start(release.info, true);
        check(updater.status().phase == UpdatePhase::kFailed && read_text(scratch.dir / "crusader-no-remorse.exe") == "OLDEXE",
              "download: cancelled, nothing installed");
    }
    // The archive holds no such executable (the program was renamed).
    {
        Scratch scratch;
        make_install(scratch.dir);
        write_text(scratch.dir / "renamed.exe", "OLD");
        FakeRelease release(vec(kZip));
        SelfUpdater updater(release.hooks, scratch.dir, "renamed.exe", "-windows-x64.zip");
        updater.start(release.info, true);
        check(updater.status().failure == UpdateFailure::kInstall && read_text(scratch.dir / "README.md") == "readme 1\n",
              "install: no executable of that name, nothing changed");
    }
    // A hostile path is not installed (and nothing lands outside).
    {
        Scratch scratch;
        make_install(scratch.dir);
        FakeRelease release(vec(kEvilZip));
        SelfUpdater updater(release.hooks, scratch.dir, "crusader-no-remorse.exe", "-windows-x64.zip");
        updater.start(release.info, true);
        check(updater.status().phase == UpdatePhase::kFailed && !fs::exists(scratch.dir.parent_path() / "evil.txt") &&
                  !fs::exists(scratch.dir / "evil.txt"),
              "install: a '..' path is ignored");
    }
    // Rollback: the second file cannot be placed ("languages" is a file here):
    // the first one, already replaced, comes back.
    {
        Scratch scratch;
        write_text(scratch.dir / "crusader-no-remorse.exe", "OLDEXE");
        write_text(scratch.dir / "README.md", "readme 1\n");
        write_text(scratch.dir / "languages", "a file, not a folder");
        FakeRelease release(vec(kZip));
        SelfUpdater updater(release.hooks, scratch.dir, "crusader-no-remorse.exe", "-windows-x64.zip");
        updater.start(release.info, true);
        check(updater.status().phase == UpdatePhase::kFailed && updater.status().failure == UpdateFailure::kInstall,
              "rollback: failed");
        check(read_text(scratch.dir / "README.md") == "readme 1\n" && read_text(scratch.dir / "crusader-no-remorse.exe") == "OLDEXE" &&
                  !fs::exists(scratch.dir / "README.md.old") && !fs::exists(scratch.dir / ".update-backup.txt") &&
                  !fs::exists(scratch.dir / ".update"),
              "rollback: everything is back");
    }
    // tar.gz: the executable keeps its mode on a system that has one.
    {
        Scratch scratch;
        write_text(scratch.dir / "crusader-no-remorse.exe", "OLDEXE");
        FakeRelease release(vec(kTarGz));
        release.info.assets[0].name = "crusader-no-remorse-1.80.0-linux-x64.tar.gz";
        release.info.assets[0].url = std::string(kReleaseDownloadPrefix) + "v1.80.0/" + release.info.assets[0].name;
        release.sums = sha256_hex(release.archive.data(), release.archive.size()) + "  " + release.info.assets[0].name + "\n";
        SelfUpdater updater(release.hooks, scratch.dir, "crusader-no-remorse.exe", "-linux-x64.tar.gz");
        updater.start(release.info, true);
        check(updater.status().phase == UpdatePhase::kDone && read_text(scratch.dir / "crusader-no-remorse.exe") == exe_text("NEWEXE"),
              "tar.gz: installed");
#ifndef _WIN32
        const auto perms = fs::status(scratch.dir / "crusader-no-remorse.exe").permissions();
        check((perms & fs::perms::owner_exec) != fs::perms::none, "tar.gz: executable bit");
#endif
    }
    // Without an archive for this system (PortMaster), or with a foreign one.
    {
        Scratch scratch;
        FakeRelease release(vec(kZip));
        SelfUpdater none(release.hooks, scratch.dir, "crusader-no-remorse.exe", "");
        check(!none.can_install(release.info), "install: no archive for this system");
        none.start(release.info, true);
        check(none.status().failure == UpdateFailure::kNoPackage, "install: nothing to do without a package");
    }
    // A second start does nothing; the folder's rights.
    {
        Scratch scratch;
        make_install(scratch.dir);
        FakeRelease release(vec(kZip));
        SelfUpdater updater(release.hooks, scratch.dir, "crusader-no-remorse.exe", "-windows-x64.zip");
        updater.start(release.info, true);
        updater.start(release.info, true);
        check(release.downloads == 1, "install: started once");
        check(install_dir_writable(scratch.dir) && !install_dir_writable(scratch.dir / "crusader-no-remorse.exe"),
              "install: writable folder");
    }
    // On a thread of its own, the end can be waited for.
    {
        Scratch scratch;
        make_install(scratch.dir);
        FakeRelease release(vec(kZip));
        {
            SelfUpdater updater(release.hooks, scratch.dir, "crusader-no-remorse.exe", "-windows-x64.zip");
            updater.start(release.info);
            for (int i = 0; i < 500 && updater.status().phase != UpdatePhase::kDone; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(updater.status().phase == UpdatePhase::kDone, "thread: done");
        }
        check(read_text(scratch.dir / "crusader-no-remorse.exe") == exe_text("NEWEXE"), "thread: installed");
    }
}

}  // namespace

int main() {
    check_sha256();
    check_inflate();
    check_zip();
    check_targz();
    check_selection();
    check_install();
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
