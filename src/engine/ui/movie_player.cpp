#include "engine/ui/movie_player.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace crusader {

namespace {

constexpr int kScreenWidth = 640;
constexpr int kScreenHeight = 480;
constexpr int kFrameRate = 15;  // ticks per strip per second (1468:3A3D)
constexpr std::uint16_t kFrameSmall = 0x34, kFrameWide = 0x35, kFrameTall = 0x33;
constexpr int kSubtitleX = 20, kSubtitleWidth = 601, kSubtitleHeight = 16, kSubtitleLowest = 470;

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// vport_remap_luma (1268:0157): each pixel to the grey level of its colour.
// @orig 1268:0157
void remap_luma(Surface8& s, const Palette& p) {
    std::array<std::uint8_t, 256> level{};
    const std::array<std::uint8_t, 768> vga = p.to_vga();
    for (std::size_t i = 0; i < 256; ++i) {
        const int l = luminance(vga[3 * i], vga[3 * i + 1], vga[3 * i + 2]) >> 2;
        level[i] = static_cast<std::uint8_t>(l > 7 ? l - 16 : l);
    }
    for (std::uint8_t& px : s.pixels) px = level[px];
}

}  // namespace

// @orig 1468:2F7C
std::string movie_base_name(std::string_view name) {
    std::string up(name);
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (up.rfind("MVA", 0) == 0) {
        if (up.size() > 4 && is_digit(up[4])) return up;
        return "MVA0" + up.substr(3);
    }
    if (up.empty() || !is_digit(up[0]) || (up.size() > 1 && is_digit(up[1]))) return up;
    return "0" + up;
}

// @orig 1468:03E3
MoviePlayer::MoviePlayer(AviMovie movie, MovieKind kind, std::vector<MovieSubtitle> subtitles, bool big,
                         Surface8 screen, const Palette& dac, ShapeCache& gumps, const FontSet& fonts)
    : movie_(std::move(movie)),
      kind_(kind),
      subtitles_(std::move(subtitles)),
      doubled_(big && kind != MovieKind::kOverlay && movie_.width() != kScreenWidth &&
               movie_.height() != kScreenHeight),
      screen_(std::move(screen)),
      dac_(dac),
      kept_palette_(dac),
      gumps_(&gumps),
      fonts_(&fonts),
      strips_(kind == MovieKind::kOverlay ? kOverlayStrips : movie_.strips()),
      frame_buffer_(movie_.width(), movie_.height()) {
    // 1468:2188: centred.
    const int scale = doubled_ ? 2 : 1;
    x_ = (kScreenWidth - movie_.width() * scale) / 2;
    y_ = (kScreenHeight - movie_.height() * scale) / 2;
    // 1468:32CB: subtitles only when their first delay is not 0.
    if (!subtitles_.empty() && subtitles_.front().delay == 0) subtitles_.clear();
    for (const AviMovie::Chunk& c : movie_.chunks()) {
        if (c.id != AviMovie::kAudioChunk) continue;
        const std::vector<std::int8_t> samples = movie_audio_samples(movie_.data(c));
        sound_.insert(sound_.end(), samples.begin(), samples.end());
    }
    if (kind_ == MovieKind::kCutscene) {
        fade_.emplace(dac_, greyscale(dac_), 0);
        period_us_ = 1'000'000 / kFadeRate;
    } else {
        setup_screen();
    }
}

// @orig 1468:3ABC
void MoviePlayer::setup_screen() {
    // 1468:37C4: the frame around the movie, or black under a doubled one.
    if (doubled_) {
        for (int y = y_; y <= y_ + 2 * movie_.height() && y < screen_.height; ++y) {
            for (int x = std::max(x_, 0); x <= x_ + 2 * movie_.width() && x < screen_.width; ++x) screen_.row(y)[x] = 0;
        }
    } else {
        const int w = movie_.width(), h = movie_.height();
        const std::uint16_t shape = h < 0x82 ? (w < 0xA2 ? kFrameSmall : kFrameWide) : h < 0xB6 ? kFrameTall : 0;
        const Shape* s = shape ? gumps_->get(shape) : nullptr;
        if (s && !s->frames().empty()) s->frames()[0].draw(screen_, x_ + 2, y_ - 1);
    }
    if (kind_ != MovieKind::kOverlay) {
        remap_luma(screen_, kept_palette_);
        // 1468:354A: the movie's colours in the middle, grey ramps around.
        std::array<std::uint8_t, 768> vga = dac_.to_vga();
        const int n = movie_.colours();
        if (n != 0) {
            const int first = (256 - n) / 2;
            for (int i = 0; i < first; ++i) {
                const auto low = static_cast<std::uint8_t>(2 + 2 * i);
                const auto high = static_cast<std::uint8_t>(2 * first + 2 * i);
                std::fill_n(vga.begin() + 3 * i, 3, low);
                std::fill_n(vga.begin() + 3 * (first + n + i), 3, high);
            }
            vga[3 * 8] = vga[3 * 8 + 1] = vga[3 * 8 + 2] = 63;
            vga[3 * 9] = 63;
            vga[3 * 9 + 1] = vga[3 * 9 + 2] = 0;
            for (int i = first; i < first + n; ++i) {
                const Rgb& c = movie_.colour_table()[i];
                vga[3 * i] = reduce8(c.r);
                vga[3 * i + 1] = reduce8(c.g);
                vga[3 * i + 2] = reduce8(c.b);
            }
        }
        dac_ = Palette::from_vga(vga);
    }
    // 1468:3904: the audio stream's initial chunks.
    if (movie_.has_audio()) {
        for (std::uint32_t i = 0; i < movie_.audio_initial_frames(); ++i) {
            queue_audio();
            if (!next_chunk()) return;
        }
    }
    phase_ = Phase::kPlay;
    period_us_ = 1'000'000 / static_cast<std::uint32_t>(strips_ * kFrameRate);
    elapsed_us_ = 0;
    playback_due_ = true;
}

// @orig ?: timer callback of the player, not located
void MoviePlayer::advance(std::uint32_t elapsed_us, const std::function<bool(std::uint8_t)>& key_down) {
    // The timer interrupt runs every tick that is due; a long stall does not
    // queue up more than a frame's worth.
    elapsed_us_ = std::min<std::uint64_t>(elapsed_us_ + elapsed_us, std::uint64_t{period_us_} * (strips_ + 1));
    while (!finished_ && period_us_ != 0 && elapsed_us_ >= period_us_) {
        elapsed_us_ -= period_us_;
        if (phase_ == Phase::kFade) {
            if (!fade_->tick(dac_)) {
                fade_.reset();
                setup_screen();
            }
        } else {
            tick(key_down);
        }
    }
}

bool MoviePlayer::take_playback_start() { return std::exchange(playback_due_, false); }

bool MoviePlayer::take_sound_start() { return std::exchange(sound_due_, false); }

bool MoviePlayer::at_video_chunk() const {
    if (chunk_ >= movie_.chunks().size()) return false;
    const std::uint32_t id = movie_.chunks()[chunk_].id;
    return id == AviMovie::kFrameChunk || id == AviMovie::kHalfWidthChunk;
}

// 1468:17B0: the next chunk; the end of the "movi" list stops the movie.
// @orig 1468:17B0
bool MoviePlayer::next_chunk() {
    ++chunk_;
    if (chunk_ >= movie_.chunks().size()) {
        finished_ = true;
        return false;
    }
    return true;
}

// 1468:1EF7: an audio chunk goes to the sound buffer.
// @orig 1468:1EF7
bool MoviePlayer::queue_audio() {
    if (chunk_ >= movie_.chunks().size() || movie_.chunks()[chunk_].id != AviMovie::kAudioChunk) return false;
    ++audio_queued_;
    if (movie_.audio_initial_frames() < audio_queued_) {
        if (!sound_started_) {
            sound_started_ = true;
            sound_due_ = true;
        }
        if (audio_queued_ > 7) audio_queued_ = 0;
    }
    return true;
}

// 1468:1A92: one strip of the current chunk, then onto the screen.
// @orig 1468:1A92
void MoviePlayer::decode(int strip) {
    const AviMovie::Chunk& c = movie_.chunks()[chunk_];
    const bool half = movie_.rrv() && c.id == AviMovie::kHalfWidthChunk;
    decode_movie_strip(movie_.data(c), strip, strips_, half, frame_buffer_);
    const int rows = movie_.height() / strips_;
    const int scale = doubled_ ? 2 : 1;
    for (int y = strip * rows; y < (strip + 1) * rows; ++y) {
        const std::uint8_t* src = frame_buffer_.row(y);
        for (int dy = 0; dy < scale; ++dy) {
            const int sy = y_ + y * scale + dy;
            if (sy < 0 || sy >= screen_.height) continue;
            std::uint8_t* dst = screen_.row(sy);
            for (int x = 0; x < movie_.width(); ++x) {
                for (int dx = 0; dx < scale; ++dx) {
                    const int sx = x_ + x * scale + dx;
                    if (sx >= 0 && sx < screen_.width) dst[sx] = src[x];
                }
            }
        }
    }
}

// 1468:1DA1.
// @orig 1468:1DA1
void MoviePlayer::show_subtitle() {
    if (subtitles_.empty() || subtitle_wait_-- != 0) return;
    Surface8 strip(kSubtitleWidth, kSubtitleHeight, 0);
    const MovieSubtitle* e = subtitle_ < subtitles_.size() ? &subtitles_[subtitle_++] : nullptr;
    subtitle_wait_ = e ? e->delay : 0;
    if (e && e->text) {
        TextWriter writer(*fonts_, kSubtitleFont);
        writer.print(strip, (kSubtitleWidth - 1 - e->width) / 2, 2, *e->text);
    }
    const int height = movie_.height() * (doubled_ ? 2 : 1);
    const int top = y_ + height + 10 < kSubtitleLowest ? y_ + height + 8 : kSubtitleLowest;
    for (int y = 0; y < strip.height && top + y < screen_.height; ++y) {
        std::copy_n(strip.row(y), std::min(strip.width, screen_.width - kSubtitleX), screen_.row(top + y) + kSubtitleX);
    }
}

// 1468:2869.
// @orig 1468:2869
void MoviePlayer::tick(const std::function<bool(std::uint8_t)>& key_down) {
    if (key_down(0x01) || key_down(0x39) || key_down(0x1C)) {
        finished_ = true;
        return;
    }
    const std::int64_t frames = movie_.frame_count();
    if (frames - 1 <= frame_) {
        finished_ = true;
        return;
    }
    if (strip_ == 0) {
        ++frame_;
        // 1468:1929: "Encountered invalid chunk id", nothing more this tick.
        if (!at_video_chunk()) return;
        decode(0);
    } else if (strip_ == strips_ - 1) {
        if (!last_strip_done_) {
            decode(strip_);
            if (!next_chunk()) return;
            last_strip_done_ = true;
        }
        if (!movie_.has_audio()) {
            last_strip_done_ = false;
        } else if (last_strip_done_) {
            if (frame_ < frames - 4) {
                if (queue_audio()) {
                    if (!next_chunk()) return;
                } else if (!at_video_chunk()) {
                    return;  // waits for its audio chunk
                }
            }
            last_strip_done_ = false;
        }
    } else {
        if (strip_ == 1) show_subtitle();
        decode(strip_);
    }
    strip_ = (strip_ + 1) % strips_;
}

}  // namespace crusader
