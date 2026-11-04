#include "engine/kernel.hpp"

#include <algorithm>
#include <cassert>

namespace crusader {

// @orig 11D8:0AFF
Pid Kernel::add(std::unique_ptr<Process> process) {
    assert(process && !process->kernel_);
    std::size_t slot = 1;
    while (slot < table_.size() && table_[slot]) {
        ++slot;
    }
    assert(slot <= 0xFFFF && "process table full");
    if (slot == table_.size()) {
        table_.emplace_back();
    }
    Process& p = *process;
    p.kernel_ = this;
    p.pid_ = static_cast<Pid>(slot);
    p.level_ = level_;
    ++per_level_[p.level_];
    ++live_;
    table_[slot] = std::move(process);
    p.on_added();
    return p.pid_;
}

Process* Kernel::get(Pid pid) const {
    return pid < table_.size() ? table_[pid].get() : nullptr;
}

std::size_t Kernel::count_processes(ObjId owner, std::uint16_t type) const {
    return static_cast<std::size_t>(std::count_if(table_.begin(), table_.end(), [&](const auto& p) {
        return p && !p->terminated() && p->owner() == owner && (type == kAnyType || p->type() == type);
    }));
}

// @orig 11D8:151D
Process* Kernel::find_process(ObjId owner, std::uint16_t type) const {
    for (std::size_t pid = 1; pid < table_.size(); ++pid) {
        Process* p = table_[pid].get();
        if (p && !p->terminated() && p->owner() == owner && p->type() == type) return p;
    }
    return nullptr;
}

// @orig 11D8:0D0E
std::size_t Kernel::fail_processes(ObjId owner, std::uint16_t type, std::uint8_t level) {
    std::size_t failed = 0;
    for (std::size_t pid = 1; pid < table_.size(); ++pid) {
        Process* p = table_[pid].get();
        if (!p || p->terminated() || p->runs_at_any_level() || p->level() != level) continue;
        if ((owner != 0 && p->owner() != owner) || (type != kAnyType && p->type() != type)) continue;
        p->fail();
        ++failed;
    }
    return failed;
}

// @orig 11D8:11FE
void Kernel::fail_all() {
    failing_all_ = true;
    for (std::size_t pid = 1; pid < table_.size(); ++pid) {
        Process* p = table_[pid].get();
        if (p && !p->terminated() && !p->runs_at_any_level()) p->fail();
    }
    failing_all_ = false;
}

// @orig 11D8:2A86
void Kernel::advance_time(std::uint32_t elapsed_us) {
    tick_remainder_ += std::uint64_t{elapsed_us} * kTicksPerSecond;
    ticks_ += static_cast<std::uint32_t>(tick_remainder_ / 1'000'000);
    tick_remainder_ %= 1'000'000;

    for (std::size_t pid = 1; pid < table_.size(); ++pid) {
        if (auto* timed = dynamic_cast<TimedProcess*>(table_[pid].get())) {
            if (runnable(*timed)) {
                timed->accumulate(elapsed_us);
            }
        }
    }
    reap();
}

// @orig 11D8:0255
bool Kernel::runnable(const Process& p) const {
    if (p.terminated() || p.suspended() || p.sleeping() || p.waiting()) {
        return false;
    }
    return p.level() == level_ || p.runs_at_any_level() || p.level() >= kFirstSystemLevel;
}

// @orig 11D8:0255
void Kernel::run_slice() {
    // Index loop: processes spawned during the slice with a higher pid run in
    // this same pass, as in the original table walk.
    interrupted_ = false;
    // DS:3A88, which the main loop (11D8:31CF) measures around each pass.
    slice_ticks_ = sliced_ ? ticks_ - slice_start_ : 0;
    slice_start_ = ticks_;
    sliced_ = true;
    for (std::size_t pid = 1; pid < table_.size() && !interrupted_; ++pid) {
        Process* p = table_[pid].get();
        if (!p) {
            continue;
        }
        if (p->sleeping_ && ticks_ >= p->sleep_until_) {
            p->sleeping_ = false;
        }
        if (runnable(*p) && !dynamic_cast<TimedProcess*>(p)) {
            p->run();
        }
        reap();
    }
    drop_empty_levels();
}

void Kernel::drop_empty_levels() {
    while (level_ > 1 && per_level_[level_] == 0) {
        --level_;
    }
}

void Kernel::raise_level(Process& p) {
    assert(level_ < kFirstSystemLevel - 1);
    ++level_;
    set_level(p, level_);
}

// @orig 11D8:19B5
void Kernel::set_level(Process& p, std::uint8_t level) {
    if (!p.terminated()) {
        --per_level_[p.level_];
        ++per_level_[level];
    }
    p.level_ = level;
}

// @orig 11E8:058E 11E8:046B 11E0:0000
void Kernel::on_terminated(Process& p) {
    --per_level_[p.level_];
    --live_;
    p.on_terminate();

    // Stop waiting: unregister from dependencies still pending.
    for (const auto& t : p.wait_expr_) {
        if (t.kind == Process::WaitToken::Kind::Pending) {
            if (Process* other = get(t.pid)) {
                std::erase(other->waiters_, p.pid_);
            }
        }
    }
    p.wait_expr_.clear();

    // Wake whoever waited for this one.
    const auto waiters = std::move(p.waiters_);
    p.waiters_.clear();
    for (Pid w : waiters) {
        if (Process* other = get(w); other && !other->terminated()) {
            other->dependency_done(p.pid_, p.result_);
        }
    }
}

// @orig 11D8:1DCC
void Kernel::reap() {
    for (std::size_t pid = 1; pid < table_.size(); ++pid) {
        if (table_[pid] && table_[pid]->terminated()) {
            table_[pid].reset();
        }
    }
    while (table_.size() > 1 && !table_.back()) {
        table_.pop_back();
    }
}

}  // namespace crusader
