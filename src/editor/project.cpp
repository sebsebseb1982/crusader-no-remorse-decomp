#include "editor/project.hpp"

#include <stdexcept>

namespace crusader::editor {

std::filesystem::path resolve_file(const std::filesystem::path& data, const std::filesystem::path& mod,
                                   const std::string& relative) {
    if (!mod.empty()) {
        std::filesystem::path p = mod / relative;
        if (std::filesystem::exists(p)) return p;
    }
    return data / relative;
}

Project::Project(std::filesystem::path data, std::filesystem::path mod)
    : data_(std::move(data)), mod_(std::move(mod)) {
    fixed_ = FlexEntries::read(file("STATIC/FIXED.DAT").string());
    globs_ = FlexEntries::read(file("STATIC/GLOB.FLX").string());
}

std::vector<std::size_t> Project::used_maps() const {
    std::vector<std::size_t> maps;
    for (std::size_t m = 0; m < fixed_.entries.size(); ++m) {
        if (!fixed_.entries[m].empty()) maps.push_back(m);
    }
    return maps;
}

std::size_t Project::free_map() const {
    for (std::size_t m = 1; m < fixed_.entries.size() && m < 256; ++m) {
        if (fixed_.entries[m].empty()) return m;
    }
    return 0;
}

Level Project::load_map(std::size_t map) const {
    if (map >= fixed_.entries.size()) return {};
    return Level::parse(fixed_.entries[map]);
}

void Project::store_map(std::size_t map, const Level& level) {
    if (map == 0 || map > 255) throw std::runtime_error("map numbers go from 1 to 255");
    if (map >= fixed_.entries.size()) fixed_.entries.resize(map + 1);
    auto bytes = level.serialize();
    if (bytes != fixed_.entries[map]) {
        fixed_.entries[map] = std::move(bytes);
        fixed_dirty_ = true;
    }
}

Glob Project::glob(std::size_t index) const {
    if (index >= globs_.entries.size()) return {};
    return Glob::parse(globs_.entries[index]);
}

void Project::store_glob(std::size_t index, const Glob& glob) {
    if (index == 0) throw std::runtime_error("glob 0 means no glob");
    if (index > 0xFFFF) throw std::runtime_error("glob numbers are 16-bit");
    if (index >= globs_.entries.size()) globs_.entries.resize(index + 1);
    auto bytes = glob.serialize();
    if (bytes != globs_.entries[index]) {
        globs_.entries[index] = std::move(bytes);
        globs_dirty_ = true;
    }
}

std::size_t Project::add_glob(const Glob& glob) {
    std::size_t index = 1;
    while (index < globs_.entries.size() && !globs_.entries[index].empty()) ++index;
    store_glob(index, glob);
    return index;
}

void Project::save() {
    if (mod_.empty()) throw std::runtime_error("no mod folder: the original data are never overwritten");
    std::filesystem::create_directories(mod_ / "STATIC");
    if (fixed_dirty_) fixed_.write((mod_ / "STATIC/FIXED.DAT").string());
    if (globs_dirty_) globs_.write((mod_ / "STATIC/GLOB.FLX").string());
    fixed_dirty_ = globs_dirty_ = false;
}

}  // namespace crusader::editor
