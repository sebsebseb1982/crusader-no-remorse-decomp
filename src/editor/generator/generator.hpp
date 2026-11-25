#pragma once
// Crusader: No Limit, the procedural level generator (docs/GENERATEUR.md):
// a mission graph of rooms, laid out on a grid and built from the pieces of
// a biome, with the eggs and scripted objects of the original maps.
//
// Rooms joined by corridors through automatic doors (a door and its
// DOOREGG, as the original maps make them), the mission start (arrival egg
// 0x1E) in the first room and, in the room farthest from it, a teleporter
// pad whose departure egg leads to the next level (milestone 1); locks on
// the way and their keys (milestone 2); two storeys joined by lifts, the
// exit upstairs (milestone 3).

#include "editor/generator/biome.hpp"
#include "editor/level.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace crusader::editor::gen {

// Deterministic on every platform (the standard distributions are not).
class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed * 2654435761u + 0x9E3779B9u) { next(); }
    std::uint32_t next();
    int below(int n) { return n <= 1 ? 0 : static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
    int range(int lo, int hi) { return lo + below(hi - lo + 1); }  // lo..hi
    bool chance(int percent) { return below(100) < percent; }

private:
    std::uint64_t state_;
};

struct Options {
    std::uint32_t seed = 1;
    int biome = -1;           // index in the biomes; -1: drawn from the seed
    int rooms = 0;            // 0: drawn (6 to 10)
    int locks = -1;           // locks on the way to the exit; -1: drawn (1 to 3)
    int storeys = -1;         // 1 or 2 (z 0 and 96); -1: drawn (2 in three levels out of four)
    int guards = -1;          // NPCs in the rooms: 0 none, -1 or 1 as drawn from the seed
    int map = 0;              // the level's own map number
    int next_map = 0;         // where the exit teleporter leads (0: no exit teleport)
    std::uint8_t next_egg = 0x1E;
    // Key cards are the player's for the whole game (a bit per number, 1 to
    // 31): chained levels number theirs from different first cards.
    int first_card = 1;
};

struct Spot {
    std::int32_t x = 0, y = 0, z = 0;
};

// A room: a rectangle of whole floor tiles (usecode units, x1 / y1 excluded).
struct Room {
    int slot_x = 0, slot_y = 0;
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, z = 0;  // z: the storey's floor (0 or 96)
    int depth = 0;  // links from the start room
    Spot entrance;  // where the player comes in, when the layout gives it (missions); else see entry()
    Spot source;    // missions: the set piece's origin on its original map (x0, y0, storey)
    std::int32_t centre_x() const { return (x0 + x1) / 2; }
    std::int32_t centre_y() const { return (y0 + y1) / 2; }
};

// What bars a passage, at the doorway of the room behind it (farther from
// the start), and the key that opens it, in a room reached before it:
//   card door    a door that a key card reader (CARD_NS / CARD_EW) in the
//                corridor opens, with the card lying in the key room;
//   switch wall  an energy wall (EWALLNS / EWALLEW) in the doorway, cut by a
//                switch (SWITCHNS) on a wall of the key room;
//   panel door   a door opened by a panel (PANELNS) in the key room;
//   keypad door  a door opened by a keypad (KEYPADNS / KEYPADEW) in the
//                corridor, whose code a monitor (MONITNS, on a desk
//                MDESKNS) of the key room shows: both read it from a
//                VALUEBOX of their quality low beside the keypad, which
//                draws it at random (100 to 999) when the map loads
//                (VALUEBOX cachein); the monitor's quality high is the
//                message that gives it (TEXTFILE 10, 18 or 19).
// The readers, switches and panels act through TRIGGER on the barrier's
// shape among the items of their quality low, with the settings the
// original maps use (docs/EDITEUR.md): map number bits 7 (turned off:
// the door or wall becomes a marker), argument 3 for doors, 1 for walls.
enum class LockKind { kCardDoor, kSwitchWall, kPanelDoor, kKeypadDoor };
const char* lock_name(LockKind kind);

struct Lock {
    LockKind kind = LockKind::kCardDoor;
    int link = 0;            // the passage it bars
    int room = 0;            // the room behind it
    int key_room = 0;        // where its key is
    std::uint8_t number = 0; // the card number, or the link (quality low) of switch and barrier
    std::uint16_t barrier_shape = 0, key_shape = 0, reader_shape = 0;
    Spot barrier, key, reader;  // positions (reader: the card reader or the keypad)
    Spot code_box, desk;        // keypad doors: the VALUEBOX, the monitor's desk
    std::uint8_t message = 0;   // keypad doors: the monitor's text (TEXTFILE)
};

// A passage between two rooms: a corridor with a door at each end.
struct Link {
    int a = 0, b = 0;
    std::uint8_t door_a = 0, door_b = 0;  // the automatic doors' links (0: no automatic door at that end)
    int lock = -1;                          // in Result::locks
    int lift = -1;                          // in Result::lifts, when its rooms are on different storeys
};

// A passage between storeys, built as on the original maps (map 5): the
// corridor runs at z 0 from the lower room to a lift platform ELEVPLAT
// (8 x 8 x 1, at z 1, its top 9 above the floor: a step) that bears a
// TRIGPAD; past the platform, the corridor goes on at z 96 to the upper
// room, its underside closed by walls. The pad and a call box (BOX_NS /
// BOX_EW) at each end act through TRIGGER on the platform: shape 0x12F,
// action 2 with bit 2 (equip, argument 0), which sends it from one end of
// its run to the other (z 1 and 88, its top level with the upper floor).
// Railings (YR*X6, 48 high) stand round the shaft at the top, on the sides
// where the platform does not touch the upper floor.
struct Lift {
    int link = 0;
    int lower_room = 0, upper_room = 0;
    std::uint8_t number = 0;     // the link (quality low) of platform, pad and call boxes
    Spot platform;               // far corner, at the bottom (z 1)
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the platform's footprint
    Spot bottom_box, top_box;    // the call boxes (far corners)
    std::uint16_t box_shape = 0;
    Spot bottom, top;            // where the player stands in front of the platform, below and above
};

// An NPC of the level: a MONSTER pair of the original maps (the place, frame
// 0, and the kind, frame 1, linked by their quality low): the NPC appears at
// the place when it comes near the player (map number bit 3), from the
// lowest difficulty given, guards there, attacks on seeing the player and
// when shot at (activities 8, 5, 5).
struct Guard {
    int room = 0;
    Spot place;
    int npc_type = 0;         // DTABLE.FLX record
    int min_difficulty = 1;   // 1 to 4
    int direction = 0;        // 0 to 7 (north, north-east...)
    std::uint8_t link = 0;
};

struct Result {
    Level level;
    int biome = 0;
    std::vector<Room> rooms;
    std::vector<Link> links;
    std::vector<Lock> locks;  // in the order they are met on the way to the exit
    std::vector<Lift> lifts;
    std::vector<Guard> guards;
    // Missions: guards that a room's camera calls when it sees the player
    // (their link is the camera's); they are not there before.
    std::vector<Guard> reinforcements;
    int storeys = 1;
    int start_room = 0, exit_room = 0;
    Spot start, exit;  // the arrival egg; the exit pad's centre
    std::vector<std::string> notes;
};

Result generate(const std::vector<Biome>& biomes, const Options& options);

// Where the player stands on coming into a room from the start's side: 80
// units inside its doorway from a neighbour nearer the start (the room's
// centre for the start room). A far corner, as the player's position.
Spot entry(const Result& result, int room);

// Logical check, the solver: from the start, a lock opens once its key room
// is reached (the card picked up, the switch or panel used, the code read;
// a card reader or keypad also needs the room before the lock); the exit room
// and then every room must be reached. Empty when they are, else what is
// wrong. `order`, when given, receives the locks in the order they open.
std::string check_solvable(const Result& result, std::vector<int>* order = nullptr);

}  // namespace crusader::editor::gen
