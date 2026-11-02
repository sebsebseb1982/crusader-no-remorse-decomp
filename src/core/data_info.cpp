#include "core/data_info.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>

namespace crusader {

namespace {

// Entries (far pointers) of USECODE/UNKCOFF.DAT.
constexpr std::uintmax_t kFrenchUnkcoffSize = 309 * 4;
constexpr std::uintmax_t kUsUnkcoffSize = 311 * 4;
constexpr std::uintmax_t kV101UnkcoffSize = 308 * 4;

}  // namespace

// @orig none: port-only data version detection
DataInfo detect_data_info(const std::filesystem::path& data) {
    DataInfo info;
    std::ifstream in(data / "CRUSADER.EXE", std::ios::binary);
    if (!in) {
        // Without the executable, the size of the intrinsic table tells the
        // usecode: 309 entries in the French one, 311 in the US one, 308 in
        // the 1.01 one (its version known from it).
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(data / "USECODE/UNKCOFF.DAT", error);
        if (error) return info;
        if (size == kFrenchUnkcoffSize) info.language = "FR";
        else if (size == kUsUnkcoffSize) info.language = "US";
        else if (size == kV101UnkcoffSize) {
            info.language = "US";
            info.version = "1.01";
        }
        return info;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    constexpr std::string_view kBanner = "No Remorse V";
    for (std::size_t at = bytes.find(kBanner); at != std::string::npos && info.version.empty();
         at = bytes.find(kBanner, at + 1)) {
        std::size_t end = at + kBanner.size();
        const std::size_t begin = end;
        while (end < bytes.size() && (std::isdigit(static_cast<unsigned char>(bytes[end])) || bytes[end] == '.')) ++end;
        if (end > begin) info.version = bytes.substr(begin, end - begin);
    }
    // A message of the start-up code; the 1.01 executable has none, but its
    // English item names (DTABLE: family 12, record 5).
    struct Language {
        std::string_view marker, name;
    };
    constexpr Language kLanguages[] = {
        {"Appuyer sur une touche pour continuer.", "FR"},
        {"Press any key to continue.", "US"},
        {"SPIDER BOMB", "US"},
    };
    for (const Language& l : kLanguages) {
        if (bytes.find(l.marker) != std::string::npos) {
            info.language = l.name;
            break;
        }
    }
    return info;
}

}  // namespace crusader
