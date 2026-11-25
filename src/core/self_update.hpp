#pragma once
// Port addition (@orig none): the game installs a newer release of the port
// itself (GitHub issue #36 asked for a notice; this goes on from it).
//
// What a release holds (.github/workflows/release.yml): one archive per
// system, "crusader-no-remorse-X.Y.Z-windows-x64.zip" or "...-linux-x64.tar.gz"
// with a folder of the same name inside (the executables, languages/,
// ceiling_lights/, data/PUT_GAME_DATA_HERE.txt, README.md), and SHA256SUMS.
//
// The steps, on a thread of their own (SelfUpdater):
//   1. SHA256SUMS of the release is read; the archive of this system is
//      downloaded to <install>/.update/ and its SHA-256 compared with it.
//      Only addresses under kReleaseDownloadPrefix are used.
//   2. The files of the archive that the game installs (install_path: the
//      executables and other files next to them, languages/, ceiling_lights/;
//      never data/, so the game files, the saves and CRUSADER.CFG are left
//      alone) are written to <install>/.update/files/, and the running
//      executable must be among them.
//   3. Each is swapped in: the file it replaces is renamed "<name>.old" (a
//      running executable can be renamed, not overwritten, on Windows), the
//      new one moved in place. Any failure puts everything back.
//   4. The names of the ".old" files go to <install>/.update-backup.txt. The
//      program restarts itself (platform/process.hpp); the new version, once
//      it runs, deletes them (clean_after_update). If it did not start, the
//      old files are there to be renamed back.
//
// The checksum comes from the same place as the archive, so it protects
// against a broken download, not against a release that was tampered with.

#include "core/archive.hpp"
#include "core/news.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace crusader {

inline constexpr const char* kChecksumsAsset = "SHA256SUMS";

// The end of the name of the archive made for this build's system
// ("-windows-x64.zip", "-linux-x64.tar.gz"), null where releases ship none
// (the aarch64 build of PortMaster handhelds is updated by PortMaster).
const char* platform_archive_suffix();
ArchiveFormat archive_format_of(std::string_view archive_name);

// The release's archive for `suffix`, and its SHA256SUMS; none when absent or
// not under kReleaseDownloadPrefix.
std::optional<ReleaseAsset> pick_archive(const ReleaseInfo& release, std::string_view suffix);
std::optional<ReleaseAsset> pick_checksums(const ReleaseInfo& release);
bool is_trusted_download_url(std::string_view url);

// The digest `sums` ("<hex>  <name>" or "<hex> *<name>" lines, as sha256sum
// writes them) gives for `name`, lower case; none when absent or malformed.
std::optional<std::string> checksum_of(std::string_view sums, std::string_view name);

// Where the archive's `path` is installed, relative to the install folder:
// none for what is not installed (data/, folders, odd paths: "..", absolute,
// drive letters, hidden files).
std::optional<std::string> install_path(std::string_view archive_path);

// Whether files can be created and renamed in `install_dir`.
bool install_dir_writable(const std::filesystem::path& install_dir);

// Start-up of any version: removes what an update left (the ".old" files it
// listed, the staging folder). Returns how many files remain, in use.
int clean_after_update(const std::filesystem::path& install_dir);

enum class UpdatePhase { kIdle, kDownloading, kInstalling, kDone, kFailed };
enum class UpdateFailure { kNone, kNoPackage, kDownload, kChecksum, kInstall };

struct UpdateStatus {
    UpdatePhase phase = UpdatePhase::kIdle;
    std::uint64_t done = 0, total = 0;  // bytes, while downloading
    UpdateFailure failure = UpdateFailure::kNone;
    std::string detail;  // English, for the log
};

// What the platform gives (platform/http_get.hpp): the body at a URL, and a
// file downloaded; `progress` gets the bytes written so far and returns false
// to cancel.
struct UpdateHooks {
    std::function<std::optional<std::string>(const std::string& url)> fetch;
    std::function<bool(const std::string& url, const std::filesystem::path& file, std::uint64_t expected_size,
                       const std::function<bool(std::uint64_t)>& progress)>
        download;
};

class SelfUpdater {
public:
    // `exe_name`: the file name of the running executable, which the archive
    // must hold. `suffix`: platform_archive_suffix().
    SelfUpdater(UpdateHooks hooks, std::filesystem::path install_dir, std::string exe_name, std::string suffix);
    ~SelfUpdater();
    SelfUpdater(const SelfUpdater&) = delete;
    SelfUpdater& operator=(const SelfUpdater&) = delete;

    // Whether `release` has what installing needs (an archive for this system
    // and the checksums).
    bool can_install(const ReleaseInfo& release) const;
    // Starts the steps above; `synchronous` runs them here (tests). Does
    // nothing while running or once done.
    void start(const ReleaseInfo& release, bool synchronous = false);
    // Stops the download (the install, once begun, runs to its end).
    void cancel();
    UpdateStatus status() const;

private:
    struct State;
    static void run(const std::shared_ptr<State>& state, UpdateHooks hooks, std::filesystem::path install_dir,
                    std::string exe_name, std::string suffix, ReleaseInfo release);

    UpdateHooks hooks_;
    std::filesystem::path install_dir_;
    std::string exe_name_, suffix_;
    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace crusader
