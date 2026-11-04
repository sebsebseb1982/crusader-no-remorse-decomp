// Sound: the Asylum mixer, the voices (SNDMGR), the sound sources (SNDSRC) and
// the music player, on synthetic data (no game data).

#include "core/amf_module.hpp"
#include "core/sound_flex.hpp"
#include "engine/audio/audio.hpp"
#include "engine/world/teleport.hpp"
#include "engine/world/world.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace crusader;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

SoundSample sample_of(const std::vector<std::int8_t>& pcm, std::uint16_t rate, std::uint32_t loop_start = 0,
                      std::uint32_t loop_length = 0) {
    SoundSample s;
    s.pcm = pcm;
    s.rate = rate;
    s.loop_start = loop_start;
    s.loop_length = loop_length;
    return s;
}

std::vector<std::int16_t> mix(AsylumMixer& m, std::size_t n) {
    std::vector<std::int16_t> out(n);
    m.mix(out.data(), n);
    return out;
}

// Ends the current buffer: a request posted now applies at the next one.
void flush(AsylumMixer& m) { mix(m, m.buffered()); }

void test_mixer() {
    // At the mix rate, one sample per output; volume 0x40 at sound volume
    // 0x40 gives s * 32, negative values towards s + 1.
    const std::vector<std::int8_t> pcm = {100, -100, 50, -1, -128};
    AsylumMixer m;
    m.set_filter(false);
    CHECK(m.buffered() == 0);
    m.play(0, sample_of(pcm, 22050), 0x40, 0, false);
    CHECK(m.busy(0));
    const auto out = mix(m, 7);
    CHECK(out[0] == 3200 && out[1] == -3168 && out[2] == 1600 && out[3] == 0 && out[4] == -4064);
    CHECK(out[5] == 0 && out[6] == 0 && !m.busy(0));  // stopped at its length
    // One buffer per music tick: mix rate / 50 without music.
    CHECK(m.buffered() == 22050 / 50 - 7);

    // A request waits for the next buffer.
    m.play(1, sample_of(pcm, 22050), 0x20, 0, false);
    CHECK(m.busy(1) && mix(m, 1)[0] == 0);
    flush(m);
    // v * g / 64: volume 0x20, or sound volume 0x20.
    CHECK(mix(m, 1)[0] == 1600);
    m.set_sound_volume(0x20);
    m.play(1, sample_of(pcm, 22050), 0x40, 0, false);
    flush(m);
    CHECK(mix(m, 1)[0] == 1600);
    m.set_sound_volume(0x40);
    m.stop(1);
    CHECK(!m.busy(1));
    flush(m);

    // Half rate: each sample twice.
    m.play(2, sample_of(pcm, 11025), 0x40, 0, false);
    const auto half = mix(m, 4);
    CHECK(half[0] == 3200 && half[1] == 3200 && half[2] == -3168 && half[3] == -3168);
    m.stop(2);
    flush(m);

    // A loop longer than 2 goes back by its length; 2 does not loop.
    const std::vector<std::int8_t> ramp = {10, 20, 30, 40};
    m.play(3, sample_of(ramp, 22050, 1, 3), 0x40, 0, true);
    const auto looped = mix(m, 8);
    CHECK(looped[3] == 1280 && looped[4] == 640 && looped[6] == 1280 && looped[7] == 640 && m.busy(3));
    m.play(3, sample_of(ramp, 22050, 1, 2), 0x40, 0, true);
    flush(m);
    CHECK(mix(m, 5)[4] == 0 && !m.busy(3));

    // Oversampling: mean of the sum and the previous one.
    AsylumMixer f;
    f.play(0, sample_of(pcm, 22050), 0x40, 0, false);
    const auto filtered = mix(f, 2);
    CHECK(filtered[0] == 1600 && filtered[1] == 16);
}

// A Flex with an empty tag table and `count` sounds of 1000 samples
// (sound 3 loops).
std::filesystem::path make_flex(int count) {
    std::vector<std::uint8_t> d(0x80 + 8 * (count + 1), 0);
    d[0x54] = static_cast<std::uint8_t>(count + 1);
    auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    for (int n = 1; n <= count; ++n) {
        const std::size_t at = d.size();
        put32(0x80 + 8 * n, static_cast<std::uint32_t>(at));
        put32(0x80 + 8 * n + 4, 0x14 + 1000);
        d.resize(at + 0x14 + 1000, 10);
        d[at] = 'A';
        d[at + 1] = 'S';
        d[at + 2] = 'F';
        d[at + 3] = 'X';
        put32(at + 4, 1000);
        put32(at + 8, 0);
        put32(at + 0xC, n == 3 ? 1000 : 0);
        d[at + 0x12] = 0x22;  // 22050
        d[at + 0x13] = 0x56;
    }
    const auto path = std::filesystem::temp_directory_path() / "crusader_audio_test.flx";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    return path;
}

void test_voices() {
    Audio audio(SoundFlex::open(make_flex(3)));
    SoundManager& v = audio.voices;
    std::vector<SoundManager::Handle> stolen;
    v.set_on_steal([&](SoundManager::Handle h) { stolen.push_back(h); });
    // One-shot sounds take voices 0-7, then the round robin takes them over.
    for (int i = 0; i < 8; ++i) CHECK(v.play(1, 0x40, 0xFF, false) == i);
    CHECK(v.play(2, 0x40, 0xFF, false) == 8 && !v.is_playing(0) && v.is_playing(8));
    CHECK(v.play(2, 0x40, 0xFF, false) == 9 && !v.is_playing(1));
    CHECK(stolen.size() == 2 && stolen[0] == 0 && stolen[1] == 1);
    // Looping sounds on voices 8-11, apart from the others.
    const auto loop = v.play(3, 0x40, 0xFF, true);
    CHECK(v.is_playing(loop) && v.is_playing(2));
    CHECK(v.sfx_of(9) == 2 && v.handle_playing(3) == loop);
    // Once over, the voices are freed; the loop plays on.
    std::vector<std::int16_t> out(1001);
    audio.mixer.mix(out.data(), out.size());
    v.update();
    CHECK(!v.is_playing(2) && v.sfx_of(2) == -1 && v.is_playing(loop));
    v.stop(loop);
    CHECK(!v.is_playing(loop) && !audio.mixer.busy(8));
}

void test_sources() {
    Audio audio(SoundFlex::open(make_flex(3)));
    // Volume from the distance to the player, the camera far away (nothing
    // on screen).
    World w;
    w.set_audio(&audio);
    Item player;
    player.id = 1;
    player.x = 4000;
    player.y = 4000;
    w.add_with_id(player);
    w.set_camera(30000, 30000, 0);
    auto at = [&](std::int32_t dx) {
        Item i;
        i.x = 4000 + dx;
        i.y = 4000;
        return w.add(i).id;
    };
    CHECK(w.get(1) != nullptr);
    CHECK(SoundSources::volume_for(w, at(0x100)) == 0x32 && SoundSources::volume_for(w, at(0x200)) == 0x20 &&
          SoundSources::volume_for(w, at(0x300)) == 0x10 && SoundSources::volume_for(w, at(0x400)) == 8);
    const ObjId far = at(0x300);
    audio.sources.play(w, 1, far);
    CHECK(audio.sources.is_playing(1, far) && !audio.sources.is_playing(1, 1));
    audio.sources.stop(1, far);
    CHECK(!audio.sources.is_playing(1, far));
    // A looping ambient sound plays once, whatever the item.
    audio.sources.play_ambient(w, 3, far);
    audio.sources.play_ambient(w, 3, 1);
    CHECK(audio.sources.is_playing(3, far) && !audio.sources.is_playing(3, 1));
    audio.sources.stop_item(far);
    CHECK(!audio.sources.is_playing(3, far));
    w.set_audio(nullptr);
}

// A teleport to an egg on a given map stops the sounds (SHAPHAND 1098:011F);
// the current-map form (1098:019C) leaves them playing.
void test_teleport_sounds() {
    Audio audio(SoundFlex::open(make_flex(3)));
    World w;
    w.set_audio(&audio);
    Item player;
    player.id = 1;
    player.x = 1000;
    player.y = 1000;
    w.add_with_id(player);
    w.set_camera(1000, 1000, 0);
    Kernel kernel;

    audio.sources.play_ambient(w, 3, 1);
    CHECK(audio.sources.is_playing(3, 1));
    start_teleport_to_egg(kernel, w, w.current_map(), 0x1E, false, false);
    CHECK(audio.sources.is_playing(3, 1));

    start_teleport_to_egg(kernel, w, 1, 0x1E, false, true);
    CHECK(!audio.sources.is_playing(3, 1));
    w.set_audio(nullptr);
}

// A module of speed 2: one pattern played twice, a sample of 1000 samples.
//   row 0: channel 0 plays note 36 (period 3424) with sample 1;
//   row 1: channel 0 slides the period down by 8 a tick (1 02), channel 1
//          sets the tempo to 125 (F 7D);
//   row 2: pattern break to row 10 (D 10).
AmfModule make_module() {
    std::vector<std::uint8_t> d(0xA66 + 0x800 + 1000, 0);
    std::memcpy(d.data(), "ASYLUM Music Format V1.0", 24);
    d[0x20] = 2;
    d[0x21] = 125;
    d[0x22] = 31;
    d[0x23] = 1;
    d[0x24] = 2;
    const std::size_t smp = 0x126;
    d[smp + 23] = 0x40;
    d[smp + 25] = 1000 & 0xFF;
    d[smp + 26] = 1000 >> 8;
    auto put_cell = [&](int row, int ch, std::uint8_t note, std::uint8_t sample, std::uint8_t cmd, std::uint8_t param) {
        const std::size_t at = 0xA66 + static_cast<std::size_t>(row * 8 + ch) * 4;
        d[at] = note;
        d[at + 1] = sample;
        d[at + 2] = cmd;
        d[at + 3] = param;
    };
    put_cell(0, 0, 36, 1, 0, 0);
    put_cell(1, 0, 0, 0, 1, 2);
    put_cell(1, 1, 0, 0, 0xF, 0x7D);
    put_cell(2, 0, 0, 0, 0xD, 0x10);
    std::fill(d.begin() + 0xA66 + 0x800, d.end(), std::uint8_t{10});
    auto m = AmfModule::parse(d);
    CHECK(m.has_value());
    return m ? *m : AmfModule{};
}

void test_music_player() {
    AsylumMusic mu(22050);
    mu.load(std::make_shared<const AmfModule>(make_module()));
    mu.start();
    CHECK(mu.playing() && mu.tick_length() == 441);  // 22050 / (125 * 24 / 60)
    mu.tick();  // speed 2: the first row comes at the second tick
    CHECK(!mu.voices()[0].active);
    mu.tick();
    CHECK(mu.period(0) == 3424 && mu.voices()[0].active && mu.voices()[0].step == 12426);
    CHECK(mu.voices()[0].volume == 0x40 && mu.order_position() == 1 && mu.row() == 1);
    mu.tick();
    mu.tick();  // row 1: the slide runs on the row tick too
    CHECK(mu.period(0) == 3416 && mu.tick_length() == 450);  // 22050 / (125 * 0x66 >> 8)
    mu.tick();
    CHECK(mu.period(0) == 3408);
    mu.tick();  // row 2: the break
    CHECK(mu.period(0) == 3408 && mu.row() == 64);
    mu.tick();
    mu.tick();  // next order, counted from row 10 but read from row 0
    CHECK(mu.order_position() == 2 && mu.row() == 11 && mu.period(0) == 3424);

    // Through the mixer: the sample (10) at full volume, music volume 0x20.
    AsylumMixer m;
    m.set_filter(false);
    m.play_music(std::make_shared<const AmfModule>(make_module()));
    CHECK(m.music_playing());
    auto out = mix(m, 441 * 2);
    CHECK(out[0] == 0 && out[441] == 160);  // 10 * (0x40 * 0x20 / 0x40) / 2
    m.pause_music(true);
    flush(m);
    const auto paused = mix(m, 4);
    CHECK(paused[0] == 0 && m.music_playing());
    m.stop_music();
    CHECK(!m.music_playing());
}
}  // namespace

int main() {
    test_mixer();
    test_voices();
    test_sources();
    test_teleport_sounds();
    test_music_player();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("audio tests passed\n");
    return 0;
}
