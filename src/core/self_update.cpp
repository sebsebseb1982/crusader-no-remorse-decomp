#include "core/self_update.hpp"

#include "core/sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace crusader {

namespace fs = std::filesystem;

namespace {

constexpr const char* kStagingDir = ".update";
constexpr const char* kBackupList = ".update-backup.txt";
constexpr std::uint64_t kMaxPackage = 128u << 20;  // bytes, the archive as downloaded

bool ends_with(std::string_view text, std::string_view end) {
    return text.size() >= end.size() && text.substr(text.size() - end.size()) == end;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = text.find(separator, start);
        parts.emplace_back(text.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) return parts;
        start = end + 1;
    }
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    return static_cast<bool>(out);
}

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path, std::uint64_t max) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > max) return std::nullopt;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in) return std::nullopt;
    return data;
}

// The SHA-256 of a file, read in pieces.
std::optional<std::string> hash_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash.update(buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (!in.eof()) return std::nullopt;
    return hash.finish_hex();
}

// One file swapped in: the file it replaced, renamed aside (empty when it
// was new).
struct Swapped {
    fs::path target, backup;
};

void roll_back(const std::vector<Swapped>& done) {
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
        std::error_code ignored;
        fs::remove(it->target, ignored);
        if (!it->backup.empty()) fs::rename(it->backup, it->target, ignored);
    }
}

// Moves the staged files over the installed ones; false (all put back) when
// one cannot. `backups` gets the ".old" files made.
bool swap_in(const fs::path& staged_root, const fs::path& install_dir, const std::vector<std::string>& files,
             std::vector<fs::path>& backups, std::string& why) {
    std::vector<Swapped> done;
    for (const std::string& rel : files) {
        const fs::path target = install_dir / fs::path(rel);
        const fs::path staged = staged_root / fs::path(rel);
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        Swapped entry{target, {}};
        if (fs::exists(target, error)) {
            entry.backup = target;
            entry.backup += ".old";
            fs::remove(entry.backup, error);  // a left-over of an earlier update
            error.clear();
            fs::rename(target, entry.backup, error);
            if (error) {
                why = "cannot rename " + target.string() + ": " + error.message();
                roll_back(done);
                return false;
            }
        }
        done.push_back(entry);
        fs::rename(staged, target, error);
        if (error) {
            why = "cannot move " + staged.string() + " in place: " + error.message();
            roll_back(done);
            return false;
        }
    }
    for (const Swapped& entry : done) {
        if (!entry.backup.empty()) backups.push_back(entry.backup);
    }
    return true;
}

void write_backup_list(const fs::path& install_dir, const std::vector<fs::path>& backups) {
    std::ofstream out(install_dir / kBackupList, std::ios::trunc);
    for (const fs::path& backup : backups) {
        std::error_code error;
        out << fs::relative(backup, install_dir, error).generic_string() << "\n";
    }
}

}  // namespace

// @orig none: port's self-update
const char* platform_archive_suffix() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "-windows-x64.zip";
#elif defined(__linux__) && defined(__x86_64__)
    return "-linux-x64.tar.gz";
#else
    return nullptr;
#endif
}

// @orig none: port's self-update
ArchiveFormat archive_format_of(std::string_view archive_name) {
    return ends_with(archive_name, ".zip") ? ArchiveFormat::kZip : ArchiveFormat::kTarGz;
}

// @orig none: port's self-update
bool is_trusted_download_url(std::string_view url) {
    const std::string_view prefix = kReleaseDownloadPrefix;
    // "..", a query or a fragment could lead elsewhere than the prefix says.
    return url.size() > prefix.size() && url.substr(0, prefix.size()) == prefix &&
           url.find("..") == std::string_view::npos && url.find_first_of("?#\\ ") == std::string_view::npos;
}

// @orig none: port's self-update
std::optional<ReleaseAsset> pick_archive(const ReleaseInfo& release, std::string_view suffix) {
    if (suffix.empty()) return std::nullopt;
    for (const ReleaseAsset& asset : release.assets) {
        if (ends_with(asset.name, suffix) && is_trusted_download_url(asset.url)) return asset;
    }
    return std::nullopt;
}

// @orig none: port's self-update
std::optional<ReleaseAsset> pick_checksums(const ReleaseInfo& release) {
    for (const ReleaseAsset& asset : release.assets) {
        if (asset.name == kChecksumsAsset && is_trusted_download_url(asset.url)) return asset;
    }
    return std::nullopt;
}

// @orig none: port's self-update
std::optional<std::string> checksum_of(std::string_view sums, std::string_view name) {
    for (const std::string& raw : split(sums, '\n')) {
        std::string_view line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.size() < 66) continue;
        const std::string_view hex = line.substr(0, 64);
        if (line[64] != ' ') continue;
        std::string_view file = line.substr(65);
        if (!file.empty() && (file[0] == '*' || file[0] == ' ')) file.remove_prefix(1);
        if (file != name) continue;
        std::string digest(hex);
        for (char& c : digest) {
            if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
        }
        return digest;
    }
    return std::nullopt;
}

// @orig none: port's self-update
std::optional<std::string> install_path(std::string_view archive_path) {
    std::string path(archive_path);
    std::replace(path.begin(), path.end(), '\\', '/');
    for (const char c : path) {
        if (static_cast<unsigned char>(c) < 0x20 || c == ':') return std::nullopt;
    }
    if (path.empty() || path.front() == '/' || path.back() == '/') return std::nullopt;
    const std::vector<std::string> parts = split(path, '/');
    for (const std::string& part : parts) {
        if (part.empty() || part == "." || part == ".." || part.front() == '.' || ends_with(part, ".old")) {
            return std::nullopt;
        }
    }
    // The first part is the folder of the package; what it holds, then:
    // a file beside the executable, or a file of languages/ or ceiling_lights/.
    if (parts.size() == 2) return parts[1];
    if (parts.size() > 2 && (parts[1] == "languages" || parts[1] == "ceiling_lights")) {
        std::string rel = parts[1];
        for (std::size_t i = 2; i < parts.size(); ++i) rel += "/" + parts[i];
        return rel;
    }
    return std::nullopt;
}

// @orig none: port's self-update
bool install_dir_writable(const fs::path& install_dir) {
    std::error_code error;
    const fs::path dir = install_dir / kStagingDir;
    fs::create_directories(dir, error);
    if (error) return false;
    const fs::path probe = dir / "probe";
    const fs::path moved = dir / "probe.moved";
    const bool ok = write_file(probe, {'x'});
    fs::rename(probe, moved, error);
    const bool renamed = !error;
    fs::remove(probe, error);
    fs::remove(moved, error);
    fs::remove(dir, error);
    return ok && renamed;
}

// @orig none: port's self-update
int clean_after_update(const fs::path& install_dir) {
    std::error_code error;
    fs::remove_all(install_dir / kStagingDir, error);
    const fs::path list = install_dir / kBackupList;
    std::ifstream in(list);
    if (!in) return 0;
    std::vector<std::string> remaining;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        // Only what an update lists, and only its ".old" files below the folder.
        if (line.empty() || !ends_with(line, ".old") || line.find("..") != std::string::npos ||
            line.front() == '/' || line.find(':') != std::string::npos) {
            continue;
        }
        const fs::path file = install_dir / fs::path(line);
        if (!fs::exists(file, error)) continue;
        error.clear();
        fs::remove(file, error);
        if (error) remaining.push_back(line);
    }
    in.close();
    if (remaining.empty()) {
        fs::remove(list, error);
    } else {
        std::ofstream out(list, std::ios::trunc);
        for (const std::string& line : remaining) out << line << "\n";
    }
    return static_cast<int>(remaining.size());
}

// --- SelfUpdater

struct SelfUpdater::State {
    mutable std::mutex mutex;
    UpdateStatus status;
    bool started = false;
    std::atomic<bool> cancel{false};
};

// @orig none: port's self-update
SelfUpdater::SelfUpdater(UpdateHooks hooks, fs::path install_dir, std::string exe_name, std::string suffix)
    : hooks_(std::move(hooks)),
      install_dir_(std::move(install_dir)),
      exe_name_(std::move(exe_name)),
      suffix_(std::move(suffix)),
      state_(std::make_shared<State>()) {}

// @orig none: port's self-update
SelfUpdater::~SelfUpdater() {
    state_->cancel = true;
    if (!thread_.joinable()) return;
    // Out while downloading: the thread ends by itself, a slow network never
    // holds the game's end (it keeps what it needs). Once installing, the
    // files must be swapped to the end: wait. The thread starts installing
    // only when `cancel` is still false, under the mutex.
    bool installing;
    {
        const std::lock_guard lock(state_->mutex);
        installing = state_->status.phase == UpdatePhase::kInstalling;
    }
    if (installing) thread_.join();
    else thread_.detach();
}

// @orig none: port's self-update
bool SelfUpdater::can_install(const ReleaseInfo& release) const {
    return pick_archive(release, suffix_) && pick_checksums(release) && !exe_name_.empty() &&
           !ends_with(exe_name_, ".old");
}

// @orig none: port's self-update
void SelfUpdater::start(const ReleaseInfo& release, bool synchronous) {
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->started) return;
        state_->started = true;
        state_->status = {};
        state_->status.phase = UpdatePhase::kDownloading;
    }
    if (synchronous) {
        run(state_, hooks_, install_dir_, exe_name_, suffix_, release);
        return;
    }
    thread_ = std::thread(run, state_, hooks_, install_dir_, exe_name_, suffix_, release);
}

// @orig none: port's self-update
void SelfUpdater::cancel() { state_->cancel = true; }

// @orig none: port's self-update
UpdateStatus SelfUpdater::status() const {
    const std::lock_guard lock(state_->mutex);
    return state_->status;
}

// @orig none: port's self-update
void SelfUpdater::run(const std::shared_ptr<State>& state, UpdateHooks hooks, fs::path install_dir,
                      std::string exe_name, std::string suffix, ReleaseInfo release) {
    const auto fail = [&](UpdateFailure failure, std::string detail) {
        std::fprintf(stderr, "update failed: %s\n", detail.c_str());
        const std::lock_guard lock(state->mutex);
        state->status.phase = UpdatePhase::kFailed;
        state->status.failure = failure;
        state->status.detail = std::move(detail);
    };
    const fs::path staging = install_dir / kStagingDir;
    const auto fail_clean = [&](UpdateFailure failure, std::string detail) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        fail(failure, std::move(detail));
    };

    const auto archive = pick_archive(release, suffix);
    const auto sums_asset = pick_checksums(release);
    if (!archive || !sums_asset || !hooks.fetch || !hooks.download) {
        fail(UpdateFailure::kNoPackage, "the release has no package for this system");
        return;
    }
    const auto sums = hooks.fetch(sums_asset->url);
    const auto expected = sums ? checksum_of(*sums, archive->name) : std::nullopt;
    if (!expected) {
        fail(UpdateFailure::kDownload, "no checksum of " + archive->name);
        return;
    }
    if (state->cancel) {
        fail(UpdateFailure::kDownload, "cancelled");
        return;
    }

    // 1. Download.
    std::error_code error;
    fs::remove_all(staging, error);
    fs::create_directories(staging / "files", error);
    if (error) {
        fail(UpdateFailure::kInstall, "cannot create " + staging.string() + ": " + error.message());
        return;
    }
    {
        const std::lock_guard lock(state->mutex);
        state->status.total = archive->size;
    }
    const fs::path package = staging / "package";
    const bool downloaded = hooks.download(archive->url, package, archive->size, [&](std::uint64_t written) {
        const std::lock_guard lock(state->mutex);
        state->status.done = written;
        if (written > kMaxPackage) state->cancel = true;
        return !state->cancel;
    });
    if (!downloaded) {
        fail_clean(UpdateFailure::kDownload, state->cancel ? "cancelled" : "cannot download " + archive->url);
        return;
    }
    const auto digest = hash_file(package);
    if (!digest || *digest != *expected) {
        fail_clean(UpdateFailure::kChecksum, "checksum of " + archive->name + " is " + (digest ? *digest : "unreadable") +
                                                 ", expected " + *expected);
        return;
    }

    // From here the steps are not interrupted by the destructor's cancel.
    {
        const std::lock_guard lock(state->mutex);
        if (state->cancel) {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
            state->status.phase = UpdatePhase::kFailed;
            state->status.failure = UpdateFailure::kDownload;
            state->status.detail = "cancelled";
            return;
        }
        state->status.phase = UpdatePhase::kInstalling;
    }

    // 2. Unpack what is installed.
    const auto bytes = read_file(package, kMaxPackage);
    if (!bytes) {
        fail_clean(UpdateFailure::kInstall, "cannot read the package");
        return;
    }
    std::vector<std::string> files;
    std::string write_error;
    const ArchiveResult read = read_archive(*bytes, archive_format_of(archive->name), [&](ArchiveEntry& entry) {
        if (entry.directory) return true;
        const auto rel = install_path(entry.path);
        if (!rel) return true;
        const fs::path out = staging / "files" / fs::path(*rel);
        std::error_code e;
        fs::create_directories(out.parent_path(), e);
        if (e || !write_file(out, entry.data)) {
            write_error = "cannot write " + out.string();
            return false;
        }
        if (entry.executable || *rel == exe_name) {
            fs::permissions(out, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, e);
        }
        files.push_back(*rel);
        return true;
    });
    if (!read.ok) {
        fail_clean(UpdateFailure::kInstall, !write_error.empty() ? write_error : read.error);
        return;
    }
    if (std::find(files.begin(), files.end(), exe_name) == files.end()) {
        fail_clean(UpdateFailure::kInstall, "the package holds no " + exe_name);
        return;
    }

    // 3. Swap in, the executable last: a failure before it leaves the old one.
    std::stable_partition(files.begin(), files.end(), [&](const std::string& f) { return f != exe_name; });
    std::vector<fs::path> backups;
    std::string why;
    if (!swap_in(staging / "files", install_dir, files, backups, why)) {
        fail_clean(UpdateFailure::kInstall, why);
        return;
    }
    write_backup_list(install_dir, backups);
    {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
    }
    const std::lock_guard lock(state->mutex);
    state->status.phase = UpdatePhase::kDone;
}

}  // namespace crusader
