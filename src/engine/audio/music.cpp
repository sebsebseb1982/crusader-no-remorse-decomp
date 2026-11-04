#include "engine/audio/music.hpp"

#include "core/amf_module.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace crusader {

namespace {

// The files are "M01.AMF" on the CD: look the name up without the case, for
// case-sensitive file systems (Linux).
// @orig none: file lookup without the case, port addition
std::filesystem::path find_module(const std::filesystem::path& dir, const std::string& name) {
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string wanted = lower(name + ".amf");
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (lower(entry.path().filename().string()) == wanted) return entry.path();
    }
    return dir / (name + ".amf");
}

constexpr int kPlaying = 1;
constexpr int kPaused = 8;
constexpr int kM16 = 0x10;

}  // namespace

Music::Music(AsylumMixer& mixer, std::filesystem::path sound_dir) : mixer_(mixer), dir_(std::move(sound_dir)) {}

// @orig 1200:00C8
std::optional<std::string> Music::file_name(int track) const {
    switch (track) {
    case kM16:
        return std::string("m16") + m16_;
    case kCredits:
        return "cred";
    case kMenu:
        return "menu";
    default:
        if (track >= 1 && track <= 15) return (track < 10 ? "m0" : "m") + std::to_string(track);
        return std::nullopt;
    }
}

void Music::pause() {
    if (flags_ & kPaused) return;
    flags_ |= kPaused;
    mixer_.pause_music(true);
}

void Music::resume() {
    if (!(flags_ & kPaused)) return;
    mixer_.pause_music(false);
    flags_ &= ~kPaused;
}

void Music::stop() {
    if (!(flags_ & kPlaying)) return;
    flags_ = 0;
    mixer_.stop_music();
}

// @orig 1200:0143
void Music::play(int track) {
    stop();
    if (track == kM16) m16_ = m16_ == 'c' ? 'a' : static_cast<char>(m16_ + 1);
    const auto name = file_name(track);
    // The original builds the path whatever the file; a missing one is
    // silence here.
    std::optional<AmfModule> module;
    if (name) module = AmfModule::load(find_module(dir_, *name));
    if (!module) {
        flags_ = 0;
        current_ = 0;
        return;
    }
    pause();
    mixer_.play_music(std::make_shared<const AmfModule>(std::move(*module)));
    flags_ |= kPlaying;
    current_ = track;
    resume();
}

void Music::push(int track) {
    if (current_ != 0 && depth_ < static_cast<int>(stack_.size())) stack_[static_cast<std::size_t>(depth_++)] = current_;
    play(track);
}

void Music::pop() {
    if (depth_ == 0) return;
    play(stack_[static_cast<std::size_t>(--depth_)]);
}

void Music::reset() {
    depth_ = 0;
    play(0);
}

void Music::restore(int track, int flags) {
    current_ = track;
    flags_ = flags;
    play(track);
}

}  // namespace crusader
