#pragma once
// Usecode interpreter (INTER.C). Executes UCProcess bytecode; opcode semantics
// are documented in docs/usecode.md.
//
// 32-bit "pointers" handed to usecode are opaque handles:
//   high 16 bits = process pid      -> offset into that process's stack
//   0xFFFE                          -> offset into the globals
//   0xFFFD                          -> string handle
//   0xFFFC                          -> object id (dereferences to the 2-byte id)

#include "engine/kernel.hpp"
#include "engine/usecode/uc_heap.hpp"
#include "engine/usecode/usecode_file.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace crusader {

class UCProcess;

class UCMachine {
public:
    // Engine function called by `calli`. `args` are the argument bytes on the
    // caller's stack (last pushed first); they are not popped.
    using Intrinsic = std::function<std::uint32_t(UCMachine&, UCProcess&, std::span<const std::uint8_t> args)>;
    // World query for the `loop` opcode: (search type 2..6, a, b, loop script) -> item ids.
    using ItemSearch = std::function<std::vector<ObjId>(int type, std::uint16_t a, std::uint16_t b,
                                                        std::span<const std::uint8_t> script)>;

    struct Fault {
        Pid pid;
        std::uint16_t class_id;
        std::uint16_t ip;
        std::string message;
    };

    static constexpr std::uint32_t kSegGlobal = 0xFFFE;
    static constexpr std::uint32_t kSegString = 0xFFFD;
    static constexpr std::uint32_t kSegObject = 0xFFFC;

    UCMachine(Kernel& kernel, const UsecodeFile& usecode);

    Kernel& kernel() { return kernel_; }
    const UsecodeFile& usecode() const { return usecode_; }
    UCHeap& heap() { return heap_; }
    std::vector<std::uint8_t>& globals() { return globals_; }

    void set_intrinsic(std::uint16_t number, Intrinsic fn);
    // Usecode whose intrinsic numbers differ from those of the bindings (US
    // data): numbering[n] is the bound number called by `calli n`. Empty: the
    // same numbers.
    void set_intrinsic_numbering(std::span<const std::uint16_t> numbering) { numbering_ = numbering; }
    void set_item_search(ItemSearch fn) { item_search_ = std::move(fn); }
    void set_item_exists(std::function<bool(ObjId)> fn) { item_exists_ = std::move(fn); }
    // Instructions a process may execute in one slice before being forced to yield.
    void set_instruction_budget(std::uint32_t n) { budget_ = n; }

    // Starts event/function `event` of class `cid` on an item (engine-side
    // trigger: use(), look(), hatch()...). Returns the new pid, 0 if absent.
    Pid start_event(std::uint16_t cid, std::uint32_t event, ObjId item, std::span<const std::uint8_t> args = {});
    // Item::callUsecodeEvent: start_event, then the process runs at once until
    // it first yields; its pid, or 0 when it is already over.
    Pid call_event(std::uint16_t cid, std::uint32_t event, ObjId item, std::span<const std::uint8_t> args = {});

    void execute(UCProcess& p);

    static std::uint32_t stack_ptr(Pid pid, std::uint16_t offset) { return std::uint32_t{pid} << 16 | offset; }
    static std::uint32_t global_ptr(std::uint16_t offset) { return kSegGlobal << 16 | offset; }
    static std::uint32_t string_ptr(std::uint16_t id) { return kSegString << 16 | id; }
    static std::uint32_t object_ptr(ObjId id) { return kSegObject << 16 | id; }

    bool dereference(std::uint32_t ptr, std::span<std::uint8_t> out);
    bool assign(std::uint32_t ptr, std::span<const std::uint8_t> in);

    // Diagnostics.
    const std::vector<Fault>& faults() const { return faults_; }
    const std::map<std::uint16_t, std::uint32_t>& unbound_intrinsic_calls() const { return unbound_calls_; }
    std::uint64_t instructions_executed() const { return executed_; }

private:
    bool step(UCProcess& p, bool& yield);
    void fault(UCProcess& p, std::string message);

    Kernel& kernel_;
    const UsecodeFile& usecode_;
    UCHeap heap_;
    std::vector<std::uint8_t> globals_;
    std::vector<Intrinsic> intrinsics_;
    std::span<const std::uint16_t> numbering_;
    ItemSearch item_search_;
    std::function<bool(ObjId)> item_exists_;
    std::uint32_t budget_ = 100'000;

    std::vector<Fault> faults_;
    std::map<std::uint16_t, std::uint32_t> unbound_calls_;
    std::uint64_t executed_ = 0;
};

}  // namespace crusader
