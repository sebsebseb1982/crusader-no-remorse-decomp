#include "engine/world/teleport.hpp"

#include "engine/audio/audio.hpp"
#include "engine/world/aim_mode.hpp"
#include "engine/world/gravity.hpp"

#include <array>
#include <cstdio>

namespace crusader {

namespace {

constexpr ObjId kPlayer = 1;
constexpr std::uint16_t kPlayerAnimType = 0xF0;
constexpr std::uint8_t kFamilyEgg = 8;

// DS:0F89: set while a failed teleport takes the player back.
bool g_returning = false;

// Process DS:1006 (1098:0000 / 0047): raises the busy flag while it lives;
// it waits for the teleport, then ends at once, putting the flag back.
// @orig 1098:0000 1098:0047
class BusyUntilTeleported : public Process {
public:
    explicit BusyUntilTeleported(World& world) : world_(world), was_busy_(world.state.avatar_in_stasis) {
        world_.state.avatar_in_stasis = true;
    }

protected:
    void run() override { terminate(); }
    void on_terminate() override {
        if (!was_busy_) world_.state.avatar_in_stasis = false;
    }

private:
    World& world_;
    bool was_busy_;
};

// @orig 1098:011F 1098:022D
class TeleportToEgg : public Process {
public:
    TeleportToEgg(Kernel& kernel, World& world, int map, std::uint16_t egg)
        : kernel_(kernel), world_(world), map_(map), egg_(egg) {}

protected:
    void run() override {
        if (g_returning) {
            terminate();
            return;
        }
        Item* player = world_.get(kPlayer);
        if (!player) {
            terminate();
            return;
        }
        const std::int32_t old_x = player->x, old_y = player->y, old_z = player->z;
        const int old_map = player->map_number;
        const bool same_map = map_ == world_.current_map();
        if (!same_map) world_.teleport(kPlayer, 0, 0, 0, map_);

        const ObjId egg = find_teleport_egg(world_, egg_);
        if (egg == 0) {
            if (!same_map) {
                g_returning = true;
                world_.teleport(kPlayer, old_x, old_y, old_z, old_map);
                g_returning = false;
            }
            std::fprintf(stderr, "No destination for this egg! (map %d, egg %u)\n", map_, egg_);
        } else {
            const Item& e = *world_.get(egg);
            const std::int32_t x = e.x, y = e.y, z = e.z;
            world_.teleport(kPlayer, x, y, z, map_);
            player = world_.get(kPlayer);
            if (!(support_mask(world_, player->shape, x, y, z, true, kPlayer) & 0x10)) fall(kernel_, world_, kPlayer);
            // Port addition: a mission starts (autosave).
            if (egg_ == kTeleportStartEgg && world_.hooks().autosave_soon) world_.hooks().autosave_soon();
        }
        terminate();
    }

private:
    Kernel& kernel_;
    World& world_;
    int map_;
    std::uint16_t egg_;
};

// SHAPHAND_1098_01f4: the player's animations end; with `wait`, the busy
// flag is up until the teleport is over.
// @orig 1098:01F4
Pid spawn_teleport(Kernel& kernel, World& world, int map, std::uint16_t egg, bool wait) {
    kernel.fail_processes(kPlayer, kPlayerAnimType, 1);
    Process& teleport = kernel.spawn<TeleportToEgg>(kernel, world, map, egg);
    if (wait && !g_returning) kernel.spawn<BusyUntilTeleported>(world).wait_for(teleport.pid());
    return teleport.pid();
}

// @orig 1098:068B 1098:0718
class EggHatcher : public Process {
public:
    static constexpr std::uint16_t kType = 0x20F;

    EggHatcher(Kernel& kernel, World& world, ObjId egg) : kernel_(kernel), world_(world) {
        set_owner(egg);
        set_type(kType);
    }

protected:
    void run() override {
        const Item* egg = world_.get(owner());
        const Item* player = world_.get(kPlayer);
        if (!egg || !player || world_.family_of(egg->shape) == 7) return;
        const ShapeInfo* info = world_.shape_info(player->shape);
        const int foot_x = info ? info->footpad_x : 0, foot_y = info ? info->footpad_y : 0;
        const int range_x = egg->npc_number >> 4, range_y = egg->npc_number & 0xF;
        auto u16 = [](std::int32_t v) { return static_cast<std::uint16_t>(v); };
        const std::uint16_t px = u16(player->x), py = u16(player->y);
        bool inside = !(px < u16(egg->x - range_x * 32) || u16(egg->x + range_x * 32) < u16(px - foot_x * 16 + 1) ||
                        py < u16(egg->y - range_y * 32) || u16(egg->y + range_y * 32) < u16(py - foot_y * 16 + 1));
        const int pz = player->z & 0xFF, ez = egg->z & 0xFF;
        if (inside && (ez + 0x30 < pz || pz < ez - 0x30)) inside = false;
        if (!hatched_) {
            if (inside) {
                hatched_ = true;
                hatch(*egg);
            }
        } else if (!inside) {
            hatched_ = false;
            if (world_.hooks().start_event) world_.hooks().start_event(owner(), 0x15);
        }
    }

private:
    // @orig 1098:05FD
    void hatch(const Item& egg) {
        if (world_.family_of(egg.shape) == 8) {
            spawn_teleport(kernel_, world_, egg.map_number, egg.quality & 0xFF, (egg.quality >> 8) != 0);
        } else if (world_.hooks().start_event) {
            world_.hooks().start_event(egg.id, 7);
        }
    }

    Kernel& kernel_;
    World& world_;
    bool hatched_ = false;
};

// Process DS:0566 (1028:0477).
// @orig 1028:0436 1028:0477 1138:3181
class SwitchMap : public Process {
public:
    SwitchMap(Kernel& kernel, World& world, int map) : kernel_(kernel), world_(world), map_(map) {}

protected:
    void run() override {
        // The colour cycling pauses; GAMEPAL_1028_03f6, this process
        // included (not the Controller, reset instead: 1138:3181); then
        // the teleport.
        world_.state.cycle_paused = true;
        kernel_.fail_all();
        leave_aim_mode(kernel_, world_);
        if (world_.hooks().reset_fire) world_.hooks().reset_fire();
        GameState::Aim& aim = world_.state.aim;
        aim.marker_pid = 0;
        aim.target = 0;
        aim.marker = -1;
        aim.marked_dir = 0x10;
        aim.turns = 0;
        if (Audio* audio = world_.audio()) {
            audio->stop_all();
            audio->sources.clear();
        }
        start_teleport_to_egg(kernel_, world_, map_, kTeleportStartEgg, true, false);
    }

private:
    Kernel& kernel_;
    World& world_;
    int map_;
};

}  // namespace

// @orig 1098:022D
ObjId find_teleport_egg(const World& world, std::uint16_t egg) {
    const std::array<std::uint8_t, 18> script = {
        ':', '%', kFamilyEgg, 0, '=', '`', '%', 1, 0, '=', '&', '*', '%', 0xFF, 0, '&', '%', 0,
    };
    std::array<std::uint8_t, 24> code{};
    std::copy(script.begin(), script.end(), code.begin());
    code[17] = static_cast<std::uint8_t>(egg);
    code[18] = static_cast<std::uint8_t>(egg >> 8);
    code[19] = '=';
    code[20] = '&';
    code[21] = '$';
    // The whole map (AreaSearch_init with every bound negative), first match.
    for (ObjId id : world.rect_search(0, 0, 0x7FFF, 0x7FFF)) {
        if (world.matches_loop_script(*world.get(id), std::span(code).first(22))) return id;
    }
    return 0;
}

// @orig 1098:0072 1098:019C
Pid start_teleport_to_egg(Kernel& kernel, World& world, int map, std::uint16_t egg, bool wait, bool stop_sounds) {
    const Pid pid = spawn_teleport(kernel, world, map, egg, wait);
    // Only the explicit-map constructor stops the sounds (1098:011F); the
    // current-map one (1098:019C) leaves them playing.
    if (stop_sounds) {
        if (Audio* audio = world.audio()) {
            audio->sources.clear();
            audio->stop_all();
        }
    }
    return pid;
}

Pid start_switch_map(Kernel& kernel, World& world, int map) {
    return kernel.spawn<SwitchMap>(kernel, world, map).pid();
}

Pid start_egg_hatcher(Kernel& kernel, World& world, ObjId egg) {
    return kernel.spawn<EggHatcher>(kernel, world, egg).pid();
}

}  // namespace crusader
