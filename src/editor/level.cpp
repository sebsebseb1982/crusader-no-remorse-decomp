#include "editor/level.hpp"

#include "core/flex.hpp"

#include <atomic>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace crusader::editor {

namespace {

std::uint16_t u16(std::span<const std::uint8_t> d, std::size_t at) {
    return static_cast<std::uint16_t>(d[at] | d[at + 1] << 8);
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void write_record(std::vector<std::uint8_t>& out, const LevelItem& item, std::uint16_t x) {
    put16(out, x);
    put16(out, static_cast<std::uint32_t>(item.y));
    out.push_back(static_cast<std::uint8_t>(item.z));
    put16(out, item.shape);
    out.push_back(item.frame);
    put16(out, item.flags);
    put16(out, item.quality);
    out.push_back(item.npc_number);
    out.push_back(item.map_number);
    put16(out, item.next);
}

// The x of a content is written as stored (its depth, set_depths): some
// records of map 244 are not items and carry any value there.
void write_tree(std::vector<std::uint8_t>& out, const LevelItem& item) {
    write_record(out, item, static_cast<std::uint16_t>(item.x));
    for (const LevelItem& c : item.contents) write_tree(out, c);
}

std::size_t count_tree(const LevelItem& item) {
    std::size_t n = 1;
    for (const LevelItem& c : item.contents) n += count_tree(c);
    return n;
}

LevelItem* find_in(std::vector<LevelItem>& items, std::uint32_t uid) {
    for (LevelItem& i : items) {
        if (i.uid == uid) return &i;
        if (LevelItem* f = find_in(i.contents, uid)) return f;
    }
    return nullptr;
}

bool remove_in(std::vector<LevelItem>& items, std::uint32_t uid) {
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (it->uid == uid) {
            items.erase(it);
            return true;
        }
        if (remove_in(it->contents, uid)) return true;
    }
    return false;
}

std::atomic<std::uint32_t> g_next_uid{1};

}  // namespace

std::uint32_t new_uid() { return g_next_uid++; }

void set_depths(LevelItem& container, std::int32_t depth) {
    for (LevelItem& c : container.contents) {
        c.x = depth + 1;
        c.flags |= kFlagContained;
        set_depths(c, depth + 1);
    }
}

void assign_uids(LevelItem& item) {
    item.uid = new_uid();
    for (LevelItem& c : item.contents) assign_uids(c);
}

Level Level::parse(std::span<const std::uint8_t> entry) {
    Level level;
    std::vector<LevelItem*> path;  // open containers, innermost last
    LevelItem* previous = nullptr;
    for (std::size_t pos = 0; pos + kRecordSize <= entry.size(); pos += kRecordSize) {
        LevelItem item;
        item.uid = new_uid();
        const std::uint16_t x = u16(entry, pos);
        item.y = u16(entry, pos + 2);
        item.z = entry[pos + 4];
        item.shape = u16(entry, pos + 5);
        item.frame = entry[pos + 7];
        item.flags = u16(entry, pos + 8);
        item.quality = u16(entry, pos + 10);
        item.npc_number = entry[pos + 12];
        item.map_number = entry[pos + 13];
        item.next = u16(entry, pos + 14);

        const bool contained = (item.flags & kFlagContained) && x > 0;
        if (contained && x > path.size() && previous) path.push_back(previous);
        while (contained && x < path.size()) path.pop_back();
        if (!contained || path.empty()) {
            item.x = x;
            path.clear();
            level.items.push_back(std::move(item));
            previous = &level.items.back();
        } else {
            item.x = x;
            path.back()->contents.push_back(std::move(item));
            previous = &path.back()->contents.back();
        }
    }
    return level;
}

std::vector<std::uint8_t> Level::serialize() const {
    std::vector<std::uint8_t> out;
    out.reserve(record_count() * kRecordSize);
    for (const LevelItem& item : items) write_tree(out, item);
    return out;
}

std::size_t Level::record_count() const {
    std::size_t n = 0;
    for (const LevelItem& i : items) n += count_tree(i);
    return n;
}

LevelItem* Level::find(std::uint32_t uid) { return find_in(items, uid); }

const LevelItem* Level::find(std::uint32_t uid) const {
    return find_in(const_cast<std::vector<LevelItem>&>(items), uid);
}

std::optional<std::size_t> Level::top_index(std::uint32_t uid) const {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].uid == uid) return i;
    }
    return std::nullopt;
}

const LevelItem* Level::top_of(std::uint32_t uid) const {
    for (const LevelItem& i : items) {
        if (i.uid == uid || find_in(const_cast<std::vector<LevelItem>&>(i.contents), uid)) return &i;
    }
    return nullptr;
}

bool Level::remove(std::uint32_t uid) { return remove_in(items, uid); }

Glob Glob::parse(std::span<const std::uint8_t> entry) {
    Glob glob;
    if (entry.size() < 2) return glob;
    const std::size_t count = u16(entry, 0);
    for (std::size_t i = 0; i < count && 2 + 6 * i + 6 <= entry.size(); ++i) {
        const std::size_t p = 2 + 6 * i;
        glob.pieces.push_back({entry[p], entry[p + 1], entry[p + 2], u16(entry, p + 3), entry[p + 5]});
    }
    return glob;
}

std::vector<std::uint8_t> Glob::serialize() const {
    std::vector<std::uint8_t> out;
    out.reserve(2 + 6 * pieces.size());
    put16(out, static_cast<std::uint32_t>(pieces.size()));
    for (const GlobPiece& p : pieces) {
        out.push_back(p.x);
        out.push_back(p.y);
        out.push_back(p.z);
        put16(out, p.shape);
        out.push_back(p.frame);
    }
    return out;
}

Point3 glob_piece_position(const LevelItem& egg, const GlobPiece& piece) {
    return {(egg.x & ~0x1FF) + 2 * piece.x + 1, (egg.y & ~0x1FF) + 2 * piece.y + 1, (egg.z + piece.z) & 0xFF};
}

FlexEntries FlexEntries::read(const std::string& path) {
    const FlexArchive flex = FlexArchive::open(path);
    FlexEntries out;
    out.entries.resize(flex.count());
    for (std::size_t i = 0; i < flex.count(); ++i) {
        const auto e = flex.entry(i);
        out.entries[i].assign(e.begin(), e.end());
    }
    return out;
}

std::vector<std::uint8_t> FlexEntries::bytes() const {
    FlexWriter writer(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].empty()) writer.add(i, entries[i]);
    }
    return writer.bytes();
}

void FlexEntries::write(const std::string& path) const {
    FlexWriter writer(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].empty()) writer.add(i, entries[i]);
    }
    writer.write(path);
}

}  // namespace crusader::editor
