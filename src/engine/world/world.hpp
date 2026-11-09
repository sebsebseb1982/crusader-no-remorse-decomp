#pragma once
// Items and maps (ITEM.C, ITEMDATA.C, GLOB.C, ...).
//
// FIXED.DAT is a Flex with one entry per map; an entry is an array of 16-byte
// records:
//   u16 x, u16 y, u8 z, u16 shape, u8 frame, u16 flags, u16 quality,
//   u8 npc number, u8 map number, u16 next
// Coordinates are in usecode units (what intrinsics such as getX return).
// Items stored inside a container follow it; their x field is then the
// nesting depth instead of a coordinate. In FIXED.DAT all 107 containers are
// empty (contents come from saved games), but the rule is kept for them.
// ~0.7% of the original records reference shapes or frames that do not exist;
// they are loaded as-is (the original engine skips what it cannot create).

#include "core/flex.hpp"
#include "core/palette.hpp"
#include "core/random.hpp"
#include "core/trig.hpp"
#include "core/typeflags.hpp"
#include "engine/process.hpp"
#include "engine/world/pickup_list.hpp"
#include "engine/world/screen_sprites.hpp"

#include <array>
#include <bitset>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace crusader {

struct Item {
    ObjId id = 0;
    std::uint16_t shape = 0;
    // 8 bits in the item table; NPCs (ids 1..255) keep the high byte in their
    // NPC record (+0x0F), see World::set_frame.
    std::uint16_t frame = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint16_t flags = 0;  // "status" for usecode (getStatus/orStatus/andStatus)
    std::uint16_t quality = 0;
    std::uint8_t npc_number = 0;
    std::uint8_t map_number = 0;
    ObjId parent = 0;  // containing item, 0 when on the map
    std::vector<ObjId> contents;  // in the original's list order
    std::uint8_t hit_points = 0;  // from DAMAGE.FLX at creation (item table array 12)
    // Item table array 11, not decoded yet (rarely non-zero), kept raw.
    std::uint16_t save_u2 = 0;
    // In a container but out of its chain in the saved game it came from
    // (weapons equipped by some NPCs; the original's reason is not decoded):
    // written back out of the chain.
    bool unchained = false;

    // Port only: a projectile drawn as an item (the original draws a sprite,
    // outside the world). Collisions ignore it.
    bool sprite_only = false;
    // An NPC kept while its map is not loaded: in no list, neither a map
    // cell nor the pending stack (ITEMCACH_10b0_04c9 unlinks it). Also the
    // player before the first map of a new game.
    bool off_map = false;
};

// Item flags (Item::flags, "status" for usecode).
// Stored inside a container. As in the original's item table, x then holds
// the container id (getX/getY/getZ return the outermost container's position).
constexpr std::uint16_t kItemContained = 0x0008;
// Out of the world: on the pending-item stack (Item::I_create, I_push). Not
// drawn. Freed slots keep it too (the 264 / 1143 slots at z = 254 that have
// it in SAVEGAME.1 / .13 are all in the free lists, dead NPCs among them).

constexpr std::uint16_t kItemEthereal = 0x0800;
// Set on items created by an egg (glob eggs add 0x80; monster eggs' NPCs):
// such an NPC takes the events of its shape's class (World::usecode_class).
constexpr std::uint16_t kItemFromEgg = 0x0002;
// Created by a glob egg (with 0x02): destroyed when its cell leaves the fast area.
constexpr std::uint16_t kItemDisposable = 0x0080;
// In one of its container NPC's equipment slots (Actor::I_setEquip); its z
// is the slot.
constexpr std::uint16_t kItemEquipped = 0x0200;
// Inside the fast area (enterFastArea / leaveFastArea).
constexpr std::uint16_t kItemFastArea = 0x2000;
// Gravity (1040:06D2): set when the last step of a fall was blocked; such
// an item makes others leave it at each step. Low friction (0x4000) would
// make a landed item roll on; nothing sets it in this version.
constexpr std::uint16_t kItemBouncing = 0x0400;
constexpr std::uint16_t kItemLowFriction = 0x4000;
// Broken (Item::I_setBroken; dead NPCs): not aimed at.
constexpr std::uint16_t kItemBroken = 0x8000;

// Shape families (TYPEFLAG.DAT) whose quality heads a list of contents (ITEM.C).
constexpr std::uint8_t kFamilyContainer = 6;
constexpr std::uint8_t kFamilyActorContainer = 7;

// Item ids 1..255 are NPCs (Actor::I_isNPC); other items come after.
inline bool is_npc_id(ObjId id) { return id != 0 && id <= 0xFF; }
constexpr ObjId kFirstItemId = 0x100;
// Slots of the item table (ids 0x100..0x27FF for items, 1..0xFF for NPCs).
constexpr std::size_t kItemSlots = 0x2800;

struct FixedRecord {
    std::uint16_t x, y;
    std::uint8_t z;
    std::uint16_t shape;
    std::uint8_t frame;
    std::uint16_t flags, quality;
    std::uint8_t npc_number, map_number;
    std::uint16_t next;
};

std::vector<FixedRecord> read_fixed_map(const FlexArchive& fixed, std::size_t map);

// A "glob" (GLOB.FLX entry) is a prefab group of items placed by a glob egg
// (shape family 3, 881 of them in map 1, shape 16): most floors and walls of a
// level come from globs. Entry: u16 count, then count * { u8 x, u8 y, u8 z,
// u16 shape, u8 frame }. Expansion, in world units (GLOBEGG.C behaviour):
//   x = (egg.x & ~0x3FF) + 4 * gx + 2    (same for y)    z = egg.z + gz
struct GlobItem {
    std::uint8_t x, y, z;
    std::uint16_t shape;
    std::uint8_t frame;
};
std::vector<GlobItem> read_glob(const FlexArchive& globs, std::size_t index);

// Global game state read and written by usecode.
struct GameState {
    bool alert_active = false;        // World::I_getAlertActive
    // DS:2895: Vargas's shield (shape 0x3AC), 5000 at start; hits take from
    // it until it is empty. World::I_resetVargasShield puts it back to 500
    // unless it is already empty.
    std::int16_t vargas_shield = 5000;
    std::uint16_t difficulty = 1;     // World::I_gameDifficulty
    std::uint16_t battery = 0;        // DS:7E1F, MainActor::I_getMaxEnergy
    std::uint32_t keycards = 0;       // DS:7E1B, bit n: keycard n (MainActor::I_hasKeycard)
    // The 21-byte player block at DS:7E0E (saved game entry 20) as last
    // loaded; the fields the port keeps elsewhere are written over it when
    // saving (+2 stasis, +4 random seed, +0xB difficulty, +0xD keycards,
    // +0x11 battery). Bytes +8..+0xA and +0x13..+0x14 are not decoded.
    std::array<std::uint8_t, 0x15> player_block{};

    std::uint16_t controlled_npc = 1; // World::I_getControlledNPCNum (1 = the player)
    int frameskip = 0;                 // DS:2887; animation frame skipping (0 or 3 in CRUSADER.CFG)
    // The AutoFirer (1138:2FFB): the process that fires weapon 909 (fire type 6) at 6 Hz while
    // the player holds the fire key (DS:2871, 0 when there is none); the fire key flag of the
    // last Controller_fire call (DS:28CD); a shot of the AutoFirer under way (DS:2876).
    Pid auto_firer = 0;
    bool fire_held = false;
    bool auto_firing = false;
    // The large shield's sprite (0x52B), which follows the player (FR DS:2875,
    // US g_281d; cleared at its end through the avatar mover, event 1), and its item.
    Pid shield_sprite = 0;
    ObjId shield_sprite_item = 0;
    // DS:7E10 (Ultima8Engine::I_setAvatarInStasis / I_getAvatarInStasis):
    // the player is busy (a healer or charger serving him, a teleport with
    // wait, the scripts); the keys of World_on_key that act on the player do
    // nothing.
    bool avatar_in_stasis = false;
    // DS:5FFB (Ultima8Engine::I_setCruStasis / I_clrCruStasis): the keys
    // of WORLD_13e8_14b4 (Escape to the menu, item and save keys) are ignored.
    bool cru_stasis = false;
    // DS:6000 (Ultima8Engine::I_get / set / clrCrusaderTeleporting).
    bool crusader_teleporting = false;
    // DS:2889: target designation, on at start-up (data segment), toggled by
    // R. The original keeps it for the whole session, loads included.
    bool target_designation = true;
    // The cheats (cheats.hpp), kept for the whole session as in the original.
    struct Cheats {
        bool enabled = false;          // DS:08F0: typed code or "-laurie"
        bool on = false;               // DS:5FF5: F10 works (~ toggles it)
        bool hack_mover_allowed = false;  // DS:8C3A: set by the typed code
        int typed = 0;                 // DS:28C7: letters of the code typed so far
        bool cd_transfer_display = false;  // DS:5FFF (Ctrl+Q)
        bool bounding_boxes = false;   // DS:2D7C (f): not drawn by the port
    } cheats;
    // The aiming mode of the controlled NPC and the designated target
    // (aim_mode.hpp). The original resets it when the Controller starts
    // (1138:3181), as the port does with every new game state.
    struct Aim {
        bool active = false;           // DS:2863
        bool no_stand = false;         // DS:2864: the controlled shape has no action 1
        bool crosshair_dirty = false;  // DS:2862
        int angle = 1000;              // DS:285E, degrees; above 360: taken from the direction
        int crosshair = -1;            // DS:2860, screen sprite slot of the crosshair
        int turns = 0;                 // DS:288A, turning animations under way
        ObjId target = 0;              // DS:289B, the designated target
        int marker = -1;               // DS:288C, screen sprite slot of its marker
        Pid marker_pid = 0;            // DS:288E, the marker's SpriteProcess
        int marked_dir = 0x10;         // DS:2890, direction of the last designation
        std::uint32_t checked_at = 0;  // DS:2891, tick of the last designation
        std::uint32_t timer_at = 0;    // DumbTimer +0x40
    } aim;
    // The mouse turning the controlled NPC (world_mouse.hpp, 13E8:2A13):
    // set while the arrow key it sends is handled (DS:5FF6), with its rate
    // (DS:5FF8); the mouse speed of the options (DS:6001, 0..3); the tick of
    // the last walk it turned (DS:2883, 1138:0F8D).
    struct MouseTurn {
        bool active = false;
        int rate = 0;
        int speed = 2;
        std::uint32_t redirected_at = 0;
    } mouse_turn;
    // SPMGR.C: sprites drawn over the scene (screen_sprites.hpp).
    ScreenSprites screen_sprites;
    // DISPLAY.C: the items just picked up (pickup_list.hpp).
    PickupList pickups;
    // The item selection mode of the game view (item_selection.hpp): gump
    // flag 0x100 and DS:5FF7 (`active`), the sorted list (+0x3F), the
    // selected node (+0x3B, -1 none), "pick everything" (+0x38) and the item
    // to pick up when the mode ends (+0x39).
    struct ItemSelection {
        bool active = false;
        std::vector<ObjId> items;
        int current = -1;
        bool pick_all = false;
        ObjId chosen = 0;
    } selection;
    // CycleProcess +0x40 (DS:6810): the palette colour cycling is paused
    // (see palette_cycle.hpp).
    bool cycle_paused = false;
    // DS:5FFE, set by intrinsic 0x092 and cleared by 0x094: a cutscene leaves
    // the colour cycling paused when it ends (player field +0xC2).
    bool movie_keeps_cycle_paused = false;
    // The two generators (core/random.hpp): SYSTEM.C's, saved with the
    // game, and the C library's rand().
    std::uint32_t random_state = kRandomStartState;
    std::uint32_t crt_rand_state = kCrtRandStartState;
    // CameraProcess::I_moveTo / I_getCameraX..Z, usecode units.
    std::int32_t camera_x = 0, camera_y = 0, camera_z = 0;
    // Screen position of the view's top-left corner (DAT_2C41 / 2C43), set
    // by set_camera; screen = ((x - y) / 2, (x + y) / 4 - z) in usecode units.
    std::int16_t view_left = 0, view_top = 0;
    // Screen surface (struct at 4094), inclusive bounds; 640 x 480 originally.
    std::int16_t screen_right = 639, screen_bottom = 479;
    // Item the camera follows (DAT_2C50); the view jumps when it nears an edge.
    ObjId camera_target = 1;
    // Status bar shown (DS:2DAC): CruStatusGump::I_show/hideStatusGump; shown
    // at the end of starting or loading a game (GAMEPAL_1028_0000).
    bool status_bar_visible = false;
    // The DAC contents (palette_read_hw / palette_write_hw), what the screen
    // shows; and the game palette GAMEPAL.PAL (DS:058C).
    Palette screen_palette;
    Palette game_palette;
    // Set when the controlled NPC starts a moving action (Actor::I_doAnim,
    // NPC.C 10F0:0245): the time (DS:2869), how long it counts (DS:286D,
    // ticks) and the spread divisor of NPC shots meanwhile (DS:2877, 1 at
    // start). Read by the aiming spread (SPRITE_1140_09c9, CRUSADER_1130_1180).
    std::uint32_t player_moved_at = 0;
    std::uint32_t player_move_window = 0;
    std::uint16_t player_move_accuracy = 1;
};

// NPC table (NPC.C, array at 7E0A; SAVEGAME entry 10): one 121-byte record
// per NPC slot 0..255. Decoded so far: +0x0C direction (0..15), +0x13 bit 0 dead.
constexpr std::size_t kNpcRecordSize = 0x79;
constexpr std::size_t kNpcSlots = 0x100;

// Item::I_getDirFromTo16 (ITEMFIND.C): direction from (x1, y1) to (x2, y2)
// in 16 steps, 0 = towards -y, clockwise; 16 when the points are the same.
std::uint8_t direction_16(std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2);
// Same in 8 steps (even values of the same scale), as used by
// Item::I_getDirToCoords / I_getDirToItem (ITEMFIND.C).
std::uint8_t direction_8(std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2);
// Item::I_getClosestDirectionInRange: direction_8 when `steps` is 8, else
// direction_16; outside [lo, hi] it becomes the nearer bound (wrapping modulo
// `steps`; ties go to hi).
int closest_direction_in_range(std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2, int steps, int lo,
                               int hi);

// Frame box used for on-screen tests (frame header: width, height, anchor).
struct FrameBox {
    int width, height, xoff, yoff;
};

// Fast area (ITEMCACH.C): the part of the map where items are active. Map
// cells are 512 usecode units (the item grid, 64 x 64 cells). With s = cx + cy
// and d = cx - cy, the area is s0 <= s <= s1 and d0 <= d <= d1: a diamond on
// the map, i.e. a rectangle on screen. Around the camera cell it spans
// s +/- 5 and d +/- 3 in the original (saved in SAVEGAME entry 1). A larger
// native-resolution view widens it (World::set_view_size).
struct FastArea {
    static constexpr std::int32_t kHalfS = 5, kHalfD = 3;  // 640 x 480
    std::int32_t s0 = 0, s1 = -1, d0 = 0, d1 = -1;  // empty by default

    static FastArea around(std::int32_t x, std::int32_t y, std::int32_t half_s = kHalfS,
                           std::int32_t half_d = kHalfD);
    bool contains_cell(std::int32_t cx, std::int32_t cy) const;
    bool contains(std::int32_t x, std::int32_t y) const { return contains_cell(x >> 9, y >> 9); }
    bool operator==(const FastArea&) const = default;
};

class Audio;

// Engine-to-usecode calls made by the world. Unset hooks do nothing.
struct WorldHooks {
    // Starts usecode event `event` of the item's class if the class has it
    // (UPROCESS.C; the original returns the new pid without running it).
    // Returns that pid, 0 when the event did not start.
    std::function<std::uint16_t(ObjId item, std::uint16_t event)> start_event;
    // Fails the item's game-level processes of any type (KERNEL.C kill routine).
    std::function<void(ObjId item)> fail_processes;
    // Same with argument bytes (pushed as the original lays them in memory).
    std::function<void(ObjId item, std::uint16_t event, std::span<const std::uint8_t> args)> start_event_args;
    // An actor's animation reached a frame that fires its weapon
    // (WPNCACHE_1160_005f): muzzle offsets of the frame, direction.
    std::function<void(ObjId actor, int muzzle_a, int muzzle_b, int muzzle_z, int dir)> fire_weapon;
    // An explosion occurred: size is 0 (small), 1 (medium) or 2 (large).
    std::function<void(int size)> explosion;
    // The currently controlled NPC lost hit points; `damage` is the actual loss.
    std::function<void(int damage)> player_damaged;
    // A living NPC entered the fast area (Item::I_enterFastArea, ITEM.C
    // 2CA8): its activity starts again.
    std::function<void(ObjId npc)> npc_entered_fast_area;
    // The item's usecode class has this event (ITEM.C 275F reads the class flags).
    std::function<bool(ObjId item, std::uint16_t event)> has_event;
    // A monster egg (family 7) entered the fast area.
    std::function<void(ObjId egg)> monster_egg;
    // A trigger egg (family 4, or 8 at frame 0) entered the fast area: its
    // EggHatcher process starts (SHAPHAND_1098_068b).
    std::function<void(ObjId egg)> egg_hatcher;
    // The player landed after a long fall (GravityProcess end, 1040:04BC).
    std::function<void(ObjId npc)> hard_landing;
    // An item of a cell leaving the fast area, before any of them leaves:
    // its fall ends on the ground (10B0:0386, 1040:11FD).
    std::function<void(ObjId item)> drop_falling;
    // The engine's message box (GUMPSHAP_1358_0046 then Gump_run_modal):
    // the game freezes until it is closed.
    std::function<void(const std::string& text)> show_message;
    // The designated target (GameState::Aim::target) moved (Item_I_move,
    // 10A8:1531) or is being destroyed (Item_forget, 10B0:1AB3), target
    // designation being on.
    std::function<void(ObjId item, std::int32_t x, std::int32_t y, std::int32_t z)> designated_target_moved;
    std::function<void(ObjId item)> designated_target_forgotten;
    // Controller_fire_reset then the end of the AutoFirer (1138:2F9F).
    std::function<void()> reset_fire;
    // ITEM.C 275F: the item is hit by `other` with `force` (contact events
    // of a move, 10E8:123A; see hit_item).
    std::function<void(ObjId item, ObjId other, std::uint16_t force)> got_hit;
    // Port addition, the autosave's moments (Game::autosave): `autosave` saves
    // the game as it is (a keycard picked up, a door about to open with one),
    // `autosave_soon` once the player may act again (a mission start).
    std::function<void()> autosave;
    std::function<void()> autosave_soon;
};

class World {
public:
    GameState state;

    explicit World(std::vector<ShapeInfo> typeflags = {}) : typeflags_(std::move(typeflags)) { init_free_slots(); }


    void set_hooks(WorldHooks hooks) { hooks_ = std::move(hooks); }
    const WorldHooks& hooks() const { return hooks_; }
    // The sound system (DSFXMAN.C), null when there is none (DS:462A).
    void set_audio(Audio* audio) { audio_ = audio; }
    Audio* audio() const { return audio_; }
    // Frame sizes, from SHAPES.FLX (nullopt when the frame does not exist).
    using FrameLookup = std::function<std::optional<FrameBox>(std::uint16_t shape, std::uint16_t frame)>;
    void set_frame_lookup(FrameLookup lookup) { frame_lookup_ = std::move(lookup); }
    // GLOB.FLX, needed to expand glob eggs entering the fast area.
    void set_globs(const FlexArchive* globs) { globs_ = globs; }

    Item* get(ObjId id);
    const Item* get(ObjId id) const;
    // Allocates the id at the head of the free item slots (ITEMCACH_10b0_262a;
    // 1..255 are NPC slots) and stores the item.
    Item& add(Item item);
    // Stores the item under item.id (saved games keep their slot numbers),
    // taking the slot out of its free list.
    Item& add_with_id(Item item);
    void destroy(ObjId id);  // also destroys contents; the slot becomes free
    std::size_t item_count() const { return items_.size(); }

    const ShapeInfo* shape_info(std::uint16_t shape) const {
        return shape < typeflags_.size() ? &typeflags_[shape] : nullptr;
    }
    // The footpad height of a shape (TYPEFLAG bytes 3-4, bits 7-11), which
    // the player's kneeling changes (10F0:221B / 226D).
    // @orig 10F0:221B 10F0:226D
    void set_footpad_z(std::uint16_t shape, std::uint8_t z) {
        if (shape >= typeflags_.size()) return;
        ShapeInfo& info = typeflags_[shape];
        info.footpad_z = z;
        info.bytes[3] = static_cast<std::uint8_t>((info.bytes[3] & 0x7F) | (z & 1) << 7);
        info.bytes[4] = static_cast<std::uint8_t>((info.bytes[4] & 0xF0) | (z >> 1));
    }

    // Creates the items of one map from FIXED.DAT; returns the top-level ids.
    // For viewers; the game changes maps with load_map / unload_map.
    std::vector<ObjId> load_fixed_map(const FlexArchive& fixed, std::size_t map);

    // Map changes (ITEMCACH.C, map_change.cpp). The item table holds the
    // current map only (DS:10FE; -1 before the first one); NPCs stay in it
    // across maps.
    void set_fixed(const FlexArchive* fixed) { fixed_ = fixed; }
    // STATIC/TRIG.DAT; computed values until it is set.
    void set_trig(const TrigTables* trig) { trig_ = trig; }
    const TrigTables& trig() const;
    int current_map() const { return current_map_; }
    // ITEMCACH_10b0_04c9: every item of the map grid leaves the fast area;
    // NPCs are unlinked and kept (off_map), the others destroyed with their
    // contents. Items in containers off the map and pending items stay.
    void unload_map();
    // ITEMCACH_10b0_05d6 for a map > 0: FIXED.DAT entry `map`, then the NPCs
    // whose map number is `map` are put back in their cells. Items whose
    // shape has TYPEFLAG byte 0 bit 7, and those NPCs, get usecode event 4
    // ("cachein"). Map 0 (reloading the NPCs) is not reproduced.
    void load_map(int map);
    // CameraProcess::I_moveTo with a map (ITEMCACH_10b0_13be): 0 = the
    // current map. Another map is loaded in place of the current one, and
    // the fast area is then entered as a whole.
    void move_camera_to(std::int32_t x, std::int32_t y, std::int32_t z, int map);
    // Actor::I_teleport (10F0:29FA): moves an NPC, maybe to another map.
    // The player carries the camera along when it follows him, and so
    // changes the map.
    void teleport(ObjId npc, std::int32_t x, std::int32_t y, std::int32_t z, int map);

    // Expands every glob egg of the map at once, for viewers (the game does it
    // per cell entering the fast area, see update_fast_area). Eggs whose items
    // are already there (saved games contain those of the fast area) are
    // skipped. Returns the items created.
    std::size_t expand_glob_eggs(const FlexArchive& globs);

    // Expands one glob egg (family 3, glob number = quality; 0 = none) where it
    // stands. Items are flagged disposable (0x82). Returns the items created.
    std::size_t expand_glob_egg(const FlexArchive& globs, ObjId egg);

    // Replaces the world with the item table of a saved game (entry 0 of a
    // SAVEGAME Flex, see docs/savegame.md) and the saved fast area (entry 1).
    // Returns the number of items.
    std::size_t load_saved_items(const FlexArchive& save);
    // The same entries as ITEMCACH_10b0_0b67 writes them: the item table
    // (twelve arrays; free slots keep what they held last) and its lists
    // (map number, fast area, cell heads and counts, free lists, pending
    // stack). Throws if an item has an id past the table.
    std::vector<std::uint8_t> saved_item_table() const;
    std::vector<std::uint8_t> saved_item_lists() const;
    // Entry 10 (NPC_10f0_00bc): the NPC table as is.
    std::span<const std::uint8_t> npc_table() const { return npcs_; }
    // Entries 50, 51, 52 (TRACKER_1128_05b7): slot count, items tracked, slots.
    std::vector<std::uint8_t> saved_tracker(int part) const;

    // Free slots of the item table (ITEMCACH.C), head first: items (head
    // DS:7DC8, tail 7DCC, count 7DD0) and NPCs (7DCA, 7DCE, 7DD2). A freed
    // slot goes to the head; a new item takes the head.
    struct FreeSlots {
        std::deque<ObjId> ids;
        ObjId tail = 0;
    };
    const FreeSlots& free_items() const { return free_items_; }
    const FreeSlots& free_npcs() const { return free_npcs_; }

    // Per-shape hit points given to created items (STATIC/DAMAGE.FLX entry 0:
    // 6 bytes per shape, byte 0 bit 0 = present, byte 5 = hit points).
    void set_damage_table(std::vector<std::uint8_t> table) { damage_ = std::move(table); }
    std::uint8_t initial_hit_points(std::uint16_t shape) const;
    // The 6-byte DAMAGE.FLX record of a shape (empty when absent: bit 0 clear).
    std::span<const std::uint8_t> damage_record(std::uint16_t shape) const {
        const std::size_t at = std::size_t{shape} * 6;
        if (at + 6 > damage_.size() || !(damage_[at] & 1)) return {};
        return std::span(damage_).subspan(at, 6);
    }
    // Items whose x, y lie in [x0, x1] x [y0, y1], cell by cell (x inner, y
    // outer), each cell in chain order (ITEMFIND_10c8_18c0).
    std::vector<ObjId> rect_search(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) const;

    // Position of the outermost container (the item's own when on the map).
    struct Point {
        std::int32_t x, y, z;
    };
    Point location(const Item& item) const;

    // NPC_10f0_236b: a new NPC of `shape` / `frame` on the pending stack, in
    // the free NPC slot at the head of its list (ITEMCACH_10b0_268d); with
    // none, a disposable NPC (status 2) out of the fast area and of special
    // shapes is destroyed and its slot reused. Returns 0 when there is no slot.
    ObjId create_npc(std::uint16_t shape, std::uint16_t frame);

    // Item::I_create: a new item of `shape`/`frame` on the pending stack.
    // Returns 0 when the shape is out of range (> 0x7FF).
    ObjId create(std::uint16_t shape, std::uint8_t frame);

    // Pending-item stack (ITEMCACH.C; ScummVM's "ethereal void"). Items are
    // created there, or pushed out of the world, then popped to a place.
    void push(ObjId id);
    ObjId pending_top() const { return pending_.empty() ? 0 : pending_.back(); }
    std::size_t pending_count() const { return pending_.size(); }
    // Each pop takes the top item; they return its id (0 if the stack is empty).
    ObjId pop_to_coords(std::int32_t x, std::int32_t y, std::int32_t z);
    ObjId pop_to_container(ObjId container);  // first in the contents
    ObjId pop_to_end(ObjId container);        // last in the contents
    ObjId pop();                              // back where it was
    // Moves an item to the map: out of its cell or container and at the head
    // of the new cell, entering or leaving the fast area (push +
    // popToCoords).
    void move_item(ObjId id, std::int32_t x, std::int32_t y, std::int32_t z);

    // Map grid (ITEMCACH.C): 64 x 64 cells of 512 units, each a chain of the
    // items standing in it. Order matters to scripts (searches follow it):
    // popping an item to the map puts it at the head of its cell.
    static constexpr int kGridSize = 64;
    static constexpr int kCellShift = 9;
    // Items of a cell in chain order (empty outside the grid).
    std::span<const ObjId> cell(int cx, int cy) const;
    // True if every item standing on the map is in its cell exactly once.
    bool cells_consistent() const;

    // Usecode `loop` searches (ITEMFIND.C), in the original's order; `script`
    // is the loop filter (see matches_loop_script).
    // Type 2: items whose x/y lie within `range` of `centre`'s x/y, cell by
    // cell (x inner, y outer), each cell in chain order.
    std::vector<ObjId> area_search(ObjId centre, std::uint16_t range, std::span<const std::uint8_t> script) const;
    // Type 6: items resting on `item` (mode bit 0: their bottom at its top)
    // and/or supporting it (bit 1: their top at its bottom), with footprints
    // overlapping in x and y. `sides`, when given, receives for each item
    // found 1 if it rests on `item`, 2 if it supports it (search +0x40).
    std::vector<ObjId> surface_search(ObjId item, int mode, std::span<const std::uint8_t> script,
                                      std::vector<std::uint8_t>* sides = nullptr) const;
    // Type 4: contents of `container` in order.
    std::vector<ObjId> container_search(ObjId container, std::span<const std::uint8_t> script) const;
    // Loop filter, a byte-coded RPN expression (ITEMFIND.C 010C), see docs/usecode.md.
    bool matches_loop_script(const Item& item, std::span<const std::uint8_t> script) const;

    // Fast area. update_fast_area recentres it on (x, y) (ITEMCACH.C, called
    // by CameraProcess::I_moveTo): items of the cells left behind leave it and
    // disposable ones are destroyed; in the cells reached, glob eggs are
    // expanded and items enter it.
    const FastArea& fast_area() const { return fast_area_; }
    void set_fast_area(const FastArea& area) { fast_area_ = area; }
    void update_fast_area(std::int32_t x, std::int32_t y);
    // Empties the fast area as if arriving on the map: glob items (0x82) are
    // destroyed, other items lose the fast-area flag. The next set_camera
    // rebuilds it, expanding the globs again and starting the items' events.
    void clear_fast_area();
    // Item::I_enterFastArea / leaveFastArea (ITEM.C), reduced to what the
    // port has: the flag, process cleanup and the usecode events 0x0F / 0x10.
    // enter_fast_area returns the pid of event 0x0F, 0 when none started.
    std::uint16_t enter_fast_area(Item& item);
    void leave_fast_area(Item& item);

    // Items that shots can aim at (TRACKER.C, 100 slots at DS:26E0, count at
    // DS:26E6): shapes with TYPEFLAG byte 0 bit 4 or byte 6 bit 4, in the
    // fast area and not broken (status 0x8000). An item takes the first free
    // slot when it enters the fast area, is repaired or gets such a shape; it
    // leaves its slot when it leaves the fast area, breaks, is destroyed or
    // loses the shape. The slot order is the search order of find_target.
    static constexpr std::size_t kTrackedSlots = 100;
    const std::array<ObjId, kTrackedSlots>& tracked() const { return tracked_; }
    std::size_t tracked_count() const { return tracked_count_; }
    bool is_trackable(std::uint16_t shape) const;
    // Item::I_setShape (keeps the tracked list up to date).
    void set_shape(Item& item, std::uint16_t shape);
    // Item::I_setBroken: status 0x8000, out of the tracked list.
    void set_broken(Item& item);
    // ITEM_10a8_260f: status 0x8000 cleared, tracked again.
    void clear_broken(Item& item);

    // CameraProcess::I_moveTo: camera position, view corner and fast area,
    // then the looping sounds' volumes (1450:0809). The view corner keeps the
    // original constants (camera 320 pixels from the left, 300 from the top
    // of a 640 x 480 screen).
    void set_camera(std::int32_t x, std::int32_t y, std::int32_t z);
    // Port addition (native resolution): the view is w x h instead of
    // 640 x 480. The camera keeps the same place relative to the centre
    // (w / 2 from the left, 300 + (h - 480) / 2 from the top), so the extra
    // pixels show more of the surroundings. The fast area grows with it and
    // keeps the original margin around the view: one more cell of d per 256
    // extra pixels on each side, one more of s per 128 above and below, so
    // the whole view is filled and items still wake up out of sight. No
    // change when the size is the same.
    void set_view_size(int w, int h);
    int view_origin_x() const;
    int view_origin_y() const;
    // ITEMCACH.C 1D29, when the controlled NPC is put down at (x, y, z): if
    // it gets within 100 pixels of the left / right edge, 140 of the top or
    // 70 of the bottom, the view jumps to put it at x 520 (coming from the
    // left), 120 (from the right) or the centre, and y 280.
    void track_camera_target(std::int32_t x, std::int32_t y, std::int32_t z);
    // Port addition ("camera suit le joueur" in the enhancements window): the
    // controlled NPC is kept at the centre of the view at every step instead
    // of only when it nears an edge.
    void set_follow_player(bool on) {
        follow_player_ = on;
        if (!on) follow_active_ = false;
    }
    // Once a frame: the view follows the NPC's position averaged over about a
    // stride, so the uneven steps of the walk cycle scroll at an even speed.
    void update_follow_camera(std::uint32_t elapsed_us = 16667);

    // A rectangle of the view, in screen pixels, bounds included.
    struct ScreenRect {
        int x0, y0, x1, y1;
    };
    // The whole view drawn: the window in native resolution.
    ScreenRect view_rect() const { return {0, 0, state.screen_right, state.screen_bottom}; }
    // Where combat happens: the 640 x 480 screen of the original, placed in a
    // larger view like the original screen around the camera. An NPC only
    // spots, keeps and shoots the player in it, and the player's shots only
    // pick their target in it. The whole view when the combat zone is not
    // kept (set_original_combat_zone) or the view is not larger.
    ScreenRect combat_rect() const;
    void set_original_combat_zone(bool on) { original_combat_zone_ = on; }
    bool original_combat_zone() const { return original_combat_zone_; }

    // Item::I_isPartlyOnScreen: the item's frame overlaps the screen. Uses
    // the item's own x/y/z, like the original.
    bool is_partly_on_screen(const Item& item) const { return is_partly_in(item, view_rect()); }
    // ITEM_10a8_41f6: at least half the frame's width and its whole height
    // are on screen.
    bool is_mostly_on_screen(const Item& item) const { return is_mostly_in(item, view_rect()); }
    // The same tests against the combat zone, for what decides a fight.
    bool is_partly_in_combat_zone(const Item& item) const { return is_partly_in(item, combat_rect()); }
    bool is_mostly_in_combat_zone(const Item& item) const { return is_mostly_in(item, combat_rect()); }
    bool is_partly_in(const Item& item, const ScreenRect& r) const;
    bool is_mostly_in(const Item& item, const ScreenRect& r) const;

    // Item::I_setFrame: 16 bits for NPCs (high byte also in the NPC record),
    // 8 bits for other items.
    void set_frame(Item& item, std::uint16_t frame);

    // NPC table. Ids outside 1..255 count as dead (Actor::I_isDead).
    bool is_dead(ObjId id) const;
    std::uint8_t npc_direction(ObjId id) const;  // Actor::I_getDir
    std::span<std::uint8_t> npc_record(ObjId id);  // empty outside 0..255
    std::span<const std::uint8_t> npc_record(ObjId id) const;
    // Actor::I_getHp (+0x00), I_getMana (+0x57), I_getEquip (+0x1B, 30 words).
    std::uint16_t npc_hp(ObjId id) const { return npc_word(id, 0x00); }
    std::uint16_t npc_mana(ObjId id) const { return npc_word(id, 0x57); }
    ObjId npc_equip(ObjId id, int slot) const {
        return slot >= 0 && slot < 30 ? npc_word(id, 0x1B + 2 * slot) : 0;
    }
    std::uint16_t npc_word(ObjId id, std::size_t at) const {
        const auto r = npc_record(id);
        return r.empty() ? 0 : static_cast<std::uint16_t>(r[at] | r[at + 1] << 8);
    }
    // @orig none: NPC record accessor
    void set_npc_word(ObjId id, std::size_t at, std::uint16_t value) {
        const auto r = npc_record(id);
        if (r.empty()) return;
        r[at] = static_cast<std::uint8_t>(value);
        r[at + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    // NPC_10f0_2933: every equipment slot of `npc` holding `item` emptied.
    void unequip(ObjId npc, ObjId item) {
        for (int slot = 0; slot < 30; ++slot) {
            if (npc_equip(npc, slot) == item) set_npc_word(npc, 0x1B + 2 * slot, 0);
        }
    }
    // Port repair of saves made before World::destroy emptied the slots of
    // the items it destroys: every slot naming an item that is gone, or that
    // its NPC no longer holds, is emptied. Returns how many were.
    int forget_lost_equipment();
    // The NPC's active weapon: equipment slot at record +0x15, 0 = none
    // (NPC_10f0_33b8).
    ObjId active_weapon(ObjId npc) const {
        const int slot = npc_word(npc, 0x15);
        return slot == 0 ? 0 : npc_equip(npc, slot);
    }
    // The NPC's active inventory item: equipment slot at record +0x17, 0 = none
    // (MainActor::getActiveInvItem, 10F0:33EC).
    ObjId active_inventory_item(ObjId npc) const {
        const int slot = npc_word(npc, 0x17);
        return slot == 0 ? 0 : npc_equip(npc, slot);
    }
    // Shape family (TYPEFLAG.DAT), Item::I_getFamily.
    std::uint8_t family_of(std::uint16_t shape) const {
        const ShapeInfo* info = shape_info(shape);
        return info ? info->family : 0;
    }

    // Iteration in id order.
    template <class Fn>
    void for_each(Fn&& fn) const {
        for (ObjId id = 1; id < next_id_; ++id) {
            if (auto it = items_.find(id); it != items_.end()) fn(it->second);
        }
    }

    // UPROCESS_1420_0dc5: the usecode class that gets an item's events. The
    // classes are four ranges (sizes at DS:65F0, starts at DS:8C64..8C6A):
    // 0x800 shapes, then 0x100 NPC numbers (an NPC not made by an egg; only
    // 0x801, AVATAR, exists), 0x100 types of family 4 eggs (Item::
    // I_getUnkEggType, the quality's low byte: TRIGEGG, CAMERA, MUSIC, the
    // mission eggs MISS1EGG..), and 0x200 free classes that only scripts call.
    std::uint16_t usecode_class(const Item& item) const {
        if (is_npc_id(item.id) && !(item.flags & kItemFromEgg)) return static_cast<std::uint16_t>(kClassNpcs + item.id);
        if (family(item) == kFamilyUnkEgg) return static_cast<std::uint16_t>(kClassEggs + (item.quality & 0xFF));
        return item.shape;
    }
    static constexpr std::uint16_t kClassNpcs = 0x800;
    static constexpr std::uint16_t kClassEggs = 0x900;
    static constexpr std::uint8_t kFamilyUnkEgg = 4;

private:
    std::uint8_t family(const Item& item) const {
        const ShapeInfo* info = shape_info(item.shape);
        return info ? info->family : 0;
    }
    void detach(Item& item);  // out of its container
    static bool on_map(const Item& item) {
        return item.parent == 0 && !(item.flags & (kItemContained | kItemEthereal)) && !item.off_map;
    }
    std::vector<ObjId>* cell_of(const Item& item);
    void link_to_cell(const Item& item);      // at the head, if on the map
    void unlink_from_cell(const Item& item);
    bool has_fast_area_events(const Item& item) const;
    ObjId pop_into(ObjId container, bool at_end);
    void track(ObjId id);    // TRACKER_1128_0098
    void untrack(ObjId id);  // TRACKER_1128_00fe
    void clear_tracked();    // TRACKER_1128_0066 + 0000
    void init_free_slots();  // ITEMCACH_10b0_161b
    void push_free(ObjId id);    // ITEMCACH_10b0_1a23
    void remove_free(ObjId id);  // ITEMCACH_10b0_1986, nothing if the slot is not free

    std::vector<ShapeInfo> typeflags_;
    std::vector<std::uint8_t> damage_;
    std::vector<ObjId> pending_;  // top = back
    std::vector<std::vector<ObjId>> cells_ = std::vector<std::vector<ObjId>>(kGridSize * kGridSize);
    WorldHooks hooks_;
    Audio* audio_ = nullptr;
    FrameLookup frame_lookup_;
    std::vector<std::uint8_t> npcs_ = std::vector<std::uint8_t>(kNpcSlots * kNpcRecordSize);
    const FlexArchive* globs_ = nullptr;
    const FlexArchive* fixed_ = nullptr;
    const TrigTables* trig_ = nullptr;
    int current_map_ = -1;
    FastArea fast_area_;
    std::int32_t fast_half_s_ = FastArea::kHalfS, fast_half_d_ = FastArea::kHalfD;  // grown by set_view_size
    bool original_combat_zone_ = true;
    void shift_camera(int dx, int dy);  // the view moves by (dx, dy) pixels
    void set_camera_view(std::int32_t x, std::int32_t y, std::int32_t z, int left, int top);
    bool follow_player_ = false;
    bool follow_active_ = false;  // follow_goal_ is set
    std::int32_t follow_goal_[3] = {0, 0, 0};
    struct FollowSample {
        std::int64_t at_us;
        int left, top;  // view that centres the NPC, from at_us on
    };
    std::deque<FollowSample> follow_history_;
    std::int64_t follow_clock_us_ = 0;
    double follow_view_[2] = {0, 0};
    bool expanding_glob_ = false;  // no fast area entry while set (glob far from the drawn area)
    // GLOB.C 039C: the drawn area's corner (DS:2CF1, 2CF3) as the last move
    // left it, 128-pixel aligned; reset to the new view after clear_fast_area.
    int drawn_left_ = 0, drawn_top_ = 0;
    bool drawn_reset_ = true;
    bool glob_near_view(std::int32_t base_x, std::int32_t base_y) const;
    std::array<ObjId, kTrackedSlots> tracked_{};
    std::size_t tracked_count_ = 0;
    std::unordered_map<ObjId, Item> items_;
    ObjId next_id_ = 1;
    FreeSlots free_items_, free_npcs_;
    std::bitset<kItemSlots> free_mask_;
    // What the free slots held last (the original leaves it in the table).
    std::unordered_map<ObjId, Item> stale_;
    // Saved cell counts minus the chain lengths (the original's counts drift).
    std::array<std::int16_t, kGridSize * kGridSize> cell_count_bias_{};
    std::uint16_t slot0_array11_ = 0;  // array 11 of slot 0, kept raw

};

}  // namespace crusader
