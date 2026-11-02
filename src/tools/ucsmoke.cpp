// Usecode smoke test on the original game.
//
// Default mode: runs every event of every class on a fake item, all
// intrinsics stubbed (return 0), empty item searches. A fault means the
// interpreter mis-decodes or mis-handles the stack; the stubs make scripts take
// arbitrary branches, which gives wide code coverage.
//
// --map N: loads map N from FIXED.DAT, binds the implemented item intrinsics,
// and runs every event of the usecode class of each item shape present (one
// representative item per shape; an item's class is its shape number). Lists
// the unbound intrinsics most called, to prioritise the next ones.
//
// Usage: ucsmoke [data_dir] [slices_per_event] [--map N]

#include "core/data_info.hpp"
#include "core/flex.hpp"
#include "core/typeflags.hpp"
#include "engine/kernel.hpp"
#include "core/anim_dat.hpp"
#include "engine/usecode/audio_intrinsics.hpp"
#include "engine/usecode/intrinsic_numbering.hpp"
#include "engine/usecode/item_intrinsics.hpp"
#include "engine/usecode/npc_intrinsics.hpp"
#include "engine/usecode/uc_machine.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace crusader;

namespace {

struct Stats {
    std::size_t events = 0, finished = 0, still_running = 0;
    std::uint64_t instructions = 0;
    std::map<std::string, std::size_t> fault_counts;
    std::map<std::string, std::string> fault_example;
    std::map<std::uint16_t, std::uint32_t> unbound;
};

// Events take up to 12 bytes of arguments (TRIGGER event 0x23): pass zeros;
// extra bytes sit unused below the declared ones.
constexpr std::uint8_t kArgs[16] = {};

// The intrinsic numbers of the US usecode, translated to the French ones bound.
std::span<const std::uint16_t> g_numbering;

void run_event(const UsecodeFile& usecode, std::uint16_t cid, std::uint32_t ev, int slices, Stats& st,
               World* world, ObjId item, const AnimDat* anims = nullptr, const dtable::NpcTypes* types = nullptr) {
    Kernel kernel;
    UCMachine machine(kernel, usecode);
    machine.set_intrinsic_numbering(g_numbering);
    machine.set_instruction_budget(20'000);
    if (world) {
        register_item_intrinsics(machine, *world);
        register_world_intrinsics(machine, *world);
        register_audio_intrinsics(machine, *world);
        if (anims) register_npc_intrinsics(machine, *world, *anims, types);
        // Camera on the item, as when the player stands next to it: the item
        // is on screen and the items around it enter the fast area.
        if (const Item* it = world->get(item)) world->set_camera(it->x, it->y, it->z);
    }
    const Pid pid = machine.start_event(cid, ev, item, kArgs);
    ++st.events;
    for (int s = 0; s < slices && kernel.process_count() > 0; ++s) {
        kernel.advance_time(1'000'000 / 60);
        kernel.run_slice();
    }
    if (kernel.get(pid) == nullptr) ++st.finished;
    else ++st.still_running;
    st.instructions += machine.instructions_executed();
    for (const auto& f : machine.faults()) {
        ++st.fault_counts[f.message];
        if (!st.fault_example.count(f.message)) {
            char where[96];
            std::snprintf(where, sizeof where, "%s (0x%04X) ip 0x%04X, event 0x%02X",
                          usecode.class_name(f.class_id).c_str(), f.class_id, f.ip, ev);
            st.fault_example[f.message] = where;
        }
    }
    for (const auto& [n, c] : machine.unbound_intrinsic_calls()) st.unbound[n] += c;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path data = "data";
    int slices = 30;
    std::optional<std::size_t> map;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) map = std::stoul(argv[++i]);
        else positional.emplace_back(argv[i]);
    }
    if (positional.size() > 0) data = positional[0];
    if (positional.size() > 1) slices = std::stoi(positional[1]);

    try {
        g_numbering = intrinsic_numbering(detect_data_info(data));
        const auto flex = FlexArchive::open(data / "USECODE/EUSECODE.FLX");
        const auto usecode = UsecodeFile::from_flex(flex);
        Stats st;

        if (!map) {
            for (std::uint16_t cid = 0; cid < usecode.class_count(); ++cid) {
                for (std::uint32_t ev = 0; ev < usecode.event_count(cid); ++ev) {
                    if (usecode.event_offset(cid, ev)) run_event(usecode, cid, ev, slices, st, nullptr, 1);
                }
            }
        } else {
            const auto fixed = FlexArchive::open(data / "STATIC/FIXED.DAT");
            const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
            const auto anims = AnimDat::load(data / "STATIC/ANIM.DAT");
            const auto npc_types = dtable::NpcTypes::load(data / "STATIC/DTABLE.FLX");
            const auto shapes_flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
            ShapeCache shape_cache(shapes_flex, ShapeLayout::World);
            const World::FrameLookup frames = [&](std::uint16_t shape, std::uint16_t frame) -> std::optional<FrameBox> {
                const Shape* sh = shape_cache.get(shape);
                if (!sh || frame >= sh->frames().size()) return std::nullopt;
                const ShapeFrame& f = sh->frames()[frame];
                return FrameBox{f.width, f.height, f.xoff, f.yoff};
            };
            const auto damage_flex = FlexArchive::open(data / "STATIC/DAMAGE.FLX");
            const auto damage_entry = damage_flex.entry(0);
            const std::vector<std::uint8_t> damage(damage_entry.begin(), damage_entry.end());
            std::vector<std::uint16_t> shapes;
            {
                World probe(typeflags);
                probe.load_fixed_map(fixed, *map);
                probe.for_each([&](const Item& i) {
                    if (std::find(shapes.begin(), shapes.end(), i.shape) == shapes.end()) shapes.push_back(i.shape);
                });
            }
            std::printf("map %zu: %zu distinct shapes\n", *map, shapes.size());
            for (std::uint16_t shape : shapes) {
                if (!usecode.has_class(shape)) continue;
                for (std::uint32_t ev = 0; ev < usecode.event_count(shape); ++ev) {
                    if (!usecode.event_offset(shape, ev)) continue;
                    // Fresh world per event: scripts may destroy or alter items.
                    World world(typeflags);
                    world.set_damage_table(damage);
                    world.set_frame_lookup(frames);
                    world.load_fixed_map(fixed, *map);
                    ObjId id = 0;
                    world.for_each([&](const Item& i) {
                        if (!id && i.shape == shape) id = i.id;
                    });
                    run_event(usecode, shape, ev, slices, st, &world, id, &anims, &npc_types);
                }
            }
        }

        std::printf("events run: %zu  finished: %zu  still running after %d slices: %zu\n", st.events, st.finished,
                    slices, st.still_running);
        std::printf("instructions executed: %llu\n", static_cast<unsigned long long>(st.instructions));
        std::size_t total_faults = 0;
        for (const auto& [msg, n] : st.fault_counts) total_faults += n;
        std::printf("faults: %zu\n", total_faults);
        for (const auto& [msg, n] : st.fault_counts) {
            std::printf("  %6zu  %-40s e.g. %s\n", n, msg.c_str(), st.fault_example[msg].c_str());
        }
        std::vector<std::pair<std::uint32_t, std::uint16_t>> top;
        for (const auto& [n, c] : st.unbound) top.emplace_back(c, n);
        std::sort(top.rbegin(), top.rend());
        std::printf("unbound intrinsics: %zu distinct; most called:\n", top.size());
        for (std::size_t i = 0; i < top.size() && i < 25; ++i) {
            std::printf("  0x%03X  %u calls\n", top[i].second, top[i].first);
        }
        return total_faults == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
