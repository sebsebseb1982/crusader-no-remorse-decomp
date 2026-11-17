#include "core/data_files.hpp"

#include <system_error>
#include <utility>

namespace crusader {

DataFiles::DataFiles(std::filesystem::path data, std::filesystem::path mod, std::filesystem::path language)
    : data_(std::move(data)), mod_(std::move(mod)), language_(std::move(language)) {}

// @orig none: the language and mod folders are port additions
std::filesystem::path DataFiles::find(const std::string& relative) const {
    std::error_code error;
    for (const std::filesystem::path* dir : {&language_, &mod_}) {
        if (dir->empty()) continue;
        std::filesystem::path p = *dir / relative;
        if (std::filesystem::is_regular_file(p, error)) return p;
    }
    return data_ / relative;
}

}  // namespace crusader
