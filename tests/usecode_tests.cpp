// Usecode interpreter semantics on hand-assembled bytecode (no game data).

#include "engine/kernel.hpp"
#include "engine/usecode/intrinsic_numbering.hpp"
#include "engine/usecode/uc_machine.hpp"
#include "engine/usecode/uc_process.hpp"

#include <cstdio>
#include <map>
#include <set>
#include <string>
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

// Tiny assembler: code offsets start at 1 (after the class pad byte).
struct Asm {
    std::vector<std::uint8_t> code;
    std::map<std::string, std::uint32_t> labels;
    std::vector<std::pair<std::size_t, std::string>> fixups;  // position of s16, label

    std::uint32_t here() const { return static_cast<std::uint32_t>(code.size()) + 1; }
    Asm& label(const std::string& name) { labels[name] = here(); return *this; }
    Asm& b(std::initializer_list<int> bytes) {
        for (int v : bytes) code.push_back(static_cast<std::uint8_t>(v));
        return *this;
    }
    Asm& w(int v) { return b({v & 0xFF, (v >> 8) & 0xFF}); }
    Asm& jump(int op, const std::string& target) {
        b({op});
        fixups.emplace_back(code.size(), target);
        return w(0);
    }
    std::vector<std::uint8_t> finish() {
        for (auto& [pos, name] : fixups) {
            const int rel = static_cast<int>(labels.at(name)) - static_cast<int>(pos + 2 + 1);
            code[pos] = static_cast<std::uint8_t>(rel);
            code[pos + 1] = static_cast<std::uint8_t>(rel >> 8);
        }
        return code;
    }
};

enum : int {
    POP = 0x01, POP_RES = 0x08, PUSH_BYTE = 0x0A, PUSH = 0x0B, PUSH_DWORD = 0x0C, MK_LIST = 0x0E,
    CALLI = 0x0F, CALL = 0x11, POP_TEMP = 0x12, ADD = 0x14, SUB = 0x1C, MUL = 0x1E, PUSH_BP = 0x3F,
    JZ = 0x51, JMP = 0x52, SUSPEND = 0x53, IMPLIES = 0x54, SPAWN = 0x57, PUSH_PID = 0x59, INIT = 0x5A,
    PUSH_RETVAL = 0x5E, W2D = 0x60, PUSH_PROC_RESULT = 0x6D, ADD_SP = 0x6E, FOREACH = 0x75, RET = 0x50,
};

// Single class 0 whose events are the given functions, laid out back to back.
UsecodeFile make_file(std::vector<std::vector<std::uint8_t>> functions) {
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint8_t> code;
    for (auto& f : functions) {
        offsets.push_back(static_cast<std::uint32_t>(code.size()) + 1);
        code.insert(code.end(), f.begin(), f.end());
    }
    return UsecodeFile::from_classes({UsecodeFile::make_class(offsets, code)});
}

// Waits for a process and records the result it terminated with (terminated
// processes are reaped at once, so the result is read through a waiter).
class Watcher : public Process {
public:
    explicit Watcher(std::uint32_t& out) : out_(out) {}

protected:
    void run() override {
        out_ = wake_result();
        terminate();
    }

private:
    std::uint32_t& out_;
};

std::uint32_t run_to_end(Kernel& k, UCMachine& m, Pid pid, int max_slices = 50) {
    (void)m;
    std::uint32_t result = 0xDEADBEEF;
    auto watcher = std::make_unique<Watcher>(result);
    Watcher& w = *watcher;
    const Pid wpid = k.add(std::move(watcher));
    w.wait_for(pid);
    for (int i = 0; i < max_slices && k.get(wpid); ++i) {
        k.advance_time(1'000'000 / 60);
        k.run_slice();
    }
    return result;
}

Pid start(UCMachine& m, std::uint32_t event) {
    return m.kernel().add(std::make_unique<UCProcess>(
        m, std::uint16_t{0}, static_cast<std::uint16_t>(m.usecode().event_offset(0, event)), std::uint32_t{0},
        std::size_t{0}, std::span<const std::uint8_t>{}));
}

void test_arithmetic() {
    Asm a;
    a.b({PUSH_BYTE, 5}).b({PUSH_BYTE, 7}).b({ADD}).b({PUSH_BYTE, 3}).b({MUL}).b({W2D}).b({POP_RES}).b({RET});
    const auto file = make_file({a.finish()});
    Kernel k;
    UCMachine m(k, file);
    CHECK(run_to_end(k, m, start(m, 0)) == 36);
    CHECK(m.faults().empty());
}

void test_loop_and_locals() {
    // sum = 0; i = 10; while (i) { sum += i; i -= 1; } return sum;
    Asm a;
    a.b({INIT, 4}).b({PUSH_BYTE, 0}).b({POP, 0xFC}).b({PUSH_BYTE, 10}).b({POP, 0xFE});
    a.label("loop").b({PUSH_BP, 0xFE}).jump(JZ, "end");
    a.b({PUSH_BP, 0xFC}).b({PUSH_BP, 0xFE}).b({ADD}).b({POP, 0xFC});
    a.b({PUSH_BP, 0xFE}).b({PUSH_BYTE, 1}).b({SUB}).b({POP, 0xFE}).jump(JMP, "loop");
    a.label("end").b({PUSH_BP, 0xFC}).b({W2D}).b({POP_RES}).b({RET});
    const auto file = make_file({a.finish()});
    Kernel k;
    UCMachine m(k, file);
    CHECK(run_to_end(k, m, start(m, 0)) == 55);
    CHECK(m.faults().empty());
}

void test_call_and_return_value() {
    Asm caller;
    caller.b({CALL}).w(0).w(1).b({PUSH_RETVAL}).b({W2D}).b({POP_RES}).b({RET});
    Asm callee;
    callee.b({PUSH_BYTE, 42}).b({POP_TEMP}).b({RET});
    const auto file = make_file({caller.finish(), callee.finish()});
    Kernel k;
    UCMachine m(k, file);
    CHECK(run_to_end(k, m, start(m, 0)) == 42);
    CHECK(m.faults().empty());
}

void test_list_foreach() {
    // list = [1, 2, 3]; sum over it with foreach
    Asm a;
    a.b({INIT, 4}).b({PUSH_BYTE, 0}).b({POP, 0xFC});
    a.b({PUSH_BYTE, 1}).b({PUSH_BYTE, 2}).b({PUSH_BYTE, 3}).b({MK_LIST, 2, 3});
    a.b({PUSH, 0xFF, 0xFF});
    a.label("loop");
    a.b({FOREACH, 0xFE, 2}).fixups.emplace_back(a.code.size(), "end");
    a.w(0);
    a.b({PUSH_BP, 0xFC}).b({PUSH_BP, 0xFE}).b({ADD}).b({POP, 0xFC}).jump(JMP, "loop");
    a.label("end").b({PUSH_BP, 0xFC}).b({W2D}).b({POP_RES}).b({RET});
    const auto file = make_file({a.finish()});
    Kernel k;
    UCMachine m(k, file);
    CHECK(run_to_end(k, m, start(m, 0)) == 6);
    CHECK(m.faults().empty());
    CHECK(m.heap().list_count() == 0);  // foreach frees its list
}

void test_intrinsic() {
    Asm a;
    a.b({PUSH_BYTE, 21}).b({CALLI, 2}).w(0x10).b({ADD_SP, 0xFE}).b({PUSH_RETVAL}).b({W2D}).b({POP_RES}).b({RET});
    const auto file = make_file({a.finish()});
    Kernel k;
    UCMachine m(k, file);
    m.set_intrinsic(0x10, [](UCMachine&, UCProcess&, std::span<const std::uint8_t> args) {
        return static_cast<std::uint32_t>((args[0] | args[1] << 8) * 2);
    });
    CHECK(run_to_end(k, m, start(m, 0)) == 42);
    CHECK(m.faults().empty());
}

// US usecode: `calli 0x11` reaches the routine bound under the French number
// the numbering gives (here 0x10); a number past the table is kept.
void test_intrinsic_numbering() {
    Asm a;
    a.b({PUSH_BYTE, 21}).b({CALLI, 2}).w(0x11).b({ADD_SP, 0xFE});
    a.b({CALLI, 0}).w(0x30).b({PUSH_RETVAL}).b({W2D}).b({POP_RES}).b({RET});
    const auto file = make_file({a.finish()});
    Kernel k;
    UCMachine m(k, file);
    std::uint32_t got = 0;
    m.set_intrinsic(0x10, [&got](UCMachine&, UCProcess&, std::span<const std::uint8_t> args) {
        got = static_cast<std::uint32_t>(args[0] | args[1] << 8);
        return 0u;
    });
    m.set_intrinsic(0x30, [](UCMachine&, UCProcess&, std::span<const std::uint8_t>) { return 7u; });
    std::uint16_t numbering[0x12] = {};
    numbering[0x11] = 0x10;
    m.set_intrinsic_numbering(numbering);
    CHECK(run_to_end(k, m, start(m, 0)) == 7);
    CHECK(got == 21 && m.unbound_intrinsic_calls().empty() && m.faults().empty());
}

// The 1.01 table: one French number (or 0xFFFF, unbound) per entry of its
// UNKCOFF.DAT (308); the routines named by their address in the sources.
void test_v101_numbering() {
    const auto n = v101_intrinsic_numbering();
    CHECK(n.size() == 308);
    for (const std::uint16_t f : n) CHECK(f < 309 || f == 0xFFFF);
    // setDead (10F0:2010) at its eight numbers, rndRange (12E0:0293).
    const std::set<std::uint16_t> set_dead = {0x021, 0x062, 0x074, 0x0A1, 0x0A9, 0x0D8, 0x0E7, 0x12E};
    for (const std::uint16_t i : {0x021, 0x05D, 0x06F, 0x099, 0x0A1, 0x0D0, 0x0DF, 0x132}) CHECK(set_dead.count(n[i]));
    CHECK(n[0x018] == 0x018);
    CHECK(intrinsic_numbering(DataInfo{"1.01", "US"}).data() == n.data());
    CHECK(intrinsic_numbering(DataInfo{"1.23", "US"}).data() == us_intrinsic_numbering().data());
    CHECK(intrinsic_numbering(DataInfo{"1.08", "FR"}).empty());
}

void test_spawn_implies_result() {
    // parent: child = spawn fn 1; implies(self waits for child); suspend;
    //         return the child's result
    Asm parent;
    parent.b({PUSH_DWORD, 0, 0, 0, 0}).b({SPAWN, 0, 0}).w(0).w(1);
    parent.b({PUSH_PID}).b({PUSH_RETVAL}).b({IMPLIES, 1, 1}).b({POP_TEMP}).b({SUSPEND});
    parent.b({PUSH_PROC_RESULT}).b({POP_RES}).b({RET});
    // child: yields once, then returns 99
    Asm child;
    child.b({SUSPEND}).b({PUSH_BYTE, 99}).b({W2D}).b({POP_RES}).b({RET});
    const auto file = make_file({parent.finish(), child.finish()});
    Kernel k;
    UCMachine m(k, file);
    const Pid pid = start(m, 0);
    CHECK(run_to_end(k, m, pid) == 99);
    CHECK(m.faults().empty());
}

// One result register (INTER_1418_038b): a script that ends without its own
// pop_res returns what the child it waited for left there (WALLGUN_3A77
// after ITEM_0574).
void test_result_register_shared() {
    Asm parent;
    parent.b({PUSH_DWORD, 0, 0, 0, 0}).b({SPAWN, 0, 0}).w(0).w(1);
    parent.b({PUSH_PID}).b({PUSH_RETVAL}).b({IMPLIES, 1, 1}).b({POP_TEMP}).b({SUSPEND});
    parent.b({RET});
    Asm child;
    child.b({SUSPEND}).b({PUSH_BYTE, 7}).b({W2D}).b({POP_RES}).b({RET});
    const auto file = make_file({parent.finish(), child.finish()});
    Kernel k;
    UCMachine m(k, file);
    CHECK(run_to_end(k, m, start(m, 0)) == 7);
    CHECK(m.faults().empty());
}

// `implies` blocks the process on its target, but INTER_1418_038b leaves its
// loop only on suspend / ret (the exit code local_36): the instructions after
// it, down to the suspend the compiler always emits (implies, pop_temp,
// suspend: 2498 times in the game), run in the same slice. They used to run
// only after the target had finished, one more tick late.
void test_implies_runs_on_until_suspend() {
    Asm parent;
    parent.b({PUSH_DWORD, 0, 0, 0, 0}).b({SPAWN, 0, 0}).w(0).w(1);
    parent.b({PUSH_PID}).b({PUSH_RETVAL}).b({IMPLIES, 1, 1}).b({POP_TEMP});
    parent.b({CALLI, 0}).w(0x20).b({SUSPEND}).b({RET});
    Asm child;  // alive for three more ticks
    child.b({SUSPEND}).b({SUSPEND}).b({SUSPEND}).b({RET});
    const auto file = make_file({parent.finish(), child.finish()});
    Kernel k;
    UCMachine m(k, file);
    int reached = 0;
    m.set_intrinsic(0x20, [&](UCMachine&, UCProcess&, std::span<const std::uint8_t>) {
        ++reached;
        return std::uint32_t{0};
    });
    start(m, 0);
    k.advance_time(1'000'000 / 60);
    k.run_slice();
    CHECK(reached == 1);  // before the child is over
    CHECK(m.faults().empty());
}

void test_invalid_opcode_faults() {
    const auto file = make_file({{0x7B}});
    Kernel k;
    UCMachine m(k, file);
    const Pid pid = start(m, 0);
    k.run_slice();
    CHECK(m.faults().size() == 1);
    CHECK(k.get(pid) == nullptr);  // failed and reaped
}

}  // namespace

int main() {
    test_arithmetic();
    test_loop_and_locals();
    test_call_and_return_value();
    test_list_foreach();
    test_intrinsic();
    test_intrinsic_numbering();
    test_v101_numbering();
    test_spawn_implies_result();
    test_result_register_shared();
    test_implies_runs_on_until_suspend();
    test_invalid_opcode_faults();
    std::printf(failures ? "%d failure(s)\n" : "all usecode checks passed\n", failures);
    return failures ? 1 : 0;
}
