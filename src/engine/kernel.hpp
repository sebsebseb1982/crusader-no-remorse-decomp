#pragma once
// Process scheduler (KERNEL.C).
//
// The process table is indexed by pid (pid 0 is never used; freed slots are
// reused, lowest first). A slice walks the table in pid order and runs every
// live process that is not suspended, sleeping or waiting, and whose level is
// the current kernel level. Processes flagged runs_at_any_level and the system
// levels 30/31 always run. Raising the level makes a process modal: everything
// below freezes until no process is left at the raised level, at which point
// the kernel drops back down.
//
// Time: the original SystemTimer process counted 60 Hz ticks; here the kernel
// owns the tick counter and advances it from real elapsed time.

#include "engine/process.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace crusader {

class Kernel {
public:
    static constexpr std::uint32_t kTicksPerSecond = 60;
    static constexpr std::uint8_t kMaxLevel = 31;
    static constexpr std::uint8_t kFirstSystemLevel = 30;

    Kernel() = default;
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // Takes ownership; the process joins the current level. Returns its pid.
    Pid add(std::unique_ptr<Process> process);

    // @orig none: C++ construction helper around add
    template <class T, class... Args>
    T& spawn(Args&&... args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *p;
        add(std::move(p));
        return ref;
    }

    Process* get(Pid pid) const;
    std::size_t process_count() const { return live_; }
    // Process type matching every type in the kernel queries (KERNEL.C compares with 0x0C).
    static constexpr std::uint16_t kAnyType = 0x000C;
    // Usecode Kernel::I_getNumProcesses: live processes of an owner and type.
    std::size_t count_processes(ObjId owner, std::uint16_t type) const;
    // KERNEL_11d8_13a4: the first live process of `owner` and `type`, or nullptr.
    Process* find_process(ObjId owner, std::uint16_t type) const;
    // KERNEL.C kill routine, used by Kernel::I_resetRef: fails the live
    // processes of `owner` (0 = any) and `type` running at `level` exactly,
    // except those that run at any level. Returns how many were failed.
    // Not reproduced: the original skips the first table slots and processes
    // with flag 0x200 at +0x18 (meaning unknown).
    std::size_t fail_processes(ObjId owner, std::uint16_t type, std::uint8_t level);
    // KERNEL_11d8_11fe (new game, map switch): fails every live process
    // except those that run at any level. The original starts at table slot
    // 3; the port's first slots do not hold the same processes.
    void fail_all();
    // DS:324D, set while fail_all runs: an animation failed then skips its
    // cleanup (1158:2DC6).
    bool failing_all() const { return failing_all_; }

    // Calls fn(Process&) for every live process, in table order.
    template <class Fn>
    void for_each_process(Fn&& fn) const {
        for (const auto& p : table_) {
            if (p && !p->terminated()) fn(*p);
        }
    }

    // Advances the clock by real elapsed time and feeds timed processes.
    void advance_time(std::uint32_t elapsed_us);
    std::uint32_t ticks() const { return ticks_; }

    // One scheduling pass over the table.
    void run_slice();
    // Ticks between the start of the previous pass and of this one (DS:3A88,
    // measured by the original's main loop); 0 before the second pass.
    std::uint32_t slice_ticks() const { return slice_ticks_; }

    // Ends the current pass after the running process: for a call that blocks
    // in the original (a cutscene played inside its intrinsic), the usecode
    // process stops at once and resumes at the next pass, which the caller
    // delays until the call is over. Cleared when a pass starts.
    void interrupt_slice() { interrupted_ = true; }
    bool slice_interrupted() const { return interrupted_; }

    std::uint8_t level() const { return level_; }
    // Moves `p` and the kernel one level up (Process_raise_level).
    void raise_level(Process& p);
    // Drops back down past the levels left without processes, as at the end
    // of a slice: for the port's modal placeholders, so that the processes
    // started right after they end join the game's level.
    void drop_empty_levels();

private:
    friend class Process;

    bool runnable(const Process& p) const;
    void on_terminated(Process& p);
    void reap();
    void set_level(Process& p, std::uint8_t level);

    std::vector<std::unique_ptr<Process>> table_{1};  // slot 0 unused
    std::array<std::size_t, kMaxLevel + 1> per_level_{};
    std::size_t live_ = 0;
    std::uint8_t level_ = 1;
    bool interrupted_ = false;
    bool failing_all_ = false;

    std::uint32_t ticks_ = 0;
    std::uint32_t slice_start_ = 0, slice_ticks_ = 0;
    bool sliced_ = false;
    std::uint64_t tick_remainder_ = 0;  // microseconds * kTicksPerSecond not yet turned into a tick
};

}  // namespace crusader
