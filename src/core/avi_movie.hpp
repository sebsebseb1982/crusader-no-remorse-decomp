#pragma once
// The cutscenes, FLICS/*.AVI, as the video player (PLAYER.C, segment 1468)
// reads them: RIFF AVI files with the Origin codecs RRV1, RRV2 and JYV1.
//
// Header (PLAYER_1468_2188): the "avih" chunk, then one "strl" list per
// stream with its "strh" (stream header) and "strf" (format). The video
// stream's format is a BITMAPINFOHEADER and its colour table; the audio
// stream (fourcc JYA1, absent from the overlay movies) holds 8-bit unsigned
// mono PCM at 22050 Hz, 1470 bytes (one frame) per chunk. The "movi" list
// is read chunk after chunk: "00dc" and "00db" are video frames, "01wb"
// audio. The audio stream's initial frames (4) come first.
//
// A video frame is cut into horizontal strips of height / strips rows (the
// rest at the bottom is never drawn): 5 strips for RRV1 and JYV1, 15 for
// RRV2 (13 when played as an overlay; the US overlays are JYV1: 5). The chunk
// starts with one u32 offset
// per strip, from the chunk start. A strip is:
//   u32  n: size of the run codes, 0 when the strip did not change
//   n bytes of run codes, then the pixels to copy
// The run codes (read most significant bit first) give lengths that
// alternate, starting with a skip: skip (keep the previous frame's pixels),
// copy (take the next pixels), skip... Rows follow one another. A length is a
// 4-bit code c, then extra bits: length = kRunBase[c] + the next
// kRunExtraBits[c] bits (no extra bits for codes 0 and 8). Tables at DS:69C2
// and DS:69B2; the bit reader is FUN_1460_0000.
//
// RRV1 and RRV2 store some frames at half width ("00db", PLAYER_1468_04f9):
// lengths count pixel pairs, and each pixel byte is drawn twice (HUGE_12b0_
// 1596). "00dc" frames are at full width (PLAYER_1468_0e51). JYV1 frames are
// never halved.

#include "core/palette.hpp"
#include "core/surface.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace crusader {

// @orig none: FOURCC helper
constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24;
}

class AviMovie {
public:
    static constexpr std::uint32_t kFrameChunk = fourcc("00dc");
    static constexpr std::uint32_t kHalfWidthChunk = fourcc("00db");
    static constexpr std::uint32_t kAudioChunk = fourcc("01wb");
    static constexpr std::uint32_t kAudioRate = 22050;  // the player's sound buffer, fixed (0x5622)

    struct Chunk {
        std::uint32_t id = 0;
        std::size_t offset = 0;  // of the chunk's data in the file
        std::uint32_t size = 0;
    };

    // Throws std::runtime_error when the file cannot be read or is not an AVI.
    static AviMovie load(const std::filesystem::path& path);
    static AviMovie parse(std::vector<std::uint8_t> bytes);

    int width() const { return width_; }
    int height() const { return height_; }
    std::uint32_t codec() const { return codec_; }
    // RRV1 / RRV2 (DS:0048 of the player): "00db" frames are at half width.
    bool rrv() const { return codec_ == fourcc("RRV1") || codec_ == fourcc("RRV2"); }
    // Strips per frame: RRV2 15, others 5 (an RRV2 overlay forces 13).
    int strips() const { return codec_ == fourcc("RRV2") ? 15 : 5; }
    // Video frames in the stream (the video "strh" length).
    std::uint32_t frame_count() const { return frame_count_; }
    // Colours the video uses (biClrImportant): the movie palette puts them
    // at (256 - n) / 2 onwards, the pixels being palette indices.
    int colours() const { return colours_; }
    // The colour table of the format, 8-bit components; black past its end.
    const std::array<Rgb, 256>& colour_table() const { return colour_table_; }
    bool has_audio() const { return has_audio_; }
    // The audio stream's initial frames (strh), read before the video starts.
    std::uint32_t audio_initial_frames() const { return audio_initial_frames_; }

    const std::vector<Chunk>& chunks() const { return chunks_; }
    std::span<const std::uint8_t> data(const Chunk& c) const {
        return std::span<const std::uint8_t>(bytes_).subspan(c.offset, c.size);
    }

private:
    std::vector<std::uint8_t> bytes_;
    int width_ = 0;
    int height_ = 0;
    std::uint32_t codec_ = 0;
    std::uint32_t frame_count_ = 0;
    int colours_ = 0;
    std::array<Rgb, 256> colour_table_{};
    bool has_audio_ = false;
    std::uint32_t audio_initial_frames_ = 0;
    std::vector<Chunk> chunks_;
};

// Decodes strip `strip` (0-based) of a video chunk cut into `strips` strips
// onto `frame` (the movie's width and height), whose other pixels are the
// previous frame. `half_width`: a "00db" chunk of an RRV movie.
void decode_movie_strip(std::span<const std::uint8_t> chunk, int strip, int strips, bool half_width,
                        Surface8& frame);

// The audio chunk as the player hands it to the mixer: signed samples
// (PLAYER_1468_1ef7 subtracts 0x80 from each byte).
std::vector<std::int8_t> movie_audio_samples(std::span<const std::uint8_t> chunk);

}  // namespace crusader
