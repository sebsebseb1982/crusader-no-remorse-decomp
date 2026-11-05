#pragma once
// The video player (PLAYER.C, segment 1468): "Video Player(AVI)", a timed
// process of type 0x240 created and run to its end inside the usecode
// intrinsics (the game is frozen meanwhile):
//   0x0D2, 0x00F  MovieGump::I_playMovieCutscene(name, x, y)  -> 1468:3F77
//   0x093, 0x0AA  I_playMovieCutsceneAlt(name, x, y)           -> 1468:4060
//   0x060         I_playMovieOverlay(name, x, y)               -> 1468:4169
// x and y are not used: the movie is centred.
//
// Opening (1468:03E3): the file name (1468:2F7C, movie_base_name) under the
// "flicpath" directory, with ".TXT" for the subtitles then ".AVI"; nothing
// at all happens when the movie does not exist. Then (1468:3ABC):
//   - the cursor is hidden, every sound effect stopped and the music paused;
//   - the palette on screen is kept (to come back to); a cutscene first fades
//     it to grey (fade_to_greyscale_blocking: each entry to its YIQ luminance,
//     40 Hz, step 1, until reached);
//   - around the movie, unless doubled, a frame from GUMPS.FLX at (x + 2,
//     y - 1): shape 0x34 for movies lower than 130 and narrower than 162
//     pixels, 0x35 for the other ones lower than 130, 0x33 below 182, none
//     above; a doubled movie gets a black rectangle instead (1468:37C4);
//   - except for an overlay, the screen (game view, status bar and frame) is
//     turned to grey levels (vport_remap_luma: index = luminance >> 2, minus
//     16 above 7) and the palette becomes the movie's (1468:354A): its
//     colours at (256 - n) / 2 onwards, and around them two grey ramps that
//     the grey levels use (6-bit 2, 4.. below; 2 * start, + 2.. above);
//     entries 8 and 9 become (63, 63, 63) and (63, 0, 0).
// The audio stream's initial chunks are then read (1468:3904).
//
// Playing (1468:2869, a tick at 15 Hz per strip, i.e. 15 frames a second):
//   - Escape, Space or Enter held stops the movie, as does reaching frame
//     count - 1 (the last frame is never shown) or the end of the chunks;
//   - each tick decodes one strip of the current video chunk, strip 0
//     first moving on to the next frame; a chunk that is not a video chunk
//     there makes the tick do nothing but count a frame;
//   - after the last strip comes the next chunk; then, until 4 frames before
//     the end, one audio chunk is queued and passed. The sound starts (channel
//     0, volume 0x41) once more chunks than the initial ones are queued;
//   - with subtitles (none when the first entry's delay is 0), at strip 1 of
//     each frame (1468:1DA1): when the delay of the shown entry runs out (it
//     starts at 0), the next one, if any, is shown in a
//     601 x 16 black strip at (20, y): font 4, centred, 2 rows down, y 8
//     rows below the movie or 470 at most.
// Big movies ("bigvideo=true", DS:68CE) are drawn twice as wide and high,
// unless 640 wide or 480 high. An overlay keeps the game palette, has no
// subtitles, does not pause the music and, when RRV2 (the French data), uses
// 13 strips; the US overlays are JYV1 with 5.
//
// Closing (1468:3D7A, 3C23): the sound stopped, the music resumed, the keys
// forgotten; except for an overlay, the screen cleared and the kept palette
// back; the cursor shown again.
//
// Not reproduced: the debug view of the read buffer (DS:0905); the read
// buffer itself (the port has the whole file in memory, so the playback
// never waits for the disk); the timed processes, which keep running in
// the original during a movie; the sound is one buffer of every audio chunk
// instead of a ring of 8 chunks filled as the movie goes (the same as long
// as the timing holds).

#include "core/avi_movie.hpp"
#include "core/font.hpp"
#include "core/movie_subtitles.hpp"
#include "core/palette.hpp"
#include "core/surface.hpp"
#include "engine/palette_fader.hpp"
#include "engine/world/render.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crusader {

enum class MovieKind {
    kCutscene,         // 0x0D2, 0x00F: fade to grey first
    kCutsceneNoFade,   // 0x093, 0x0AA
    kOverlay,          // 0x060
};

// 1468:2F7C: the file name without extension, upper case. "MVA<x>" with a
// non-digit 5th character gets a '0' after "MVA" ("mva5b" -> "MVA05B"); a
// digit followed by a non-digit gets a leading '0' ("1a" -> "01A").
std::string movie_base_name(std::string_view name);

class MoviePlayer {
public:
    static constexpr int kSubtitleFont = 4;
    static constexpr int kFadeRate = 40;
    static constexpr int kOverlayStrips = 13;

    // `screen`: the picture on screen when the movie starts (game view and
    // status bar, without the cursor); `dac`: the palette on screen.
    // `subtitles` is empty when they are off or the file is missing.
    MoviePlayer(AviMovie movie, MovieKind kind, std::vector<MovieSubtitle> subtitles, bool big, Surface8 screen,
                const Palette& dac, ShapeCache& gumps, const FontSet& fonts);

    // Real time; `key_down(scancode)` tells whether a key is held.
    void advance(std::uint32_t elapsed_us, const std::function<bool(std::uint8_t)>& key_down);

    bool finished() const { return finished_; }
    MovieKind kind() const { return kind_; }
    const Surface8& screen() const { return screen_; }
    const Palette& palette() const { return dac_; }
    // The palette on screen before the movie, given back at the end.
    const Palette& kept_palette() const { return kept_palette_; }
    // Frames decoded so far (DS:00E8 of the player).
    int frame() const { return frame_; }

    // True once, when the fade is over and the movie starts: the caller then
    // stops the sound effects and pauses the music (not for an overlay).
    bool take_playback_start();
    // The movie's sound: signed 8-bit at AviMovie::kAudioRate, every audio
    // chunk in order. take_sound_start() is true once, when it should start.
    const std::vector<std::int8_t>& sound() const { return sound_; }
    bool take_sound_start();

private:
    enum class Phase { kFade, kPlay };

    void setup_screen();
    void tick(const std::function<bool(std::uint8_t)>& key_down);
    bool at_video_chunk() const;
    void decode(int strip);
    bool next_chunk();
    bool queue_audio();
    void show_subtitle();

    AviMovie movie_;
    MovieKind kind_;
    std::vector<MovieSubtitle> subtitles_;
    bool doubled_;
    Surface8 screen_;
    Palette dac_;
    Palette kept_palette_;
    ShapeCache* gumps_;
    const FontSet* fonts_;

    Phase phase_ = Phase::kFade;
    std::optional<PaletteFade> fade_;
    std::uint32_t period_us_ = 0;
    std::uint64_t elapsed_us_ = 0;
    bool finished_ = false;

    int strips_;
    int x_ = 0, y_ = 0;  // where the movie is on screen
    Surface8 frame_buffer_;
    std::size_t chunk_ = 0;  // current chunk in movie_.chunks()
    int strip_ = 0;          // DS:012E of the player
    int frame_ = 0;          // DS:00E8
    bool last_strip_done_ = false;  // DS:00C4
    std::uint32_t audio_queued_ = 0;  // DS:0EEF
    bool sound_started_ = false;      // DS:0EF1
    bool sound_due_ = false;
    bool playback_due_ = false;
    std::vector<std::int8_t> sound_;

    std::size_t subtitle_ = 0;   // next entry
    std::int64_t subtitle_wait_ = 0;  // DS:00F2
};

}  // namespace crusader
