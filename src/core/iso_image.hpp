#pragma once
// Reader of ISO 9660 CD images (.iso), to take the game data from a copy of
// the CD instead of an install folder. The files of the CD are laid out as the
// install folder (STATIC/, USECODE/, SOUND/, FLICS/, CRUSADER.EXE...), plus
// CATALOG/, an unrelated demo browser the port does not need.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace crusader {

class IsoImage {
public:
    struct Entry {
        std::string path;  // relative, '/' separated, without the ";1" version
        std::uint32_t lba;
        std::uint32_t size;
    };

    // Throws std::runtime_error when the file is not an ISO 9660 image.
    static IsoImage open(const std::filesystem::path& path);

    const std::vector<Entry>& files() const { return files_; }

    // Writes every file whose first path component is not in `skip` under
    // `dest`. `progress` gets the bytes written so far and the total.
    void extract(const std::filesystem::path& dest, const std::vector<std::string>& skip,
                 const std::function<void(std::uint64_t, std::uint64_t)>& progress = {}) const;

private:
    mutable std::ifstream in_;
    std::uint32_t sector_size_ = 2048;  // 2352 for raw images (data at +16)
    std::uint32_t data_offset_ = 0;
    std::vector<Entry> files_;

    void read(std::uint32_t lba, std::uint64_t bytes, std::vector<std::uint8_t>& out) const;
    void walk(std::uint32_t lba, std::uint32_t size, const std::string& prefix);
};

// Copies the game data of the ISO at `iso` into `data` (see IsoImage::extract).
// Returns the number of files written; prints its progress on stderr.
std::size_t install_from_iso(const std::filesystem::path& iso, const std::filesystem::path& data);

// A CD image by its name: *.iso, or *.gog (GAME.GOG of the GOG release, an ISO
// image under another extension), in any case.
bool is_disc_image(const std::filesystem::path& path);

// The first CD image (is_disc_image) found in `dir`, empty when there is none.
std::filesystem::path find_iso(const std::filesystem::path& dir);

// Makes `data` usable: when it holds no game data (no STATIC/) and an ISO is
// given, or sits in it, the ISO is extracted into it. Creates `data` with a
// note on where to put the files when it does not exist.
void prepare_game_data(const std::filesystem::path& data, const std::filesystem::path& iso = {});

}  // namespace crusader
