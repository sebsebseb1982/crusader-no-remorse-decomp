#pragma once
// Cooperative process, the unit of work of the engine (PROCESS.C).
//
// Everything that happens over time is a process: the clock, input daemons,
// sound mixing, fades, usecode scripts, actor animations. Each one has a pid
// (its slot in the kernel table), an owner item, a type and a level. The
// kernel calls run() once per slice while the process is runnable.
//
// Mapping to the original flags (Process +16 / +18):
//   suspended       1        runs_at_any_level  2       terminated  0x4000
//   failed          0x20     sleeping           +18:0x1000
// The wait expression replaces the original MList at +1E (RPN of pids,
// 0x401 done, 0x402 AND, 0x404 OR).

#include <cstdint>
#include <vector>

namespace crusader {

using Pid = std::uint16_t;
using ObjId = std::uint16_t;

class Kernel;

class Process {
public:
    virtual ~Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    Pid pid() const { return pid_; }
    ObjId owner() const { return owner_; }
    void set_owner(ObjId owner) { owner_ = owner; }
    std::uint16_t type() const { return type_; }
    void set_type(std::uint16_t type) { type_ = type; }
    std::uint8_t level() const { return level_; }

    bool terminated() const { return terminated_; }
    bool failed() const { return failed_; }
    std::uint32_t result() const { return result_; }
    // The usecode result register (INTER_1418_038b, *param_2): written by
    // the wake-up with the result of the process waited for, read by "push
    // process result" (0x6D), written by "pop result" (0x08), and returned
    // when the script ends. One register: a script that ends without its
    // own pop_res returns what its last child left there.
    std::uint32_t wake_result() const { return wake_result_; }
    // The process whose end woke this one failed (its flag 0x20, which the
    // original reads on the process object, 1110:2259).
    bool wake_failed() const { return wake_failed_; }
    void set_wake_result(std::uint32_t result) { wake_result_ = result; }

    bool suspended() const { return suspended_; }
    void suspend() { suspended_ = true; }
    void resume() { suspended_ = false; }

    bool runs_at_any_level() const { return any_level_; }
    void set_runs_at_any_level(bool v) { any_level_ = v; }

    bool sleeping() const { return sleeping_; }
    // Sleeps for `ticks` kernel ticks (60 per second).
    void sleep(std::uint32_t ticks);

    bool waiting() const { return !wait_expr_.empty(); }
    // Blocks until `other` terminates; successive calls combine with AND.
    // No effect if `other` does not exist or already terminated. Like
    // Process_wait_for, a partner without an owner takes the other's; the
    // usecode's `implies` (KERNEL_11d8_1327) passes `share_owner` false.
    void wait_for(Pid other, bool share_owner = true);
    // Blocks until any of `others` terminates.
    void wait_for_any(const std::vector<Pid>& others);

    void terminate(std::uint32_t result = 0);
    void fail(std::uint32_t result = 0);

protected:
    Process() = default;

    virtual void run() = 0;
    virtual void on_terminate() {}
    // Called by Kernel::add once pid() is valid.
    virtual void on_added() {}

    Kernel& kernel() const { return *kernel_; }

private:
    friend class Kernel;

    struct WaitToken {
        enum class Kind : std::uint8_t { Pending, Done, And, Or } kind;
        Pid pid;
    };

    void finish(std::uint32_t result, bool failed);
    bool add_wait_operand(Pid other, bool share_owner = true);
    void dependency_done(Pid pid, std::uint32_t result, bool failed = false);
    bool evaluate_wait() const;

    Kernel* kernel_ = nullptr;
    Pid pid_ = 0;
    ObjId owner_ = 0;
    std::uint16_t type_ = 0;
    std::uint8_t level_ = 1;

    bool suspended_ = false;
    bool any_level_ = false;
    bool sleeping_ = false;
    bool terminated_ = false;
    bool failed_ = false;
    std::uint32_t sleep_until_ = 0;
    std::uint32_t result_ = 0;
    std::uint32_t wake_result_ = 0;
    bool wake_failed_ = false;

    std::vector<WaitToken> wait_expr_;  // RPN, evaluated when a dependency ends
    std::vector<Pid> waiters_;          // processes waiting for this one
};

// Process run at a fixed rate instead of every slice (TimedProcess, PROCESS.C
// 1814..1913): the kernel adds each frame's elapsed time to an accumulator and
// calls tick() once when a period has elapsed, keeping the remainder.
class TimedProcess : public Process {
public:
    void set_rate(std::uint32_t hz);
    std::uint32_t rate() const { return rate_hz_; }

    // The original runs every process in the slice, timers included; the
    // port's timers only tick, unless they also have a run (the activities).
    virtual bool runs_in_slice() const { return false; }

protected:
    TimedProcess() = default;
    virtual void tick() = 0;
    void run() override {}

private:
    friend class Kernel;
    void accumulate(std::uint32_t elapsed_us);

    std::uint32_t rate_hz_ = 0;
    std::uint32_t period_us_ = 0;
    std::uint32_t accumulator_us_ = 0;
    bool in_tick_ = false;
};

}  // namespace crusader
