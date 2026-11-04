#include "game/game.hpp"

#include "engine/usecode/audio_intrinsics.hpp"
#include "engine/usecode/intrinsic_numbering.hpp"
#include "engine/usecode/item_intrinsics.hpp"
#include "engine/usecode/movie_intrinsics.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/usecode/npc_intrinsics.hpp"
#include "engine/usecode/uc_process.hpp"
#include "engine/world/actor_create.hpp"
#include "engine/world/aim_mode.hpp"
#include "engine/world/attack.hpp"
#include "engine/world/auto_firer.hpp"
#include "engine/world/gravity.hpp"
#include "core/data_info.hpp"
#include "core/dtable.hpp"
#include "core/language.hpp"
#include "engine/palette_fader.hpp"
#include "engine/world/projectile.hpp"
#include "engine/ui/computer_gump.hpp"
#include "engine/ui/floppy_icon.hpp"
#include "engine/ui/gump_keys.hpp"
#include "engine/ui/keypad_gump.hpp"
#include "engine/ui/weasel_gump.hpp"
#include "engine/ui/shape_dialog.hpp"
#include "engine/world/item_selection.hpp"
#include "engine/world/player_commands.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/inventory.hpp"
#include "engine/world/item_anim.hpp"
#include "engine/world/item_damage.hpp"
#include "engine/world/palette_cycle.hpp"
#include "engine/world/pickup_list.hpp"
#include "engine/world/teleport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace crusader {

namespace {

// KDAEMON.C: 45 Hz timed process, at every level, repeating held keys.
class KeyDaemon : public TimedProcess {
public:
    explicit KeyDaemon(Keyboard& keyboard) : keyboard_(keyboard) {
        set_rate(Keyboard::kDaemonRate);
        set_runs_at_any_level(true);
    }

protected:
    void tick() override { keyboard_.repeat(kernel().ticks()); }

private:
    Keyboard& keyboard_;
};

// MOUSECTL.C: the mouse handler is also a 60 Hz timed process, at every
// level, that sends the held-back clicks (mouse_queue.hpp).
class MouseDaemon : public TimedProcess {
public:
    explicit MouseDaemon(MouseQueue& queue) : queue_(queue) {
        set_rate(Kernel::kTicksPerSecond);
        set_runs_at_any_level(true);
    }

protected:
    void tick() override { queue_.tick(); }

private:
    MouseQueue& queue_;
};

// Stands for the modal menu or message box in the kernel: raised one level,
// it freezes the game's processes until it terminates (see Game::open_menu).
class ModalProcess : public Process {
protected:
    void run() override {}
};

// Key daemon rate and repeat delay while the menu is open (IRGUMP_13d0_0000).
constexpr std::uint32_t kMenuDaemonRate = 10;
constexpr std::uint32_t kMenuRepeatDelay = 20;

// The options screen and the load screen of the menu, opened by F2 / F8.
class OptionsGump : public ModalGump {
public:
    OptionsGump(ShapeCache& gumps, GameConfig& config, Audio* audio, int& focus, const FontSet& fonts)
        : s_(gumps, config, audio, focus, &fonts) {}
    void draw(Surface8& screen) const override { s_.draw(screen); }
    void key(std::uint16_t code) override { s_.key(code); }
    void mouse_down(int x, int y) override { s_.mouse_down(x, y); }
    void mouse_move(int x, int y) override { s_.mouse_move(x, y); }
    void mouse_up(int x, int y) override { s_.mouse_up(x, y); }
    void update(std::uint32_t ticks) override { s_.update(ticks); }
    bool waiting_for_pad() const override { return s_.waiting_for_pad(); }
    void pad_button(int button) override { s_.pad_button(button); }
    bool closed() const override { return s_.closed(); }

private:
    OptionsScreen s_;
};

class LoadGump : public ModalGump {
public:
    LoadGump(ShapeCache& gumps, const FontSet& fonts, const LoadScreen::Names& names, int& selected,
             LoadScreen::Mode mode = LoadScreen::Mode::kLoad)
        : s_(gumps, fonts, names, selected, mode) {}
    void draw(Surface8& screen) const override { s_.draw(screen); }
    void key(std::uint16_t code) override { s_.key(code); }
    void mouse_down(int x, int y) override { s_.mouse_down(x, y); }
    bool closed() const override { return s_.result().has_value(); }
    int result() const override { return s_.result().value_or(0); }
    int slot() const { return s_.slot(); }
    const std::string& name() const { return s_.name(); }

private:
    LoadScreen s_;
};

constexpr std::size_t kSavedGlobals = 0x42;

// The autosave: the floppy disk stays 2.5 s, doubled, 8 pixels from the corner; a request waits
// 1 s (kernel ticks, 60 a second) for the scripts to settle.
constexpr std::uint32_t kAutosaveIconUs = 2'500'000;
constexpr std::uint32_t kAutosaveDelayTicks = 60;
constexpr int kFloppyScale = 2, kFloppyMargin = 8;

std::vector<std::uint8_t> load_damage_table(
const std::filesystem::path& data) {
    const auto flex = FlexArchive::open(data / "STATIC/DAMAGE.FLX");
    const auto entry = flex.entry(0);
    return {entry.begin(), entry.end()};
}

}  // namespace

// @orig none: loads the data and builds the port's subsystems
Game::Game(const std::filesystem::path& data, const std::filesystem::path& saves)
    : shapes_flex_(FlexArchive::open(data / "STATIC/SHAPES.FLX")),
      globs_(FlexArchive::open(data / "STATIC/GLOB.FLX")),
      fixed_(FlexArchive::open(data / "STATIC/FIXED.DAT")),
      typeflags_(load_typeflags(data / "STATIC/TYPEFLAG.DAT")),
      trig_(TrigTables::load(data / "STATIC/TRIG.DAT")),
      damage_(load_damage_table(data)),
      anims_(AnimDat::load(data / "STATIC/ANIM.DAT")),
      weapon_overlay_(WeaponOverlay::load(data / "STATIC/WPNOVLAY.DAT")),
      palette_(Palette::load(data / "STATIC/GAMEPAL.PAL")),
      usecode_(UsecodeFile::from_flex(FlexArchive::open(data / "USECODE/EUSECODE.FLX"))),
      shapes_(shapes_flex_, ShapeLayout::World),
      gumps_flex_(FlexArchive::open(data / "STATIC/GUMPS.FLX")),
      gumps_(gumps_flex_, ShapeLayout::Flat2D),
      fonts_(FontSet::load(data / "STATIC/FONTS.FLX")),
      xform_(XformTable::load(data / "STATIC/XFORMPAL.DAT")),
      npc_types_(dtable::NpcTypes::load(data / "STATIC/DTABLE.FLX")),
      status_bar_(gumps_, fonts_, xform_, npc_types_),
      pickup_area_(gumps_, fonts_, xform_),
      menu_palette_(Palette::load(data / "STATIC/MISC.PAL")),
      difficulty_palette_(Palette::load(data / "STATIC/DIFF.PAL")),
      cursor_(MouseCursor::load(data / "STATIC/MOUSE.SHP")),
      data_(data),
      save_dir_(saves.empty() ? data / "SAVEGAME" : saves),
      config_(GameConfig::load(data / "CRUSADER.CFG")),
      save_names_(LoadScreen::read_names(saves.empty() ? data / "SAVEGAME" : saves)),
      credits_data_(CreditsData::load(data / "STATIC")),
      help_pictures_(HelpScreen::Pictures::load(data / "STATIC")) {
    // The US data has no CRUSADER.CFG (the installer writes it): without one,
    // take the installer's choices, the doubled videos and the subtitles.
    if (!std::filesystem::exists(data / "CRUSADER.CFG")) config_.big_video = config_.subtitles = true;
    // dsfx_init: without SOUND.FLX the game runs silent, as with "sound=off".
    if (std::filesystem::exists(data / "SOUND/SOUND.FLX")) {
        audio_ = std::make_unique<Audio>(SoundFlex::open(data / "SOUND/SOUND.FLX"), data / "SOUND");
    }
    const DataInfo info = detect_data_info(data);
    // Without a known version, the usecode's intrinsics cannot be numbered:
    // played with the French numbers, other data calls the wrong routines.
    if (!info.known()) {
        throw std::runtime_error("unrecognised game data in '" + data.string() +
                                 "': supported versions are French 1.08, US 1.23 and English 1.01");
    }
    data_version_ = info.version;
    data_language_ = info.language;
    intrinsic_numbering_ = intrinsic_numbering(info);
    set_text_language(info.text_language());
    set_config(config_);
    reset_world();
}

// @orig 12E0:06BF

// WPNCACHE_1160_005f: a firing frame of an actor's animation. The object that
// fires is the active inventory item when the actor's last action is 13 and
// it has one, else its active weapon. The player's weapon of fire type 6 (909)
// fires through the AutoFirer while the fire key is held: the frame starts it
// when none runs; while it runs, only its own shots fire, and a frame seen
// with the key released ends it. Then Item::I_fireWeapon with the object's
// fire type, the muzzle offset (x, y * 16, z * 8), the actor's direction
// (record +0xC) and aiming, and the object's sound (DTABLE +2) on the actor's
// voice.
// @orig 1160:005F
void Game::fire_frame(ObjId actor, int a, int b, int z) {
    ++shots_fired_;
    const auto r = world_->npc_record(actor);
    const std::uint16_t last = r.empty() ? 0 : static_cast<std::uint16_t>(r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8);
    const ObjId item = world_->active_inventory_item(actor);
    const Item* source = world_->get(item != 0 && last == 13 ? item : world_->active_weapon(actor));
    if (!source) return;
    const int type = dtable::weapon(source->shape).fire_type();
    if (actor == 1 && type == 6) {
        if (world_->state.auto_firer != 0) {
            if (!world_->state.fire_held) stop_auto_firer(*kernel_, *world_);
            if (!world_->state.auto_firing) return;
        } else if (world_->state.fire_held) {
            world_->state.auto_firer = kernel_->add(std::make_unique<AutoFirer>(
                *world_, anims_, [this](ObjId npc, int ma, int mb, int mz) { fire_frame(npc, ma, mb, mz); }));
        }
    }
    const int dir = r.empty() ? 0 : r[npc::kDirection];
    fire_weapon(*kernel_, *world_, anims_, actor, a * 16, b * 16, z * 8, dir, type, true);
    const std::uint16_t sound = dtable::weapon(source->shape).sound();
    if (sound != 0 && audio_) audio_->sources.retrigger(*world_, sound, actor);
}

// @orig none: applying the port's settings
void Game::set_config(GameConfig config) {
    config_ = std::move(config);
    // ass_init: the volumes and the filter of the configuration.
    if (audio_) {
        audio_->mixer.set_music_volume(config_.music_volume);
        audio_->mixer.set_sound_volume(config_.sound_volume);
        audio_->mixer.set_filter(config_.filter);
    }
}

// @orig 1028:03F6
void Game::reset_world() {
    // The sounds of the previous world go with it.
    if (audio_) {
        audio_->stop_all();
        audio_->sources.clear();
    }
    machine_.reset();
    // DS:2889 is not part of a game.
    const bool target_designation = world_ ? world_->state.target_designation : true;
    world_.reset();
    kernel_ = std::make_unique<Kernel>();
    machine_ = std::make_unique<UCMachine>(*kernel_, usecode_);
    machine_->set_intrinsic_numbering(intrinsic_numbering_);
    world_ = std::make_unique<World>(typeflags_);
    world_->state.target_designation = target_designation;
    world_->set_damage_table(damage_);
    world_->state.game_palette = palette_;
    world_->state.screen_palette = palette_;
    world_->set_globs(&globs_);
    world_->set_fixed(&fixed_);
    world_->set_trig(&trig_);
    world_->set_audio(audio_.get());
    world_->set_frame_lookup([this](std::uint16_t shape, std::uint16_t frame) -> std::optional<FrameBox> {
        const Shape* s = shapes_.get(shape);
        if (!s || frame >= s->frames().size()) return std::nullopt;
        const ShapeFrame& f = s->frames()[frame];
        return FrameBox{f.width, f.height, f.xoff, f.yoff};
    });
    register_item_intrinsics(*machine_, *world_);
    register_world_intrinsics(*machine_, *world_, &anims_);
    register_audio_intrinsics(*machine_, *world_);
    register_npc_intrinsics(*machine_, *world_, anims_, &npc_types_);
    register_movie_intrinsics(*machine_, [this](const std::string& name, MovieKind kind) { play_movie(name, kind); });
    // 13E8:00A8 (unnamed in ScummVM): the keyboard queue and the mouse clicks
    // kept by the mouse handler (DS:8A80..8A84) are forgotten.
    machine_->set_intrinsic(0x0D3, [this](UCMachine&, UCProcess&, std::span<const std::uint8_t>) -> std::uint32_t {
        keyboard_.reset();
        return 0;
    });
    // ComputerGump::I_readComputer(text).
    machine_->set_intrinsic(0x0FE, [this](UCMachine& machine, UCProcess&, std::span<const std::uint8_t> a) -> std::uint32_t {
        if (a.size() < 4 || (a[2] | a[3] << 8) != UCMachine::kSegString) return 0;
        if (const std::string* text = machine.heap().string(static_cast<std::uint16_t>(a[0] | a[1] << 8))) {
            read_computer(*text);
        }
        return 0;
    });
    // KeypadGump::I_showKeypad(code): the result is set once the keypad closes.
    machine_->set_intrinsic(0x0C5, [this](UCMachine&, UCProcess& p, std::span<const std::uint8_t> a) -> std::uint32_t {
        if (a.size() < 2) return 0;
        show_keypad(static_cast<std::int16_t>(a[0] | a[1] << 8), p.pid());
        return 0;
    });
    // WeaselGump::I_showWeaselGump(level): the calling process goes on once
    // the shop is closed.
    machine_->set_intrinsic(0x12D, [this](UCMachine&, UCProcess&, std::span<const std::uint8_t> a) -> std::uint32_t {
        if (a.size() < 2) return 0;
        show_weasel(static_cast<std::int16_t>(a[0] | a[1] << 8));
        return 0;
    });
    // Ultima8Engine::I_moveKeyDownRecently (10A8:4340): DS:322A, the number
    // of keys held, any key.
    machine_->set_intrinsic(0x096, [this](UCMachine&, UCProcess&, std::span<const std::uint8_t>) -> std::uint32_t {
        return keyboard_.held();
    });
    // Game::I_playCredits (13E8:003F): creates a process (vtable DS:628B)
    // whose run is the end of the game (play_end_credits).
    machine_->set_intrinsic(0x095, [this](UCMachine&, UCProcess&, std::span<const std::uint8_t>) -> std::uint32_t {
        play_credits();
        return 0;
    });
    // 11D8:342C: the colour cycling, once the game palette is on screen.
    kernel_->spawn<PaletteCycleProcess>(world_->state);
    kernel_->spawn<PickupListProcess>(world_->state);

    // Keyboard: events go to the avatar mover; the KeyDaemon repeats held keys.
    mover_ = std::make_unique<AvatarMover>(*kernel_, *world_, anims_, keyboard_);
    keyboard_.set_handler([this](const GameKeyEvent& e) { on_key(e); });
    key_daemon_ = &kernel_->spawn<KeyDaemon>(keyboard_);
    combat_ = &kernel_->spawn<CombatControl>(*world_, anims_, keyboard_, *mover_);
    // The mouse in play sends its keys to the Controller too (DS:284E +0x28).
    world_mouse_ = std::make_unique<WorldMouse>(*kernel_, *world_, anims_, keyboard_,
                                                [this](const GameKeyEvent& e) { combat_->handle_key(e); });
    mouse_queue_.clear();
    kernel_->spawn<MouseDaemon>(mouse_queue_);
    kernel_->spawn<DumbTimer>(*world_, anims_);
    WorldHooks hooks = world_->hooks();
    hooks.fire_weapon = [this](ObjId actor, int a, int b, int z, int) { fire_frame(actor, a, b, z); };
    // NPCs entering the fast area start their activity again. The controlled
    // NPC is left alone: the port rebuilds the fast area when loading a game
    // where the original restores the saved state, and the reset would undo
    // the player's saved stance.
    hooks.monster_egg = [this](ObjId egg) { spawn_from_egg(*kernel_, *world_, anims_, npc_types_, egg); };
    hooks.egg_hatcher = [this](ObjId egg) { start_egg_hatcher(*kernel_, *world_, egg); };
    // 1040:04BC: stumble (0x3B, replacing the current animations), stand
    // up (4 in combat, else 0), and a grunt on the NPC's voice.
    hooks.hard_landing = [this](ObjId npc) {
        do_anim(*kernel_, *world_, anims_, npc, 0x3B, kDirCurrent, 10000, true);
        do_anim(*kernel_, *world_, anims_, npc, in_combat(*world_, npc) ? 4 : 0, kDirCurrent, 1, false);
        if (audio_) audio_->sources.retrigger(*world_, 0x8F, npc);
    };
    hooks.npc_entered_fast_area = [this](ObjId npc) {
        if (npc != world_->state.controlled_npc) restart_activity(*kernel_, *world_, anims_, npc);
    };
    hooks.drop_falling = [this](ObjId item) { drop_out_of_fast_area(*kernel_, *world_, item); };
    hooks.show_message = [this](const std::string& text) { show_message(text); };
    hooks.autosave = [this] { autosave(); };
    hooks.autosave_soon = [this] { request_autosave(); };
    hooks.designated_target_moved = [this](ObjId item, std::int32_t x, std::int32_t y, std::int32_t z) {
        designated_target_moved(*kernel_, *world_, anims_, item, x, y, z);
    };
    hooks.designated_target_forgotten = [this](ObjId) { redesignate(*kernel_, *world_, anims_); };
    hooks.reset_fire = [this] { combat_->reset_fire(); };
    hooks.got_hit = [this](ObjId item, ObjId other, std::uint16_t force) {
        got_hit(*kernel_, *world_, anims_, item, other, force);
    };
    world_->set_hooks(hooks);
    menu_.reset();
    menu_process_ = nullptr;
    message_.reset();
    message_process_ = nullptr;
    screen_.reset();
    screen_closed_ = nullptr;
    screen_process_ = nullptr;
    sprites_suspended_ = false;
    autosave_pending_ = false;
}

// @orig none: the controller screen's "press a button"
bool Game::waiting_for_pad() const {
    if (menu_) return menu_->waiting_for_pad();
    return screen_ && screen_->waiting_for_pad();
}

// @orig none
void Game::pad_button(int button) {
    if (menu_) menu_->pad_button(button);
    else if (screen_) screen_->pad_button(button);
}

void Game::key(std::uint8_t scancode, bool extended, bool pressed, std::uint16_t flags, std::uint8_t ascii) {
    keyboard_.key(scancode, extended, pressed, flags, ascii, kernel_->ticks());
}

// @orig none: the slot's file in the save directory
void Game::load_save(int slot) { load_save_file(save_dir_ / ("SAVEGAME." + std::to_string(slot))); }

// @orig 1058:02F1 1028:0000
void Game::load_save_file(const std::filesystem::path& path) {
    reset_world();
    const auto save = FlexArchive::open(path);
    world_->load_saved_items(save);
    const Item* player = world_->get(1);
    if (!player) throw std::runtime_error("saved game without the player (item 1)");
    // Entry 60 (FLAG_1410_02e7): the usecode globals, as in UNKDS.DAT.
    set_globals(save.entry(60));
    // Entry 91: Weasel's introduction played (DS:5ECF). Entry 90, the alert
    // (DS:135C), is read too but cleared when the game starts again.
    if (const auto weasel = save.entry(91); !weasel.empty()) weasel_intro_played_ = weasel[0] != 0;
    // The stasis of the player (entry 20 +2) is held by a process (a charger,
    // a teleport, a script) that ends it; the processes are not loaded (see
    // below), so none would: the player could never move again.
    world_->state.avatar_in_stasis = false;

    // The kernel's processes (entries 100+) are not loaded yet, so the
    // scripts running at save time cannot resume. Instead the fast area is
    // rebuilt as on arrival: its items enter it again and start their events.
    // Glob items are recreated identically (checked against SAVEGAME.2).
    // The guns and cameras that run WALLGUN's process (their enterFastArea
    // spawns it unless status bit 4 says it runs; their leaveFastArea clears
    // the bit) would otherwise stay idle: their saved process is not loaded.
    static constexpr std::uint16_t kGuardedGuns[] = {0x108, 0x113, 0x1B9, 0x1BA, 0x1CD, 0x25F,
                                                     0x260, 0x2F0, 0x373, 0x399, 0x3A1, 0x4C8};
    world_->for_each([this](const Item& i) {
        if (std::find(std::begin(kGuardedGuns), std::end(kGuardedGuns), i.shape) == std::end(kGuardedGuns)) return;
        if (Item* gun = world_->get(i.id)) gun->flags &= static_cast<std::uint16_t>(~0x0004);
    });
    // A jet of steam being shut off (0x4FB from STEAM1, 0x512 from STEAM2,
    // status 0x1000) waits in STEAMBOX_0327 for its last frame, then becomes
    // the marker 0x3A9 (frame 0 or 1) that a valve opens again. Without that
    // process it would loop forever, and no valve looks for it: finish it.
    std::vector<ObjId> closing;
    world_->for_each([&](const Item& i) {
        if ((i.shape == 0x4FB || i.shape == 0x512) && (i.flags & 0x1000)) closing.push_back(i.id);
    });
    for (ObjId id : closing) {
        Item* steam = world_->get(id);
        const std::uint16_t frame = steam->shape == 0x512 ? 1 : 0;
        world_->set_shape(*steam, 0x3A9);
        world_->set_frame(*steam, frame);
        steam->flags &= static_cast<std::uint16_t>(~0x1000);
    }
    world_->clear_fast_area();
    world_->set_camera(player->x, player->y, player->z);
    // An NPC saved while its death played (dead, last action 0x12 or 0x14,
    // short of the action's last frame): without its animation process it
    // would stay as it was, often still standing, and never become a body
    // (TEST11, docs/bugs.md). The death is finished at once: the action's
    // last frame, and its event calledFromAnim (0x0E), from which the NPC
    // classes start NPCDEATH. Replaying the animation would not do: out of
    // the fast area its steps are refused.
    for (ObjId id = 2; id < 256; ++id) {
        Item* npc = world_->get(id);
        if (!npc || (npc->z & 0xFF) == 254 || !world_->is_dead(id)) continue;
        const std::uint16_t last = world_->npc_word(id, npc::kLastAnim);
        if (last != 0x12 && last != 0x14) continue;
        const AnimAction* a = anims_.action(npc->shape, last);
        if (!a || a->frame_count() == 0) continue;
        const AnimFrame* end = a->frame(world_->npc_direction(id), a->frame_count() - 1);
        if (!end || npc->frame == end->frame || actor_busy(*kernel_, id)) continue;
        world_->set_frame(*npc, end->frame);
        if (end->mirrored) npc->flags |= 0x0020;
        else npc->flags &= static_cast<std::uint16_t>(~0x0020);
        world_->npc_record(id)[npc::kAnimFrame] = static_cast<std::uint8_t>(a->frame_count() - 1);
        if (end->event && world_->hooks().start_event) world_->hooks().start_event(id, ActorAnimProcess::kCalledFromAnimEvent);
    }
    // 1200:02BF: the music of the saved game, from its start.
    if (audio_) {
        const auto track = save.entry(70);
        const auto flags = save.entry(71);
        if (track.size() >= 2 && flags.size() >= 2) {
            audio_->music.restore(track[0] | track[1] << 8, flags[0] | flags[1] << 8);
        }
    }
    world_->state.status_bar_visible = true;
    apply_status_bar_mouse_range();
    // Game_start (1028:0000), a game loaded: the game palette, the colour
    // cycling resumed.
    world_->state.cycle_paused = false;
}

// @orig 1058:0115
void Game::save_game(int slot) {
    if (slot < 1 || slot > LoadScreen::kNames) throw std::runtime_error("no save slot " + std::to_string(slot));
    FlexWriter flex(1024);
    auto bytes = [](std::initializer_list<std::uint32_t> words, std::size_t width) {
        std::vector<std::uint8_t> out;
        for (std::uint32_t w : words) {
            for (std::size_t b = 0; b < width; ++b) out.push_back(static_cast<std::uint8_t>(w >> (8 * b)));
        }
        return out;
    };
    // Entry 9: the slot's name, 32 bytes (DS:7964); 90: the alert (DS:135C);
    // 91: Weasel's introduction played (DS:5ECF).
    std::vector<std::uint8_t> name(32, 0);
    const std::string& text = save_names_[static_cast<std::size_t>(slot - 1)];
    std::copy_n(text.begin(), std::min<std::size_t>(text.size(), 31), name.begin());
    flex.add(9, name);
    flex.add(90, bytes({world_->state.alert_active ? 1u : 0u}, 1));
    flex.add(91, bytes({weasel_intro_played_ ? 1u : 0u}, 1));
    flex.add(0, world_->saved_item_table());
    flex.add(1, world_->saved_item_lists());
    flex.add(10, world_->npc_table());
    // Entry 20 (NPC_10f0_390d): the player block, with the state of
    // SYSTEM.C's generator taken just before (random_get_seed, DS:7E12).
    const GameState& s = world_->state;
    std::array<std::uint8_t, 0x15> player = s.player_block;
    auto put = [&](std::size_t at, std::uint32_t v, std::size_t width) {
        for (std::size_t b = 0; b < width; ++b) player[at + b] = static_cast<std::uint8_t>(v >> (8 * b));
    };
    put(0x00, 1, 2);
    put(0x02, s.avatar_in_stasis ? 1 : 0, 2);
    put(0x04, s.random_state, 4);
    put(0x0B, s.difficulty, 2);
    put(0x0D, s.keycards, 4);
    put(0x11, s.battery, 2);
    flex.add(20, player);
    for (int part = 0; part < 3; ++part) flex.add(static_cast<std::size_t>(50 + part), world_->saved_tracker(part));
    // Entry 60 (FLAG_1410_02e7): the usecode globals, DS:65A8 bytes (the
    // size of UNKDS.DAT, 0x42).
    flex.add(60, std::span(machine_->globals()).first(kSavedGlobals));

    // Entries 70, 71 (STDINT_1200_0311): the music track and its flags.
    flex.add(70, bytes({static_cast<std::uint32_t>(audio_ ? audio_->music.current() : 0)}, 2));
    flex.add(71, bytes({static_cast<std::uint32_t>(audio_ ? audio_->music.flags() : 0)}, 2));
    const auto& dir = save_dir_;
    std::filesystem::create_directories(dir);
    flex.write(dir / ("SAVEGAME." + std::to_string(slot)));
}

// @orig none: port-only autosave
void Game::autosave() {
    if (!config_.autosave || modal() || movie_ || !world_ || !world_->get(1) || world_->is_dead(1) ||
        world_->state.controlled_npc != 1) {
        return;
    }
    try {
        set_save_name(kAutoSave, tr("SAUVEGARDE AUTO", "AUTOSAVE"));
        save_game(kAutoSave);
        autosave_icon_us_ = kAutosaveIconUs;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "autosave: %s\n", e.what());
    }
}

// @orig none: port-only autosave
void Game::request_autosave() {
    if (!config_.autosave) return;
    autosave_pending_ = true;
    autosave_due_ = kernel_->ticks() + kAutosaveDelayTicks;
}

// @orig none: port-only autosave
void Game::draw_autosave_icon(Surface8& screen) {
    if (autosave_icon_us_ == 0) return;
    if (floppy_.empty()) {
        // The nearest colours of the game palette, but not the cycling ones (0..14).
        const Palette& palette = world_->state.game_palette;
        for (const std::uint32_t pixel : kFloppyPixels) {
            std::uint8_t best = kTransparentIndex;
            if ((pixel & 0xFF) != 0) {
                const int r = static_cast<int>(pixel >> 24), g = static_cast<int>(pixel >> 16 & 0xFF),
                          b = static_cast<int>(pixel >> 8 & 0xFF);
                int best_distance = 1 << 30;
                for (int i = 15; i < kTransparentIndex; ++i) {
                    const Rgb& c = palette.colors[static_cast<std::size_t>(i)];
                    const int dr = r - c.r, dg = g - c.g, db = b - c.b;
                    const int distance = dr * dr + dg * dg + db * db;
                    if (distance < best_distance) {
                        best_distance = distance;
                        best = static_cast<std::uint8_t>(i);
                    }
                }
            }
            floppy_.push_back(best);
        }
    }
    // Top right, each pixel of the icon doubled.
    const int x0 = kScreenWidth - kFloppyWidth * kFloppyScale - kFloppyMargin, y0 = kFloppyMargin;
    for (int y = 0; y < kFloppyHeight * kFloppyScale; ++y) {
        for (int x = 0; x < kFloppyWidth * kFloppyScale; ++x) {
            const std::uint8_t index = floppy_[static_cast<std::size_t>(y / kFloppyScale * kFloppyWidth + x / kFloppyScale)];
            if (index != kTransparentIndex) screen.row(y0 + y)[x0 + x] = index;
        }
    }
}

// @orig none: save directory of the port (tests)
void Game::set_save_dir(const std::filesystem::path& dir) {
    save_dir_ = dir;
    save_names_ = LoadScreen::read_names(dir);
}

// @orig 1058:0528

void Game::set_save_name(int slot, const std::string& name) {
    if (slot < 1 || slot > LoadScreen::kNames) return;
    save_names_[static_cast<std::size_t>(slot - 1)] = name.substr(0, 31);
}

// @orig 1008:041A 1048:0429
void Game::save_named(int slot, const std::string& name) {
    // The save screen: the name is set, then the free space of the drive is
    // checked (GAMEPAL_1048_0429, 500000 bytes at least); without it, the
    // message DS:00A3 instead of the save.
    set_save_name(slot, name);
    std::error_code ec;
    const auto space = std::filesystem::space(save_dir_, ec);
    if (!ec && space.available < 500000) {
        show_message("ERROR:\n\nUnable to save game.\n\nPlease free up 5 Megs of drive space.");
        return;
    }
    save_or_report(slot);
}

// @orig none: a failed write is reported instead of ending the game
bool Game::save_or_report(int slot) {
    try {
        save_game(slot);
        return true;
    } catch (const std::exception& e) {
        show_message(std::string("ERROR:\n\nUnable to save game.\n\n") + e.what());
        return false;
    }
}


// @orig 1410:0051
void Game::set_globals(std::span<const std::uint8_t> bytes) {


    auto& globals = machine_->globals();
    std::fill(globals.begin(), globals.end(), std::uint8_t{0});
    std::copy_n(bytes.begin(), std::min(bytes.size(), globals.size()), globals.begin());
}

// @orig 1028:0000
void Game::new_game(int difficulty) {
    reset_world();
    // FLAG_1410_0051(0): the globals' initial values.
    {
        std::ifstream in(data_ / "USECODE/UNKDS.DAT", std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        set_globals(bytes);
    }
    // MusicProcess::I_stopMusic: the mission's scripts start its music.
    if (audio_) audio_->music.stop();
    GameState& s = world_->state;
    s.difficulty = static_cast<std::uint16_t>(difficulty >= 1 && difficulty <= 4 ? difficulty : 3);
    // Game_start: the colour cycling stays paused until the mission's
    // scripts fade the game palette in.
    s.cycle_paused = true;

    // NPC_10f0_000c: item 1, shape 1, frame 0, NPC number 1, not on any map
    // yet; NPC_10f0_385c: 150 hit points, no keycard, no battery.
    Item player;
    player.id = 1;
    player.shape = 1;
    player.npc_number = 1;
    player.off_map = true;
    world_->add_with_id(std::move(player));
    world_->set_npc_word(1, 0x00, 150);
    s.keycards = 0;
    s.battery = 0;
    s.avatar_in_stasis = false;

    // The datalink, dropped at the player's feet then picked up, and a
    // container put in him.
    if (world_->create(0x4D4, 0)) {
        const Item& p = *world_->get(1);
        add_item_cru(*world_, world_->pop_to_coords(p.x, p.y, p.z), false);
    }
    if (world_->create(0x598, 0)) world_->pop_to_container(1);

    // CameraProcess::I_setCenterOn(1), then the first mission.
    s.camera_target = 1;
    start_teleport_to_egg(*kernel_, *world_, 1, kTeleportStartEgg, true, true);
    s.status_bar_visible = true;
    apply_status_bar_mouse_range();
}

// @orig 11D8:31CF
void Game::update(std::uint32_t elapsed_us) {
    // The enhancements window edits config_ in place.
    if (world_) world_->set_follow_player(config_.follow_player);
    if (world_ && aim_stick_ && !modal() && !movie_ && !world_->state.avatar_in_stasis && world_->state.controlled_npc == 1) {
        // Screen east is the world angle 315 degrees, north 225: 315 - screen.
        aim_at_angle(*kernel_, *world_, anims_, static_cast<int>(std::lround(315.0 - aim_stick_deg_)),
                     config_.draw_on_aim);
    }
    // Outside the kernel slice: a key repeat can choose a save from within it.
    if (pending_load_) {
        const int slot = pending_load_;
        pending_load_ = 0;
        if (std::filesystem::exists(save_dir_ / ("SAVEGAME." + std::to_string(slot)))) load_save(slot);
        if (pending_notice_) {
            // F4: the notice once the game is loaded, in the menu mode again.
            const std::uint16_t shape = pending_notice_;
            pending_notice_ = 0;
            enter_menu_mode();
            notify(shape);
        }
    }
    if (pending_new_game_) {
        const int difficulty = pending_new_game_;
        pending_new_game_ = 0;
        new_game(difficulty);
    }
    autosave_icon_us_ -= std::min(autosave_icon_us_, elapsed_us);
    if (movie_) {
        // The game is frozen while the movie plays (the original plays it
        // inside the intrinsic).
        update_movie(elapsed_us);
        mix_elapsed(elapsed_us);
        return;
    }
    dispatch_mouse();
    if (!modal()) world_->update_follow_camera();
    const std::uint32_t before = kernel_->ticks();
    kernel_->advance_time(elapsed_us);
    kernel_->run_slice();
    animate_scenery();
    if (autosave_pending_ && kernel_->ticks() >= autosave_due_ && !modal() && player_may_act(*world_)) {
        autosave_pending_ = false;
        autosave();
    }
    if (end_credits_pending_ && !modal()) play_end_credits();
    if (!modal()) selection_tick(*world_);
    mix_elapsed(elapsed_us);
    if (menu_) {
        menu_->update(kernel_->ticks());
        run_credits(before);
    }
    if (screen_) {
        // Weasel's shop closes itself at one of its steps.
        screen_->update(kernel_->ticks());
        if (screen_->closed()) close_screen();
        else if (const Palette* p = screen_->palette()) world_->state.screen_palette = *p;
    }
    apply_status_bar_mouse_range();
}

// @orig 1038:01B1 1188:1719
void Game::animate_scenery() {
    if (modal()) return;
    ++game_passes_;
    animate_items(*world_, game_passes_, animated_at_, [this](std::uint16_t shape) {
        const Shape* s = shapes_.get(shape);
        return s ? static_cast<int>(s->frames().size()) : 0;
    });
    animated_at_ = game_passes_;
}

// @orig none: audio clock of the port
void Game::mix_elapsed(std::uint32_t elapsed_us) {
    if (!audio_ || audio_device_) return;
    mixed_us_ += elapsed_us;
    const std::uint64_t due = mixed_us_ * AsylumMixer::kMixRate / 1'000'000;
    const std::size_t count = static_cast<std::size_t>(due - mixed_samples_);
    mixed_samples_ = due;
    std::vector<std::int16_t> scratch;
    std::vector<std::int16_t>& out = capture_ ? captured_ : scratch;
    const std::size_t from = out.size();
    out.resize(from + count);
    audio_->mixer.mix(out.data() + from, count);
}

// @orig 1468:3F77 1468:4060 1468:4169
void Game::play_movie(const std::string& name, MovieKind kind) {
    if (movie_) return;
    Surface8 screen(kScreenWidth, kScreenHeight);
    render_view(screen);
    start_movie(name, kind, std::move(screen));
}

// @orig 1468:03E3 1468:3ABC
void Game::start_movie(const std::string& name, MovieKind kind, Surface8 screen) {
    const std::filesystem::path base = data_ / "FLICS" / movie_base_name(name);
    std::filesystem::path avi = base;
    avi += ".AVI";
    std::optional<AviMovie> movie;
    try {
        movie = AviMovie::load(avi);
    } catch (const std::exception&) {
        return;  // 1468:2F7C: no such movie, nothing happens
    }
    std::vector<MovieSubtitle> subtitles;
    if (config_.subtitles && kind != MovieKind::kOverlay) {
        std::filesystem::path txt = base;
        txt += ".TXT";
        subtitles = load_movie_subtitles(txt, fonts_, MoviePlayer::kSubtitleFont);
    }
    movie_ = std::make_unique<MoviePlayer>(std::move(*movie), kind, std::move(subtitles), config_.big_video,
                                           std::move(screen), world_->state.screen_palette, gumps_, fonts_);
    // 1468:3ABC: the colour cycling pauses.
    world_->state.cycle_paused = true;
    movie_resumes_cycle_ = !world_->state.movie_keeps_cycle_paused;
    kernel_->interrupt_slice();
}

// @orig 1468:03E3 1468:2869
void Game::update_movie(std::uint32_t elapsed_us) {
    movie_->advance(elapsed_us, [this](std::uint8_t scancode) { return keyboard_.down(scancode, false) != 0; });
    if (movie_->take_playback_start() && audio_) {
        // 1468:03E3: dsfx_stop_all, music_pause (not for an overlay).
        audio_->stop_all();
        if (movie_->kind() != MovieKind::kOverlay) audio_->music.pause();
    }
    if (movie_->take_sound_start() && audio_) {
        movie_sounds_.push_back(movie_->sound());
        if (movie_sounds_.size() > 2) movie_sounds_.pop_front();
        const std::vector<std::int8_t>& pcm = movie_sounds_.back();
        audio_->mixer.play(0, SoundSample{pcm, 0, 0, AviMovie::kAudioRate}, AsylumMixer::kMaxVolume + 1, 0, false);
    }
    world_->state.screen_palette = movie_->palette();
    if (movie_->finished()) end_movie();
}

// @orig 1468:3D7A 1468:3C23
void Game::end_movie() {
    const bool shop_movie = weasel_movie_;
    // 1468:3D7A / 3C23.
    if (audio_) {
        audio_->mixer.stop(0);
        if (movie_->kind() != MovieKind::kOverlay) audio_->music.resume();
    }
    keyboard_.reset();
    if (movie_->kind() != MovieKind::kOverlay) world_->state.screen_palette = movie_->kept_palette();
    // Not under the start-up menu (open while the movies play): the cycling
    // would write the game's colours 1..14 over the menu palette.
    if (movie_resumes_cycle_ && !menu_) world_->state.cycle_paused = false;
    if (weasel_movie_) {
        // 13E0:0CD8: palette_write_hw(DS:058C), DS:6810 +0x40 cleared.
        weasel_movie_ = false;
        world_->state.screen_palette = world_->state.game_palette;
        world_->state.cycle_paused = false;
    }
    movie_.reset();
    // Port addition: a movie of the mission's course (not the shop's, nor the
    // start-up's under the menu) ends: autosave.
    if (!shop_movie && !menu_ && !end_credits_ && !end_credits_pending_) request_autosave();
    next_start_up_movie();
}

// @orig 1028:0000
void Game::next_start_up_movie() {
    // Game_start after the end credits: the movies of the start-up, one
    // after the other on a black screen (the credits leave it black); a
    // missing one is skipped. Then the menu gets its music: in the original
    // it only opens now (IRGUMP_13d0_0000), the port opened it before to
    // hold the credits.
    while (!movie_ && !next_movies_.empty()) {
        const std::string next = next_movies_.front();
        next_movies_.erase(next_movies_.begin());
        start_movie(next, MovieKind::kCutsceneNoFade, Surface8(kScreenWidth, kScreenHeight));
    }
    if (!movie_ && end_credits_) {
        end_credits_ = false;
        if (audio_) audio_->music.push(Music::kMenu);
    }
}

// @orig 1020:0C7F
void Game::save_config() {
    if (config_file_.empty()) return;
    try {
        config_.save(config_file_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
    }
}

// @orig 13F0:03F4 13F0:0290
void Game::run_credits(std::uint32_t ticks_before) {
    Credits* c = menu_->credits();
    if (!c) {
        // Stopped by a key: Credits_done all the same.
        if (credits_music_ && audio_) audio_->music.pop();
        credits_music_ = false;
        return;
    }
    // One pass of the credits loop per retrace, i.e. per 60 Hz tick. The
    // cursor is hidden and the mouse driver off meanwhile (IRGUMP case 4).
    cursor_.hide();
    for (std::uint32_t t = ticks_before; t < kernel_->ticks() && !c->finished(); ++t) c->frame(t + 1);
    // Credits_init / Credits_done: the credits' own music over the menu's.
    if (!credits_music_ && audio_) {
        credits_music_ = true;
        audio_->music.push(Music::kCredits);
    }
    world_->state.screen_palette = c->palette();
    if (c->finished()) {
        menu_->end_credits();
        if (credits_music_ && audio_) audio_->music.pop();
        credits_music_ = false;
        world_->state.screen_palette = menu_palette_;
        keyboard_.reset();
        cursor_.show();
        if (end_credits_) {
            // Game_start (1028:0000), while DS:08F6 (a mission on the command
            // line) is -1 and DS:0904 (play them until a click) is 0.
            next_movies_ = {"T01", "T02"};
            next_start_up_movie();
        }
    }
}

// @orig 1028:0000
void Game::start_up() {
    // As the end of play_end_credits, without the credits: the menu is open
    // first (no game), the movies play over it on a black screen, and its
    // music starts once they are over (next_start_up_movie).
    open_menu(true);
    end_credits_ = true;
    next_movies_ = {"T01", "T02"};
    next_start_up_movie();
}

// @orig 13E8:0000 1028:0000
void Game::play_end_credits() {
    // 13E8:0000: the music stopped and its stack emptied (music_reset), no
    // game any more (DS:5BEE), the cursor hidden, the screen black; then the
    // credits (Credits_run) and Game_start: the start-up movies and the menu,
    // where "back" and "save" do nothing. The menu is opened first here: it
    // holds the credits as for its button 4.
    end_credits_pending_ = false;
    if (audio_) {
        audio_->music.stop();
        audio_->music.reset();
    }
    open_menu(true);
    end_credits_ = true;
    menu_->show_credits();
}

// @orig 1138:0896 13E8:14B4
void Game::on_key(const GameKeyEvent& e) {
    // A movie only looks at the keys held (1468:2869).
    if (movie_) return;
    const std::uint16_t code = gump_key_code(e);
    if (message_) {
        message_->key(code);
        if (message_->closed()) close_message();
        return;
    }
    if (menu_) {
        if (code) menu_->key(code);
        follow_menu_screen();
        if (menu_->result()) close_menu();
        return;
    }
    if (screen_) {
        if (code) screen_->key(code);
        if (screen_->closed()) close_screen();
        else if (const Palette* p = screen_->palette()) world_->state.screen_palette = *p;
        return;
    }
    // CRUSADER_1138_0896: '=' and '-' (scancodes 0x0D, 0x0C) step the music
    // volume, up to 0x30.
    if (e.pressed && audio_ && (e.scancode == 0x0D || e.scancode == 0x0C)) {
        if (e.scancode == 0x0D && config_.music_volume < 0x30) ++config_.music_volume;
        if (e.scancode == 0x0C && config_.music_volume != 0) --config_.music_volume;
        audio_->mixer.set_music_volume(config_.music_volume);
    }
    // CRUSADER_1138_0896: Escape (scancode 1) without both Ctrl and Alt gives
    // the control back to the player when another NPC is controlled (a robot
    // taken from a camera monitor, or none once it died), instead of the menu.
    if (e.pressed && e.scancode == 0x01 && (e.flags & (kCtrl | kAlt)) != (kCtrl | kAlt) &&
        world_->state.controlled_npc != 1) {
        set_controlled_npc(*kernel_, *world_, anims_, 1);
        return;
    }
    // CRUSADER_1138_0896: Z (scancode 0x2C) puts the camera on the controlled
    // NPC, living and not the player in stasis; the item selection ends.
    if (e.pressed && e.scancode == 0x2C) {
        const ObjId npc = world_->state.controlled_npc;
        const Item* item = world_->get(npc);
        if (item && !world_->is_dead(npc) && !(npc == 1 && world_->state.avatar_in_stasis)) {
            if (world_->state.selection.active) cancel_selection(*world_);
            world_->move_camera_to(item->x, item->y, item->z, 0);
            return;
        }
    }
    // Port addition (GameConfig::toggle_run): a Shift press flips running, and
    // the Controller sees Shift held exactly while it is on.
    GameKeyEvent ev = e;
    if (config_.toggle_run) {
        if ((e.scancode == 0x2A || e.scancode == 0x36) && !e.extended) {
            if (e.pressed && e.count <= 1) run_toggled_ = !run_toggled_;
            return;
        }
        ev.flags = static_cast<std::uint16_t>((e.flags & ~(kLeftShift | kRightShift)) |
                                              (run_toggled_ ? kLeftShift : 0));
    } else {
        run_toggled_ = false;
    }
    combat_->handle_key(ev);  // the Controller: movement, then fire keys
    // WORLD_13e8_14b4 (World_on_key): Alt+X first, whatever the view is
    // doing (the item selection too). Escape opens the menu, except in
    // "stasis" (DS:5FFB, GameState::cru_stasis) and when DS:5FFC is not 1
    // (not decoded yet: assumed 1 during normal play); in the item selection
    // mode it ends it instead. Then, out of that mode, the function keys,
    // and the inventory keys.
    if (code == gump_key::kAltX) {
        function_key(code);
        return;
    }
    if (code == gump_key::kEscape && !world_->state.cru_stasis) {
        if (world_->state.selection.active) cancel_selection(*world_);
        else open_menu();
    }
    if (code && !world_->state.selection.active && function_key(code)) return;
    if (code) player_command(*kernel_, *world_, anims_, code);
}

// @orig 13E8:10C3 13E8:0E7D
void Game::open_menu() {
    open_menu(false);
}

// @orig 13D0:0000
void Game::open_menu(bool no_game) {
    if (menu_) return;
    // WORLD_13e8_0e7d and 10c3: the status bar goes, the cursor shows.
    world_->state.status_bar_visible = false;
    world_->state.cycle_paused = true;
    apply_status_bar_mouse_range();
    // IRGUMP_13d0_0000: running fades are stopped (Kernel_fail_processes,
    // type 0x206), the menu palette is written.
    kernel_->fail_processes(0, PaletteFaderProcess::kType, kernel_->level());
    world_->state.screen_palette = menu_palette_;
    key_daemon_->set_rate(kMenuDaemonRate);
    keyboard_.set_repeat_delay(kMenuRepeatDelay);
    keyboard_.reset();
    cursor_.reset(MouseCursor::kMenuArrow);
    cursor_.show();
    // CRUSADER_VERSION may carry a leading "v" (release builds): the label adds its own.
    const std::string port_version =
        !port_version_.empty() && (port_version_[0] == 'v' || port_version_[0] == 'V') ? port_version_.substr(1) : port_version_;
    menu_ = std::make_unique<MainMenu>(
        MainMenu::Resources{&gumps_, &fonts_, &save_names_, &credits_data_, &config_, audio_.get(),
                            [this] { save_config(); },
                            port_version.empty() ? std::string() : tr("PORTAGE V", "PORT V") + port_version,
                            tr("DONNEES DU JEU ", "GAME DATA ") +
                                (data_version_.empty() ? std::string(tr("INCONNUES", "UNKNOWN")) : "V" + data_version_) + " " +
                                (data_language_.empty() ? std::string(tr("LANGUE INCONNUE", "UNKNOWN LANGUAGE")) : data_language_)},
        menu_shared_, no_game);
    auto p = std::make_unique<ModalProcess>();
    menu_process_ = p.get();
    kernel_->add(std::move(p));
    kernel_->raise_level(*menu_process_);
    // At the end of the game, once the start-up movies are over (end_movie).
    if (audio_ && !no_game) audio_->music.push(Music::kMenu);
}

// @orig 13D0:03F5 13E8:0EF9
void Game::close_menu() {
    const int result = *menu_->result();
    const int load_slot = menu_->load_slot();
    const std::string save_name = menu_->save_name();
    const int new_game_difficulty = menu_->new_game_difficulty();
    menu_.reset();
    difficulty_shown_ = false;
    // IRGUMP_13d0_03f5: the game's music again.
    if (audio_) audio_->music.pop();
    cursor_.hide();
    key_daemon_->set_rate(Keyboard::kDaemonRate);
    keyboard_.set_repeat_delay(Keyboard::kRepeatDelay);
    keyboard_.reset();
    menu_process_->terminate();
    menu_process_ = nullptr;
    kernel_->drop_empty_levels();
    // WORLD_13e8_10c3 then 0ef9: the status bar comes back (shown whatever
    // its state before), the cursor returns to the arrow, hidden.
    cursor_.reset(MouseCursor::kArrow);
    cursor_.hide();
    if (result == MainMenu::kQuit) {
        quit_ = true;
        return;
    }
    if (result == MainMenu::kLoad) pending_load_ = load_slot + 1;
    // World_game_menu, result 2: Game_save, then back to the game.
    if (result == MainMenu::kSave) save_named(load_slot + 1, save_name);
    if (result == MainMenu::kNewGame) pending_new_game_ = new_game_difficulty;
    world_->state.status_bar_visible = true;
    world_->state.screen_palette = world_->state.game_palette;  // palette_write_hw(DS:058C)
    world_->state.cycle_paused = false;                         // MainMenu_dtor (13D0:03F5)
    cursor_.pop();
    apply_status_bar_mouse_range();
}

// @orig 1358:0046 1328:1588
void Game::show_message(const std::string& text) {
    if (message_ || menu_) return;
    message_ = std::make_unique<MessageGump>(fonts_, text, kScreenWidth, kScreenHeight);
    // Gump_run_modal: only the display and the events, so the game stops.
    auto p = std::make_unique<ModalProcess>();
    message_process_ = p.get();
    kernel_->add(std::move(p));
    kernel_->raise_level(*message_process_);
    kernel_->interrupt_slice();
}

// @orig 1358:02C8
void Game::close_message() {
    message_.reset();
    message_process_->terminate();
    message_process_ = nullptr;
    kernel_->drop_empty_levels();
}

// @orig 13E8:14B4
bool Game::function_key(std::uint16_t code) {
    const GameState& s = world_->state;
    switch (code) {
    case gump_key::kAltX:
        // Quit? Yes: DS:6810 +0x40 = 1, a blocking fade to black (not
        // reproduced), then system_shutdown.
        ask(ShapeDialog::kQuitQuestion, 0x109, [this] { quit_ = true; });
        return true;
    case '?':
    case gump_key::kF1: {
        auto help = std::make_unique<HelpScreen>(
            gumps_, help_pictures_,
            [this](int n) {
                return static_cast<int>(random_range(world_->state.random_state, static_cast<std::uint16_t>(n)));
            },
            [this](int sfx) {
                if (audio_) audio_->play_sfx(sfx);
            });
        // 13E8:38E5: the menu mode, the palette faders stopped, the colour
        // cycling paused (DS:6810 +0x40), cursor 0x27.
        enter_menu_mode();
        kernel_->fail_processes(0, PaletteFaderProcess::kType, kernel_->level());
        world_->state.cycle_paused = true;
        cursor_.reset(HelpScreen::kCursor);
        cursor_.show();
        world_->state.screen_palette = *help->palette();
        open_screen(std::move(help), [this](const ModalGump&) {
            // 13E8:3AAE: the game palette, the colour cycling again.
            cursor_.hide();
            world_->state.screen_palette = world_->state.game_palette;
            world_->state.cycle_paused = false;
            leave_menu_mode();
        });
        return true;
    }
    case gump_key::kF2:
        // The options screen: MISC.PAL, cursor 0x25; closing it saves the
        // configuration (Options_dtor). The colour cycling is paused while
        // it is open (jumpToAllBlack, DS:6810 +0x40), else it would write
        // the game's entries 1..14 over MISC.PAL.
        enter_menu_mode();
        world_->state.cycle_paused = true;
        world_->state.screen_palette = menu_palette_;
        cursor_.reset(MouseCursor::kMenuArrow);
        cursor_.show();
        open_screen(std::make_unique<OptionsGump>(gumps_, config_, audio_.get(), menu_shared_.options_focus, fonts_),
                    [this](const ModalGump&) {
                        save_config();
                        cursor_.reset(MouseCursor::kArrow);
                        cursor_.hide();
                        world_->state.screen_palette = world_->state.game_palette;
                        world_->state.cycle_paused = false;
                        leave_menu_mode();
                    });
        return true;
    case gump_key::kF4:
        // Load the quick save (slot 12, SAVEGAME.13)? Only while the player
        // is the controlled NPC.
        if (s.cru_stasis || s.controlled_npc != 1) return false;
        ask(ShapeDialog::kQuickLoadQuestion, 0x107, [this] {
            if (audio_) audio_->stop_all();
            pending_load_ = kQuickSave;
            pending_notice_ = ShapeDialog::kQuickLoadNotice;
        });
        return true;
    case gump_key::kF6:
        // Port addition: load the autosave (slot 12, SAVEGAME.12), as F4 loads
        // the quick save.
        if (s.cru_stasis || s.controlled_npc != 1) return false;
        if (!std::filesystem::exists(save_dir_ / ("SAVEGAME." + std::to_string(kAutoSave)))) return false;
        ask(ShapeDialog::kQuickLoadQuestion, 0x107, [this] {
            if (audio_) audio_->stop_all();
            pending_load_ = kAutoSave;
            pending_notice_ = ShapeDialog::kQuickLoadNotice;
        });
        return true;
    case gump_key::kF5:
        // Quick save? Yes: slot 12 (DS:0C94 = 12) under the name it has,
        // then the notice.
        if (!player_may_act(*world_) || s.cru_stasis) return false;
        ask(ShapeDialog::kQuickSaveQuestion, 0x108, [this] {
            if (save_or_report(kQuickSave)) notify(ShapeDialog::kQuickSaveNotice);
            else leave_menu_mode();
        });

        return true;
    case gump_key::kF8: {
        // The load screen (1008:0088, mode 1): MISC.PAL, cursor 0x25 pushed,
        // the colour cycling paused as for F2.
        if (s.cru_stasis) return false;
        enter_menu_mode();
        world_->state.cycle_paused = true;
        world_->state.screen_palette = menu_palette_;
        cursor_.push(MouseCursor::kMenuArrow);
        cursor_.show();
        auto load = std::make_unique<LoadGump>(gumps_, fonts_, save_names_, menu_shared_.selected_slot);
        open_screen(std::move(load), [this](const ModalGump& screen) {
            if (screen.result()) pending_load_ = static_cast<const LoadGump&>(screen).slot() + 1;
            cursor_.reset(MouseCursor::kArrow);
            cursor_.hide();
            world_->state.screen_palette = world_->state.game_palette;
            world_->state.cycle_paused = false;
            leave_menu_mode();
        });
        return true;
    }
    case gump_key::kF9: {
        // The save screen (1008:0088, mode 0), as F8 but only while the
        // player may act; a name given saves the game (Game_save).
        if (!player_may_act(*world_) || s.cru_stasis) return false;
        enter_menu_mode();
        world_->state.cycle_paused = true;
        world_->state.screen_palette = menu_palette_;
        cursor_.push(MouseCursor::kMenuArrow);
        cursor_.show();
        auto save = std::make_unique<LoadGump>(gumps_, fonts_, save_names_, menu_shared_.selected_slot,
                                               LoadScreen::Mode::kSave);
        open_screen(std::move(save), [this](const ModalGump& screen) {
            cursor_.reset(MouseCursor::kArrow);
            cursor_.hide();
            world_->state.screen_palette = world_->state.game_palette;
            world_->state.cycle_paused = false;
            leave_menu_mode();
            if (screen.result()) {
                const auto& gump = static_cast<const LoadGump&>(screen);
                save_named(gump.slot() + 1, gump.name());
            }
        });
        return true;
    }
    default:
        // F7 and the other keys of World_on_key are cheats (docs/interface.md).
        return false;
    }
}

// @orig 13F8:0000
void Game::ask(std::uint16_t shape, int sfx, std::function<void()> yes) {
    enter_menu_mode();
    if (audio_) audio_->play_sfx(sfx);
    open_screen(std::make_unique<ShapeDialog>(ShapeDialog::question(gumps_, shape)),
                [this, yes = std::move(yes)](const ModalGump& screen) {
                    if (screen.result()) yes();
                    else leave_menu_mode();
                });
}

void Game::notify(std::uint16_t shape) {
    open_screen(std::make_unique<ShapeDialog>(ShapeDialog::notice(gumps_, shape)),
                [this](const ModalGump&) { leave_menu_mode(); });
}

// @orig 1328:1588
void Game::open_screen(std::unique_ptr<ModalGump> screen, std::function<void(const ModalGump&)> closed) {
    screen_ = std::move(screen);
    screen_closed_ = std::move(closed);
    // Gump_run_modal: only the display and the events, so the game stops.
    auto p = std::make_unique<ModalProcess>();
    screen_process_ = p.get();
    kernel_->add(std::move(p));
    kernel_->raise_level(*screen_process_);
    kernel_->interrupt_slice();
}

// @orig 1328:1588
void Game::close_screen() {
    const std::unique_ptr<ModalGump> screen = std::move(screen_);
    const auto closed = std::move(screen_closed_);
    screen_closed_ = nullptr;
    screen_process_->terminate();
    screen_process_ = nullptr;
    kernel_->drop_empty_levels();
    if (closed) closed(*screen);
}

// @orig 13A0:0005 13A0:0212
void Game::read_computer(const std::string& text) {
    if (modal() || movie_) return;
    enter_menu_mode();
    cursor_.hide();  // 13A0:0034
    auto gump = std::make_unique<ComputerGump>(gumps_, fonts_, text, [this](int sfx) {
        if (!audio_) return;
        audio_->stop_sfx(sfx);
        audio_->play_sfx(sfx);
    }, kScreenWidth, kScreenHeight);
    open_screen(std::move(gump), [this](const ModalGump&) {
        // 13A0:0212: the cursor back, the keyboard flushed.
        cursor_.show();
        keyboard_.reset();
        leave_menu_mode();
    });
}

// @orig 13C8:00FE 13C8:04EE
void Game::show_keypad(std::int16_t code, Pid pid) {
    if (modal() || movie_) return;
    enter_menu_mode();
    auto gump = std::make_unique<KeypadGump>(gumps_, code, [this](int sfx) {
        if (audio_) audio_->play_sfx(sfx);
    }, kScreenWidth, kScreenHeight);
    open_screen(std::move(gump), [this, pid, code](const ModalGump& screen) {
        // 13C8:04EE: the keyboard and the mouse clicks flushed.
        keyboard_.reset();
        leave_menu_mode();
        if (auto* p = dynamic_cast<UCProcess*>(kernel_->get(pid))) {
            p->set_call_result(static_cast<std::uint16_t>(screen.result()));
        }
        // Port addition: the code matched, the door or chest is about to
        // open (autosave).
        if (code != 0 && static_cast<std::int16_t>(screen.result()) == code) autosave();
    });
}

// @orig 13E0:0000 13E0:0435
void Game::show_weasel(int level) {
    if (modal() || movie_) return;
    std::vector<std::uint8_t> stuff;
    if (std::ifstream in{data_ / "STATIC" / "STUFF.DAT", std::ios::binary}) {
        stuff.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    enter_menu_mode();
    WeaselGump::Hooks hooks;
    hooks.play_sfx = [this](int sfx) {
        if (audio_) audio_->play_sfx(sfx);
    };
    hooks.play_movie = [this](const std::string& name) { play_weasel_movie(name); };
    hooks.random = [this](int n) {
        return static_cast<int>(random_range(world_->state.random_state, static_cast<std::uint16_t>(n)));
    };
    auto gump = std::make_unique<WeaselGump>(*world_, gumps_, fonts_, std::move(stuff), level, weasel_intro_played_,
                                             !config_.no_weasel, std::move(hooks), kScreenWidth, kScreenHeight);
    // The dtor (13E0:0435): WORLD_13e8_0ef9.
    open_screen(std::move(gump), [this](const ModalGump&) { leave_menu_mode(); });
}

// The cursor hidden, the movie played over the view and the shop (a
// cutscene); the palette and the colour cycling come back in end_movie.
// @orig 13E0:0CD8
void Game::play_weasel_movie(const std::string& name) {
    if (movie_) return;
    Surface8 screen(kScreenWidth, kScreenHeight);
    render_view(screen);
    if (screen_) screen_->draw(screen);
    start_movie(name, MovieKind::kCutscene, std::move(screen));
    weasel_movie_ = movie_ != nullptr;
}

// @orig 13E8:0E7D
void Game::enter_menu_mode() {
    // 13E8:0E7D: the sounds stop, the screen sprites and the status bar go,
    // cursor 0x24 shown.
    if (audio_) audio_->stop_all();
    sprites_suspended_ = true;
    world_->state.status_bar_visible = false;
    apply_status_bar_mouse_range();
    cursor_.show();
    cursor_.reset(MouseCursor::kArrow);
}

// @orig 13E8:0EF9
void Game::leave_menu_mode() {
    // 13E8:0EF9: the screen sprites and the status bar back, the cursor
    // popped and hidden, the keys and clicks forgotten (Input_I_flush), the
    // "Crusader teleporting" flag cleared.
    sprites_suspended_ = false;
    world_->state.status_bar_visible = true;
    apply_status_bar_mouse_range();
    cursor_.pop();
    cursor_.hide();
    keyboard_.reset();
    world_->state.crusader_teleporting = false;
}

// @orig 13D0:25A2 13D0:082E
void Game::follow_menu_screen() {
    // IRGUMP_13d0_25a2: DIFF.PAL, cursor 0x26 with the mouse at (320, 400);
    // back in the menu (13D0:082E case 0): MISC.PAL and cursor 0x25 again.
    const bool shown = menu_ && menu_->difficulty_screen();
    if (shown == difficulty_shown_) return;
    difficulty_shown_ = shown;
    if (shown) {
        world_->state.screen_palette = difficulty_palette_;
        cursor_.push(DifficultyScreen::kCursor);
        cursor_.move_to(DifficultyScreen::kMouseX, DifficultyScreen::kMouseY);
    } else {
        world_->state.screen_palette = menu_palette_;
        cursor_.reset(MouseCursor::kMenuArrow);
    }
}

// @orig 11B8:0932 11B8:098F
void Game::apply_status_bar_mouse_range() {
    // StatusBar_show / hide: mouse range y <= 439 or 479, mouse at (320, 240).
    const bool shown = world_->state.status_bar_visible;
    if (shown == status_bar_shown_) return;
    status_bar_shown_ = shown;
    cursor_.set_bottom(shown ? 439 : 479);
    cursor_.move_to(320, 240);
}

// @orig 1360:045D
void Game::mouse_move(int x, int y) {
    cursor_.move_to(x, y);
    if (message_) message_->mouse_move(cursor_.x(), cursor_.y());
    else if (menu_) menu_->mouse_move(cursor_.x(), cursor_.y());
    else if (screen_) screen_->mouse_move(cursor_.x(), cursor_.y());
    else mouse_queue_.move(cursor_.x(), cursor_.y(), mouse_buttons_);
}

// @orig 1360:045D
void Game::mouse_button(int button, bool pressed, int x, int y) {
    if (button != 1 && button != 2) return;
    const std::uint16_t bit = button == 1 ? 1 : 2;
    mouse_buttons_ = static_cast<std::uint16_t>(pressed ? mouse_buttons_ | bit : mouse_buttons_ & ~bit);
    cursor_.move_to(x, y);
    if (!modal()) {
        if (pressed) mouse_queue_.press(button == 2, cursor_.x(), cursor_.y(), mouse_buttons_);
        else mouse_queue_.release(button == 2, cursor_.x(), cursor_.y(), mouse_buttons_);
        return;
    }
    // The port's menus and screens take the left button at once, without
    // the double-click delay of the queue.
    if (button != 1) return;
    if (message_) {
        if (pressed) message_->mouse_down(cursor_.x(), cursor_.y());
        else message_->mouse_up(cursor_.x(), cursor_.y());
        if (message_->closed()) close_message();
        return;
    }
    if (screen_) {
        if (pressed) screen_->mouse_down(cursor_.x(), cursor_.y());
        else screen_->mouse_up(cursor_.x(), cursor_.y());
        if (screen_->closed()) close_screen();
        else if (const Palette* p = screen_->palette()) world_->state.screen_palette = *p;
        return;
    }
    if (!menu_) return;
    if (pressed) {
        menu_->mouse_down(cursor_.x(), cursor_.y());
        follow_menu_screen();
    } else {
        menu_->mouse_up(cursor_.x(), cursor_.y());
        follow_menu_screen();
        if (menu_->result()) close_menu();
    }
}

// @orig 1328:0624; the mouse events, then the idle event when there were none
void Game::dispatch_mouse() {
    bool any = false;
    while (const auto e = mouse_queue_.next()) {
        // Queued in play, but a menu or a screen opened since: the original
        // gives them to it; the port's menus had theirs directly.
        if (!mouse_in_play()) continue;
        any = true;
        world_->state.mouse_turn.speed = config_.mouse_speed;
        if (world_mouse_->event(*e)) cursor_.move_to(WorldMouse::kCentreX, WorldMouse::kCentreY);
    }
    if (!any && mouse_in_play()) world_mouse_->idle();
}

namespace {

// Copies a w x h block between two surfaces (both clipped by the caller's choice of sizes).
void copy_block(const Surface8& src, int sx, int sy, Surface8& dst, int dx, int dy, int w, int h) {
    for (int y = 0; y < h; ++y) {
        std::copy_n(src.row(sy + y) + sx, w, dst.row(dy + y) + dx);
    }
}

}  // namespace

// @orig none: port-only native resolution
// Runs `draw` on the 640 x 480 block of `screen` whose top-left is (x0, y0);
// the screens, menus and status bar are laid out for that size.
template <typename Draw>
void Game::draw_window(Surface8& screen, int x0, int y0, Draw&& draw) {
    if (screen.width == kScreenWidth && screen.height == kScreenHeight) {
        draw(screen);
        return;
    }
    if (window_.width != kScreenWidth) window_ = Surface8(kScreenWidth, kScreenHeight);
    copy_block(screen, x0, y0, window_, 0, 0, kScreenWidth, kScreenHeight);
    draw(window_);
    copy_block(window_, 0, 0, screen, x0, y0, kScreenWidth, kScreenHeight);
}

// @orig 1188:19C1
void Game::render(Surface8& screen, RenderStats* stats) {
    // A screen larger than 640 x 480 (native resolution) shows more of the
    // scene around the camera; everything else stays 640 x 480, centred.
    const bool large = screen.width >= kScreenWidth && screen.height >= kScreenHeight &&
                       (screen.width != kScreenWidth || screen.height != kScreenHeight);
    ui_x_ = large ? (screen.width - kScreenWidth) / 2 : 0;
    ui_y_ = large ? (screen.height - kScreenHeight) / 2 : 0;
    if (!large && (screen.width != kScreenWidth || screen.height != kScreenHeight)) {
        screen = Surface8(kScreenWidth, kScreenHeight);
    }
    if (movie_) {
        if (!large) {
            screen = movie_->screen();
        } else {
            screen.clear(0);
            copy_block(movie_->screen(), 0, 0, screen, ui_x_, ui_y_, kScreenWidth, kScreenHeight);
        }
        return;
    }
    if (menu_ || (screen_ && !screen_->over_view())) {
        if (large) screen.clear(0);
        draw_window(screen, ui_x_, ui_y_, [this](Surface8& s) {
            if (menu_) menu_->draw(s);
            else screen_->draw(s);
            cursor_.draw(s);
        });
        return;
    }
    render_view(screen, stats);
    draw_window(screen, ui_x_, ui_y_, [this](Surface8& s) {
        draw_autosave_icon(s);
        if (message_) message_->draw(s);
        if (screen_) screen_->draw(s);
        cursor_.draw(s);
    });
}

// @orig 1188:12F1
void Game::render_view(Surface8& screen, RenderStats* stats) {
    screen.clear(0);
    world_->set_view_size(screen.width, screen.height);
    const GameState& s = world_->state;
    const auto list =
        build_display_list(*world_, shapes_, View{s.view_left, s.view_top}, screen.width, screen.height, stats);
    // DLIST.C 11A0:05xx: right after item 1, in combat (or holstering,
    // last action 0xB), its weapon (WPNOVLAY.DAT), unmirrored, at the item's
    // screen place plus the offsets of the current action, direction, frame.
    draw_display_list(screen, list, &xform_, [this](Surface8& surface, const DrawItem& d) {
        if (d.item->id != 1 || d.item->shape != 1) return;
        const auto record = world_->npc_record(1);
        const std::uint16_t last = static_cast<std::uint16_t>(record[npc::kLastAnim] | record[npc::kLastAnim + 1] << 8);
        if (!in_combat(*world_, 1) && last != 0xB) return;
        const Item* weapon = world_->get(world_->active_weapon(1));
        const AnimAction* header = anims_.action(1, last);
        if (!weapon || !header) return;
        const auto piece =
            weapon_overlay_.piece(weapon->shape, last, *header, world_->npc_direction(1), record[npc::kAnimFrame]);
        if (!piece || (piece->shape == 0 && piece->frame == 0)) return;
        const Shape* shape = shapes_.get(piece->shape);
        if (!shape || piece->frame >= shape->frames().size()) return;
        shape->frames()[piece->frame].draw(surface, d.sx + piece->dx, d.sy + piece->dy, false);
    });
    // SPMGR.C: the screen sprites (aiming mode), over the scene, unless
    // suspended in the menu mode (DS:2D8F).
    for (const ScreenSprite& sprite : world_->state.screen_sprites.all()) {
        if (sprites_suspended_) break;
        if (sprite.shape == 0) continue;
        const Shape* shape = shapes_.get(sprite.shape);
        if (!shape || sprite.frame >= shape->frames().size()) continue;
        shape->frames()[sprite.frame].draw(screen, ((sprite.x - sprite.y) >> 1) - s.view_left,
                                           ((sprite.x + sprite.y) >> 2) - sprite.z - s.view_top);
    }
    // SPMGR_11a8_05f6: the frame of the item selection, over the scene at
    // the item's centre (getCX, getCY, getCZ).
    if (const Item* selected = world_->get(selected_item(*world_)); selected && !sprites_suspended_) {
        const ShapeInfo* info = world_->shape_info(selected->shape);
        const int cx = static_cast<std::uint16_t>(selected->x - (info ? info->footpad_x : 0) * 8);
        const int cy = static_cast<std::uint16_t>(selected->y - (info ? info->footpad_y : 0) * 8);
        const int cz = (world_->location(*selected).z + (info ? info->footpad_z : 0) * 4) & 0xFF;
        const Shape* frame = shapes_.get(kSelectionFrameShape);
        if (frame && !frame->frames().empty()) {
            frame->frames()[0].draw(screen, ((cx - cy) >> 1) - s.view_left, ((cx + cy) >> 2) - cz - s.view_top);
        }
    }
    // The status bar is laid out for 640 pixels: centred at the bottom.
    draw_window(screen, ui_x_, screen.height - kScreenHeight,
                [this](Surface8& s) { status_bar_.draw(s, *world_); });
    // DISPLAY_1190_010f, not in the menu mode (DS:2CEE, set with DS:2D8F).
    if (!sprites_suspended_) pickup_area_.draw(screen, world_->state.pickups);
}

void Game::move_camera(std::int32_t dx, std::int32_t dy) {
    const GameState& s = world_->state;
    world_->set_camera(s.camera_x + dx, s.camera_y + dy, s.camera_z);
}

}  // namespace crusader
