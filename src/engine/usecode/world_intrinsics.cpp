#include "engine/usecode/world_intrinsics.hpp"

#include "engine/kernel.hpp"
#include "engine/palette_fader.hpp"
#include "engine/usecode/uc_process.hpp"
#include "engine/world/attack.hpp"

#include <array>
#include <memory>
#include <initializer_list>
#include <string>
#include <utility>

namespace crusader {

namespace {

using Args = std::span<const std::uint8_t>;

std::uint16_t arg16(Args a, std::size_t pos) {
    return pos + 2 <= a.size() ? static_cast<std::uint16_t>(a[pos] | a[pos + 1] << 8) : 0;
}

}  // namespace

// @orig 10E0:0000 10E0:0121
void set_alert(World& world, bool on) {
    world.state.alert_active = on;
    // A search of the whole map ("@ %n = ... + $"), cell by cell.
    std::vector<ObjId> lights;
    for (int cy = 0; cy < World::kGridSize; ++cy) {
        for (int cx = 0; cx < World::kGridSize; ++cx) {
            for (ObjId id : world.cell(cx, cy)) {
                const std::uint16_t s = world.get(id)->shape;
                if (s == 0x49 || s == 0x21 || s == 0x174 || s == 0x271 || s == 0x477) lights.push_back(id);
            }
        }
    }
    for (ObjId id : lights) {
        Item& item = *world.get(id);
        if (item.shape == 0x477) {
            if (on && item.frame < 2) world.set_frame(item, static_cast<std::uint16_t>(item.frame + 2));
            if (!on && item.frame > 1) world.set_frame(item, static_cast<std::uint16_t>(item.frame - 2));
        } else if (item.frame == (on ? 0 : 1)) {
            world.set_frame(item, on ? 1 : 0);
        }
    }
}

// @orig none: binds the intrinsics, numbered by USECODE/UNKCOFF.DAT
std::vector<std::uint16_t> register_world_intrinsics(UCMachine& m, World& w, const AnimDat* anims) {
    std::vector<std::uint16_t> bound;
    auto bind = [&](std::initializer_list<std::uint16_t> numbers, UCMachine::Intrinsic fn) {
        for (std::uint16_t n : numbers) {
            m.set_intrinsic(n, fn);
            bound.push_back(n);
        }
    };
    GameState& s = w.state;

    // Ultima8Engine::I_getCurrentTimerTick: kernel ticks (60 Hz).
    bind({0x0B4}, [](UCMachine& machine, UCProcess&, Args) { return machine.kernel().ticks(); });
    // UCMachine::I_rndRange(lo, hi): arguments in reverse push order, signed.
    bind({0x018}, [&s](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        const auto lo = static_cast<std::int16_t>(arg16(a, 0)), hi = static_cast<std::int16_t>(arg16(a, 2));
        return static_cast<std::uint16_t>(random_between(s.random_state, lo, hi));
    });
    bind({0x000}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t { return s.alert_active; });
    bind({0x0B5}, [&w](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        set_alert(w, true);
        return 0;
    });
    bind({0x0B7}, [&w](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        set_alert(w, false);
        return 0;
    });
    bind({0x099}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        if (s.vargas_shield != 0) s.vargas_shield = 500;
        return 0;
    });
    // UCMachine::I_true (1050:00C1): character 13 of "GAME COMPILE=1" (DS:0908) is '1'.
    bind({0x02C}, [](UCMachine&, UCProcess&, Args) -> std::uint32_t { return 1; });
    // Violence (1050:00A1): character 9 of "VIOLENCE=1" (DS:090C) is '1' in
    // this build. Gates the blood under the bodies (NPCDEATH) and the bodies'
    // reaction to shots (D_GUARD... gotHit).
    bind({0x06D}, [](UCMachine&, UCProcess&, Args) -> std::uint32_t { return 1; });
    bind({0x029}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t { return s.difficulty; });
    bind({0x01B}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t { return s.controlled_npc; });
    // Ultima8Engine::I_setAvatarInStasis (10F0:3696): the flag; a living
    // player in combat leaves it (Actor::clearInCombatQuiet, NPC_10f0_32a0;
    // its global quiet flag has no reader in this version).
    bind({0x00C}, [&s, &w, anims](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        s.avatar_in_stasis = arg16(a, 0) != 0;
        if (s.avatar_in_stasis && anims && in_combat(w, 1) && !w.is_dead(1))
            clear_in_combat(machine.kernel(), w, *anims, 1);
        return 0;
    });
    bind({0x0B8}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t { return s.avatar_in_stasis; });
    // Ultima8Engine::I_getCrusaderTeleporting / I_setCrusaderTeleporting /
    // I_clrCrusaderTeleporting (13E8:0E27 / 0E2B / 0E21): DS:6000.
    bind({0x071}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t { return s.crusader_teleporting; });
    bind({0x072}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.crusader_teleporting = true;
        return 0;
    });
    bind({0x075}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.crusader_teleporting = false;
        return 0;
    });
    bind({0x073}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.cru_stasis = true;
        return 0;
    });
    bind({0x076}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.cru_stasis = false;
        return 0;
    });
    // 13E8:0033 / 0039 (unnamed in ScummVM): DS:5FFE, whether the next
    // cutscenes leave the colour cycling paused when they end.
    bind({0x092}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.movie_keeps_cycle_paused = true;
        return 0;
    });
    bind({0x094}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.movie_keeps_cycle_paused = false;
        return 0;
    });
    // 1188:0013 (unnamed in ScummVM): the camera process redraws the whole
    // view (vtable +0x2C) and puts it on the screen (+0x30) at once. The port
    // draws the view at every frame: nothing to do.
    bind({0x064}, [](UCMachine&, UCProcess&, Args) -> std::uint32_t { return 0; });
    // CameraProcess::I_setCenterOn(item) (1188:1AA5): the item the view
    // follows (DAT_2C50); 0 = none, the view stays where it is.
    bind({0x043}, [&w](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        w.state.camera_target = arg16(a, 0);
        return 0;
    });
    // Kernel::I_getNumProcesses(item, type), Kernel::I_resetRef(item, type):
    // resetRef fails the game-level (level 1) processes of the item and type.
    bind({0x0EF}, [](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        return static_cast<std::uint32_t>(machine.kernel().count_processes(arg16(a, 0), arg16(a, 2)));
    });
    bind({0x06E}, [](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        machine.kernel().fail_processes(arg16(a, 0), arg16(a, 2), 1);
        return 0;
    });
    // CameraProcess::I_moveTo(x, y, z, map): the camera position, which
    // recentres the fast area; a map other than 0 and the current one is
    // loaded first. Not reproduced: the screen tile cache (6 x 5 tiles of
    // 128 pixels) and sound source updates.
    bind({0x042}, [&w](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        w.move_camera_to(arg16(a, 0), arg16(a, 2), arg16(a, 4), arg16(a, 6) & 0xFF);
        return 0;
    });
    bind({0x087}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t { return static_cast<std::uint32_t>(s.camera_x); });
    bind({0x088}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t { return static_cast<std::uint32_t>(s.camera_y); });
    bind({0x0D6}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t { return static_cast<std::uint32_t>(s.camera_z); });
    // CruStatusGump::I_hideStatusGump / I_showStatusGump (SPMGR_11b8_098f /
    // 0932). Not reproduced: the mouse is also limited to y <= 439 while the
    // bar is shown (479 otherwise) and put back at (320, 240).
    bind({0x05F}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.status_bar_visible = false;
        return 0;
    });
    bind({0x061}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.status_bar_visible = true;
        return 0;
    });
    // PaletteFaderProcess (FADE.C, see engine/palette_fader.hpp). The fades
    // return the pid of the fade process; `resume` is its flag +0x41. The
    // fades to black pause the colour cycling first.
    auto fade = [&s](UCMachine& machine, const Palette& target, std::uint32_t rate, int count,
                     bool resume) -> std::uint32_t {
        return machine.kernel().add(std::make_unique<PaletteFaderProcess>(s.screen_palette, target, rate, count, 1,
                                                                          &s.cycle_paused, resume));
    };
    const Palette black = Palette::filled(0, 0, 0);
    bind({0x078}, [fade, black, &s](UCMachine& machine, UCProcess&, Args) {
        s.cycle_paused = true;
        return fade(machine, black, PaletteFaderProcess::kDefaultRate, 0, false);
    });
    bind({0x09E}, [fade, black, &s](UCMachine& machine, UCProcess&, Args a) {
        s.cycle_paused = true;
        return fade(machine, black, arg16(a, 0), 0, false);
    });
    bind({0x09F}, [fade, black](UCMachine& machine, UCProcess&, Args a) {
        return fade(machine, black, arg16(a, 0), static_cast<std::int16_t>(arg16(a, 2)), true);
    });
    // fadeFromBlack / fadeToGamePal(rate): the pid of a process waiting for
    // the fade, which resumes the colour cycling.
    auto fade_to_game = [fade, &s](UCMachine& machine, std::uint32_t rate) -> std::uint32_t {
        const Pid f = static_cast<Pid>(fade(machine, s.game_palette, rate, 0, false));
        return machine.kernel().add(std::make_unique<FadeWaitProcess>(f, s.cycle_paused));
    };
    bind({0x07B}, [fade_to_game](UCMachine& machine, UCProcess&, Args) {
        return fade_to_game(machine, PaletteFaderProcess::kDefaultRate);
    });
    bind({0x09C}, [fade_to_game](UCMachine& machine, UCProcess&, Args a) { return fade_to_game(machine, arg16(a, 0)); });
    bind({0x09D}, [fade, &s](UCMachine& machine, UCProcess&, Args a) {
        return fade(machine, s.game_palette, arg16(a, 0), static_cast<std::int16_t>(arg16(a, 2)), true);
    });
    // fadeToGivenColor(r, g, b, rate, count): the original stops with a fatal
    // assertion when a component is outside 0..63; the port masks it.
    bind({0x0A0}, [fade](UCMachine& machine, UCProcess&, Args a) {
        const Palette colour = Palette::filled(static_cast<std::uint8_t>(arg16(a, 0)), static_cast<std::uint8_t>(arg16(a, 2)),
                                               static_cast<std::uint8_t>(arg16(a, 4)));
        return fade(machine, colour, arg16(a, 6), static_cast<std::int16_t>(arg16(a, 8)), true);
    });
    bind({0x090}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.cycle_paused = true;
        s.screen_palette = Palette::filled(0, 0, 0);
        return 0;
    });
    bind({0x0DA}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.cycle_paused = true;
        s.screen_palette = Palette::filled(63, 63, 63);
        return 0;
    });
    bind({0x09B}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.screen_palette = s.game_palette;
        s.cycle_paused = false;
        return 0;
    });
    // jumpToGreyScale (1030:0605): the luminance of the game palette.
    bind({0x098}, [&s](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        s.cycle_paused = true;
        s.screen_palette = greyscale(s.game_palette);
        return 0;
    });
    // UCMachine::I_true.
    bind({0x0A7}, [](UCMachine&, UCProcess&, Args) -> std::uint32_t { return 1; });
    // UCMachine::I_numToStr (1418:0073): itoa(value, buffer, 10), signed,
    // into a new string.
    bind({0x0FF, 0x113, 0x122}, [](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        return machine.heap().new_string(std::to_string(static_cast<std::int16_t>(arg16(a, 0))));
    });
    return bound;
}

}  // namespace crusader
