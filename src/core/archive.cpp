#include "core/archive.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <utility>

namespace crusader {

namespace {

// --- DEFLATE (the decoder of RFC 1951, in the way of zlib's "puff")

constexpr int kMaxBits = 15;
constexpr int kLiteralCodes = 288, kDistanceCodes = 30;

struct Huffman {
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::array<std::uint16_t, kLiteralCodes> symbol{};
};

// The canonical code of `lengths` (one per symbol, 0 for none); false when
// over-subscribed. An incomplete code is accepted: a bit string with no symbol
// fails when it is read.
bool build(Huffman& h, const std::uint8_t* lengths, int n) {
    h.count.fill(0);
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left = (left << 1) - h.count[len];
        if (left < 0) return false;
    }
    std::array<std::uint16_t, kMaxBits + 2> offsets{};
    for (int len = 1; len <= kMaxBits; ++len) offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + h.count[len]);
    for (int i = 0; i < n; ++i) {
        if (lengths[i] != 0) h.symbol[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
    }
    return true;
}

class Inflater {
public:
    Inflater(const std::uint8_t* data, std::size_t size, std::size_t max_out)
        : data_(data), size_(size), max_out_(max_out) {}

    bool run() {
        for (;;) {
            const int last = bits(1);
            const int type = bits(2);
            if (failed_) return false;
            bool ok = false;
            if (type == 0) ok = stored();
            else if (type == 1) ok = fixed();
            else if (type == 2) ok = dynamic();
            if (!ok || failed_) return false;
            if (last) return true;
        }
    }
    std::vector<std::uint8_t>& out() { return out_; }
    // Whole bytes read, a partly read one included (bits() reads a byte only
    // when it needs it).
    std::size_t consumed() const { return pos_; }

private:
    // `need` bits, least significant first (0 and a failure at the end of the data).
    int bits(int need) {
        std::uint32_t value = bit_buffer_;
        while (bit_count_ < need) {
            if (pos_ >= size_) {
                failed_ = true;
                return 0;
            }
            value |= static_cast<std::uint32_t>(data_[pos_++]) << bit_count_;
            bit_count_ += 8;
        }
        bit_buffer_ = value >> need;
        bit_count_ -= need;
        return static_cast<int>(value & ((1u << need) - 1));
    }

    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            if (failed_) return -1;
            const int count = h.count[len];
            if (code - count < first) return h.symbol[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool stored() {
        bit_buffer_ = 0;
        bit_count_ = 0;
        if (pos_ + 4 > size_) return false;
        const std::size_t len = data_[pos_] | (data_[pos_ + 1] << 8);
        const std::size_t inverse = data_[pos_ + 2] | (data_[pos_ + 3] << 8);
        pos_ += 4;
        if (len != (~inverse & 0xFFFF) || pos_ + len > size_ || out_.size() + len > max_out_) return false;
        out_.insert(out_.end(), data_ + pos_, data_ + pos_ + len);
        pos_ += len;
        return true;
    }

    bool codes(const Huffman& lit, const Huffman& dist) {
        static constexpr std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                        33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lit);
            if (symbol < 0) return false;
            if (symbol < 256) {
                if (out_.size() >= max_out_) return false;
                out_.push_back(static_cast<std::uint8_t>(symbol));
            } else if (symbol == 256) {
                return true;
            } else {
                symbol -= 257;
                if (symbol >= 29) return false;
                const std::size_t length = kLengthBase[symbol] + static_cast<std::size_t>(bits(kLengthExtra[symbol]));
                const int d = decode(dist);
                if (d < 0 || d >= kDistanceCodes) return false;
                const std::size_t distance = kDistBase[d] + static_cast<std::size_t>(bits(kDistExtra[d]));
                if (failed_ || distance > out_.size() || out_.size() + length > max_out_) return false;
                // Byte by byte: the copy may overlap what it writes.
                std::size_t from = out_.size() - distance;
                for (std::size_t i = 0; i < length; ++i) out_.push_back(out_[from++]);
            }
        }
    }

    bool fixed() {
        static const std::pair<Huffman, Huffman> tables = [] {
            std::uint8_t lengths[kLiteralCodes];
            int i = 0;
            for (; i < 144; ++i) lengths[i] = 8;
            for (; i < 256; ++i) lengths[i] = 9;
            for (; i < 280; ++i) lengths[i] = 7;
            for (; i < kLiteralCodes; ++i) lengths[i] = 8;
            std::pair<Huffman, Huffman> t;
            build(t.first, lengths, kLiteralCodes);
            std::uint8_t five[kDistanceCodes];
            std::fill(std::begin(five), std::end(five), std::uint8_t{5});
            build(t.second, five, kDistanceCodes);
            return t;
        }();
        return codes(tables.first, tables.second);
    }

    bool dynamic() {
        static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (failed_ || nlen > 286 || ndist > kDistanceCodes) return false;
        std::uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<std::uint8_t>(bits(3));
        if (failed_) return false;
        Huffman length_code;
        if (!build(length_code, lengths, 19)) return false;
        int index = 0;
        std::uint8_t table[320] = {};
        while (index < nlen + ndist) {
            const int symbol = decode(length_code);
            if (symbol < 0) return false;
            if (symbol < 16) {
                table[index++] = static_cast<std::uint8_t>(symbol);
                continue;
            }
            int repeat, value = 0;
            if (symbol == 16) {
                if (index == 0) return false;
                value = table[index - 1];
                repeat = 3 + bits(2);
            } else if (symbol == 17) {
                repeat = 3 + bits(3);
            } else {
                repeat = 11 + bits(7);
            }
            if (failed_ || index + repeat > nlen + ndist) return false;
            while (repeat-- > 0) table[index++] = static_cast<std::uint8_t>(value);
        }
        if (table[256] == 0) return false;  // no end of block
        Huffman lit, dist;
        if (!build(lit, table, nlen) || !build(dist, table + nlen, ndist)) return false;
        return codes(lit, dist);
    }

    const std::uint8_t* data_;
    std::size_t size_, max_out_;
    std::size_t pos_ = 0;
    std::uint32_t bit_buffer_ = 0;
    int bit_count_ = 0;
    bool failed_ = false;
    std::vector<std::uint8_t> out_;
};

std::uint32_t le16(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8; }
std::uint32_t le32(const std::uint8_t* p) { return le16(p) | le16(p + 2) << 16; }

// --- zip

ArchiveResult fail(std::string why) { return {false, std::move(why)}; }

ArchiveResult read_zip(const std::vector<std::uint8_t>& bytes, const std::function<bool(ArchiveEntry&)>& each) {
    const std::uint8_t* b = bytes.data();
    const std::size_t n = bytes.size();
    if (n < 22) return fail("zip: too short");
    // The end of central directory record, the last 22 bytes unless a comment follows.
    std::size_t eocd = n;
    const std::size_t lowest = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (std::size_t at = n - 22 + 1; at-- > lowest;) {
        if (le32(b + at) == 0x06054b50) {
            eocd = at;
            break;
        }
    }
    if (eocd == n) return fail("zip: no end of central directory");
    const std::size_t count = le16(b + eocd + 10), directory_size = le32(b + eocd + 12), directory_at = le32(b + eocd + 16);
    if (count == 0xFFFF || directory_size == 0xFFFFFFFFu || directory_at == 0xFFFFFFFFu) return fail("zip: zip64 is not supported");
    if (count > kMaxArchiveEntries) return fail("zip: too many entries");
    if (directory_at > n || directory_size > n - directory_at) return fail("zip: bad central directory");

    std::size_t at = directory_at, unpacked = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (at + 46 > directory_at + directory_size || le32(b + at) != 0x02014b50) return fail("zip: bad central entry");
        const unsigned made_by = le16(b + at + 4), flags = le16(b + at + 8), method = le16(b + at + 10);
        const std::uint32_t crc = le32(b + at + 16);
        const std::size_t packed = le32(b + at + 20), size = le32(b + at + 24);
        const std::size_t name_length = le16(b + at + 28), extra = le16(b + at + 30), comment = le16(b + at + 32);
        const std::uint32_t attributes = le32(b + at + 38);
        const std::size_t local = le32(b + at + 42);
        if (at + 46 + name_length + extra + comment > directory_at + directory_size) return fail("zip: bad central entry");
        ArchiveEntry entry;
        entry.path.assign(reinterpret_cast<const char*>(b + at + 46), name_length);
        at += 46 + name_length + extra + comment;
        entry.directory = !entry.path.empty() && entry.path.back() == '/';
        entry.executable = (made_by >> 8) == 3 && ((attributes >> 16) & 0111) != 0;
        if (!entry.directory) {
            if (flags & 1) return fail("zip: encrypted entry");
            if (method != 0 && method != 8) return fail("zip: unsupported method");
            unpacked += size;
            if (unpacked > kMaxUnpacked) return fail("zip: too big");
            if (local > n || n - local < 30 || le32(b + local) != 0x04034b50) return fail("zip: bad local header");
            const std::size_t data_at = local + 30 + le16(b + local + 26) + le16(b + local + 28);
            if (data_at > n || packed > n - data_at) return fail("zip: entry out of the file");
            if (method == 0) {
                if (packed != size) return fail("zip: bad stored size");
                entry.data.assign(b + data_at, b + data_at + packed);
            } else {
                auto out = inflate_raw(b + data_at, packed, size);
                if (!out || out->size() != size) return fail("zip: cannot inflate " + entry.path);
                entry.data = std::move(*out);
            }
            if (crc32(entry.data.data(), entry.data.size()) != crc) return fail("zip: bad checksum of " + entry.path);
        }
        if (!each(entry)) return fail("stopped");
    }
    return {true, {}};
}

// --- tar

// An octal field, NUL or space ended; none when it holds anything else.
std::optional<std::uint64_t> octal(const std::uint8_t* p, std::size_t n) {
    std::uint64_t value = 0;
    std::size_t i = 0;
    while (i < n && p[i] == ' ') ++i;
    bool any = false;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) {
        value = value * 8 + (p[i] - '0');
        any = true;
    }
    for (; i < n; ++i) {
        if (p[i] != 0 && p[i] != ' ') return std::nullopt;
    }
    return any ? std::optional<std::uint64_t>(value) : std::optional<std::uint64_t>(0);
}

std::string cstring(const std::uint8_t* p, std::size_t n) {
    const void* end = std::memchr(p, 0, n);
    return std::string(reinterpret_cast<const char*>(p), end ? static_cast<std::size_t>(static_cast<const std::uint8_t*>(end) - p) : n);
}

// The "path" of a pax extended header ("<length> <key>=<value>\n" records).
std::string pax_path(const std::uint8_t* p, std::size_t n) {
    std::size_t at = 0;
    while (at < n) {
        std::size_t length = 0, i = at;
        while (i < n && p[i] >= '0' && p[i] <= '9') length = length * 10 + (p[i++] - '0');
        if (i >= n || p[i] != ' ' || length == 0 || at + length > n || length < (i - at) + 2) break;
        const std::string record(reinterpret_cast<const char*>(p + i + 1), length - (i - at) - 2);
        if (record.rfind("path=", 0) == 0) return record.substr(5);
        at += length;
    }
    return {};
}

ArchiveResult read_tar(const std::vector<std::uint8_t>& tar, const std::function<bool(ArchiveEntry&)>& each) {
    const std::uint8_t* b = tar.data();
    const std::size_t n = tar.size();
    std::size_t at = 0, count = 0;
    std::string long_name;  // from a GNU 'L' or a pax 'x' header, for the next entry
    while (at + 512 <= n) {
        const std::uint8_t* h = b + at;
        if (std::all_of(h, h + 512, [](std::uint8_t c) { return c == 0; })) return {true, {}};
        const auto stored = octal(h + 148, 8);
        std::uint64_t sum = 0;
        for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (!stored || *stored != sum) return fail("tar: bad header checksum");
        const auto size = octal(h + 124, 12);
        if (!size || *size > kMaxUnpacked || at + 512 + *size > n) return fail("tar: bad size");
        const auto mode = octal(h + 100, 8);
        const char type = static_cast<char>(h[156]);
        const std::uint8_t* data = h + 512;
        at += 512 + ((static_cast<std::size_t>(*size) + 511) & ~static_cast<std::size_t>(511));

        if (type == 'L') {
            long_name = cstring(data, static_cast<std::size_t>(*size));
            continue;
        }
        if (type == 'x') {
            if (std::string path = pax_path(data, static_cast<std::size_t>(*size)); !path.empty()) long_name = path;
            continue;
        }
        if (type != '0' && type != '\0' && type != '5') {  // links, devices, 'g', 'K'...: not extracted
            long_name.clear();
            continue;
        }
        if (++count > kMaxArchiveEntries) return fail("tar: too many entries");
        ArchiveEntry entry;
        if (!long_name.empty()) {
            entry.path = std::move(long_name);
            long_name.clear();
        } else {
            entry.path = cstring(h, 100);
            if (std::memcmp(h + 257, "ustar", 5) == 0) {
                if (const std::string prefix = cstring(h + 345, 155); !prefix.empty()) entry.path = prefix + "/" + entry.path;
            }
        }
        entry.directory = type == '5' || (!entry.path.empty() && entry.path.back() == '/');
        entry.executable = mode && (*mode & 0111) != 0;
        if (!entry.directory) entry.data.assign(data, data + static_cast<std::size_t>(*size));
        if (!each(entry)) return fail("stopped");
    }
    return fail("tar: no end marker");
}

}  // namespace

// @orig none: port's self-update
std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// @orig none: port's self-update
std::optional<std::vector<std::uint8_t>> inflate_raw(const std::uint8_t* data, std::size_t size, std::size_t max_out,
                                                     std::size_t* consumed) {
    Inflater inflater(data, size, max_out);
    if (!inflater.run()) return std::nullopt;
    if (consumed) *consumed = inflater.consumed();
    return std::move(inflater.out());
}

// @orig none: port's self-update
std::optional<std::vector<std::uint8_t>> gunzip(const std::uint8_t* data, std::size_t size, std::size_t max_out) {
    if (size < 18 || data[0] != 0x1F || data[1] != 0x8B || data[2] != 8) return std::nullopt;
    const unsigned flags = data[3];
    std::size_t at = 10;
    if (flags & 4) {  // FEXTRA
        if (at + 2 > size) return std::nullopt;
        at += 2 + le16(data + at);
    }
    for (const unsigned bit : {8u, 16u}) {  // FNAME, FCOMMENT: zero ended
        if (!(flags & bit)) continue;
        while (at < size && data[at] != 0) ++at;
        ++at;
    }
    if (flags & 2) at += 2;  // FHCRC
    if (at + 8 > size) return std::nullopt;
    std::size_t used = 0;
    auto out = inflate_raw(data + at, size - at, max_out, &used);
    if (!out || at + used + 8 > size) return std::nullopt;
    const std::uint8_t* trailer = data + at + used;
    if (le32(trailer) != crc32(out->data(), out->size()) || le32(trailer + 4) != (out->size() & 0xFFFFFFFFu)) {
        return std::nullopt;
    }
    return out;
}

// @orig none: port's self-update
ArchiveResult read_archive(const std::vector<std::uint8_t>& bytes, ArchiveFormat format,
                           const std::function<bool(ArchiveEntry&)>& each) {
    if (format == ArchiveFormat::kZip) return read_zip(bytes, each);
    const auto tar = gunzip(bytes.data(), bytes.size(), kMaxUnpacked);
    if (!tar) return fail("tar.gz: cannot gunzip");
    return read_tar(*tar, each);
}

}  // namespace crusader
