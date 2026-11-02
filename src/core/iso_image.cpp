#include "core/iso_image.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace crusader {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kLogicalSector = 2048;

std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// @orig none: port-only CD image reader
void IsoImage::read(std::uint32_t lba, std::uint64_t bytes, std::vector<std::uint8_t>& out) const {
    out.resize(bytes);
    std::uint64_t done = 0;
    while (done < bytes) {
        const std::uint64_t sector = lba + done / kLogicalSector;
        const std::uint64_t in_sector = done % kLogicalSector;
        const std::uint64_t n = std::min<std::uint64_t>(kLogicalSector - in_sector, bytes - done);
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(sector * sector_size_ + data_offset_ + in_sector));
        in_.read(reinterpret_cast<char*>(out.data() + done), static_cast<std::streamsize>(n));
        if (static_cast<std::uint64_t>(in_.gcount()) != n) throw std::runtime_error("ISO image truncated");
        done += n;
    }
}

void IsoImage::walk(std::uint32_t lba, std::uint32_t size, const std::string& prefix) {
    std::vector<std::uint8_t> dir;
    read(lba, size, dir);
    std::size_t pos = 0;
    while (pos < dir.size()) {
        const std::uint8_t len = dir[pos];
        if (len == 0) {  // records do not cross a sector: skip the padding
            pos = (pos / kLogicalSector + 1) * kLogicalSector;
            continue;
        }
        if (pos + len > dir.size() || len < 34) break;
        const std::uint8_t* r = dir.data() + pos;
        pos += len;
        const std::uint8_t name_len = r[32];
        if (33u + name_len > len) break;
        std::string name(reinterpret_cast<const char*>(r + 33), name_len);
        if (name_len == 1 && (name[0] == '\0' || name[0] == '\x01')) continue;  // "." and ".."
        if (const auto semi = name.find(';'); semi != std::string::npos) name.erase(semi);
        if (!name.empty() && name.back() == '.') name.pop_back();
        const std::uint32_t at = le32(r + 2), bytes = le32(r + 10);
        if (r[25] & 2) walk(at, bytes, prefix + name + "/");
        else files_.push_back({prefix + name, at, bytes});
    }
}

IsoImage IsoImage::open(const fs::path& path) {
    IsoImage iso;
    iso.in_.open(path, std::ios::binary);
    if (!iso.in_) throw std::runtime_error("cannot open " + path.string());
    // "CD001" of the primary volume descriptor, sector 16: plain 2048-byte
    // sectors, or raw 2352-byte ones (sync + header 16 bytes, mode 1).
    struct Layout {
        std::uint32_t sector, offset;
    };
    for (const Layout l : {Layout{2048, 0}, Layout{2352, 16}, Layout{2352, 24}}) {
        char id[6] = {};
        iso.in_.clear();
        iso.in_.seekg(static_cast<std::streamoff>(16ull * l.sector + l.offset));
        iso.in_.read(id, 6);
        if (iso.in_.gcount() == 6 && id[0] == 1 && std::memcmp(id + 1, "CD001", 5) == 0) {
            iso.sector_size_ = l.sector;
            iso.data_offset_ = l.offset;
            std::vector<std::uint8_t> pvd;
            iso.read(16, kLogicalSector, pvd);
            iso.walk(le32(pvd.data() + 156 + 2), le32(pvd.data() + 156 + 10), "");
            return iso;
        }
    }
    throw std::runtime_error(path.string() + " is not an ISO 9660 image");
}

void IsoImage::extract(const fs::path& dest, const std::vector<std::string>& skip,
                       const std::function<void(std::uint64_t, std::uint64_t)>& progress) const {
    auto skipped = [&](const Entry& e) {
        const std::string top = upper(e.path.substr(0, e.path.find('/')));
        return e.path.find('/') != std::string::npos && std::find(skip.begin(), skip.end(), top) != skip.end();
    };
    std::uint64_t total = 0, written = 0;
    for (const Entry& e : files_) {
        if (!skipped(e)) total += e.size;
    }
    std::vector<std::uint8_t> buffer;
    constexpr std::uint32_t kChunk = 4u << 20;
    for (const Entry& e : files_) {
        if (skipped(e)) continue;
        const fs::path target = dest / e.path;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + target.string());
        for (std::uint32_t done = 0; done < e.size;) {
            const std::uint32_t n = std::min(kChunk, e.size - done);
            read(e.lba + done / kLogicalSector, n, buffer);
            out.write(reinterpret_cast<const char*>(buffer.data()), n);
            done += n;
            written += n;
            if (progress) progress(written, total);
        }
    }
}

// @orig none: port-only installation from a CD image
std::size_t install_from_iso(const fs::path& iso_path, const fs::path& data) {
    const IsoImage iso = IsoImage::open(iso_path);
    std::fprintf(stderr, "Extracting %s into %s ...\n", iso_path.string().c_str(), data.string().c_str());
    int last = -1;
    iso.extract(data, {"CATALOG"}, [&](std::uint64_t done, std::uint64_t total) {
        const int percent = total ? static_cast<int>(done * 100 / total) : 100;
        if (percent / 10 != last / 10) std::fprintf(stderr, "  %d%%\n", percent);
        last = percent;
    });
    return iso.files().size();
}

bool is_disc_image(const fs::path& path) {
    const std::string ext = upper(path.extension().string());
    return ext == ".ISO" || ext == ".GOG";
}

fs::path find_iso(const fs::path& dir) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (entry.is_regular_file(error) && is_disc_image(entry.path())) return entry.path();
    }
    return {};
}

void prepare_game_data(const fs::path& data, const fs::path& iso) {
    std::error_code error;
    if (!fs::exists(data, error)) {
        fs::create_directories(data, error);
        if (std::ofstream note(data / "PUT_GAME_DATA_HERE.txt"); note) {
            note << "Put the game data here: either the files of the install folder of Crusader: No Remorse\n"
                    "(French 1.08, US 1.23 or English 1.01: STATIC, USECODE, SOUND, FLICS, CRUSADER.EXE...), or the .iso image of\n"
                    "the CD (GAME.GOG in the GOG release), which the game extracts on its first launch.\n";
        }
    }
    if (fs::exists(data / "STATIC", error)) return;
    const fs::path source = iso.empty() ? find_iso(data) : iso;
    if (source.empty()) return;
    install_from_iso(source, data);
}

}  // namespace crusader
