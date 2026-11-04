#include "engine/process.hpp"

#include "engine/kernel.hpp"

#include <algorithm>
#include <cassert>

namespace crusader {

void Process::sleep(std::uint32_t ticks) {
    assert(kernel_ && "process must be added to the kernel first");
    sleeping_ = true;
    sleep_until_ = kernel_->ticks() + ticks;
}

// @orig 11E8:0F2F 11D8:1327
bool Process::add_wait_operand(Pid other, bool share_owner) {
    Process* target = kernel_ ? kernel_->get(other) : nullptr;
    if (!target || target->terminated() || target == this) {
        return false;
    }
    wait_expr_.push_back({WaitToken::Kind::Pending, other});
    target->waiters_.push_back(pid_);
    // Like the original, a process without an owner inherits its partner's
    // (Process_wait_for; not KERNEL_11d8_1327, the usecode's implies).
    if (!share_owner) return true;
    if (owner_ == 0) {
        owner_ = target->owner_;
    } else if (target->owner_ == 0) {
        target->owner_ = owner_;
    }
    return true;
}

// @orig 11E8:0F2F 11D8:1327
void Process::wait_for(Pid other, bool share_owner) {
    const bool had_operand = !wait_expr_.empty();
    if (add_wait_operand(other, share_owner) && had_operand) {
        wait_expr_.push_back({WaitToken::Kind::And, 0});
    }
}

// @orig ?: OR wait in the wait expression, builder not located
void Process::wait_for_any(const std::vector<Pid>& others) {
    // Already satisfied if any of them is gone.
    for (Pid p : others) {
        const Process* target = kernel_ ? kernel_->get(p) : nullptr;
        if (!target || target->terminated()) {
            return;
        }
    }
    const bool had_operand = !wait_expr_.empty();
    bool first = true;
    for (Pid p : others) {
        if (add_wait_operand(p)) {
            if (!first) {
                wait_expr_.push_back({WaitToken::Kind::Or, 0});
            }
            first = false;
        }
    }
    if (had_operand && !first) {
        wait_expr_.push_back({WaitToken::Kind::And, 0});
    }
}

// @orig 11E8:06E0
bool Process::evaluate_wait() const {
    std::vector<bool> stack;
    for (const WaitToken& t : wait_expr_) {
        switch (t.kind) {
            case WaitToken::Kind::Pending: stack.push_back(false); break;
            case WaitToken::Kind::Done: stack.push_back(true); break;
            case WaitToken::Kind::And:
            case WaitToken::Kind::Or: {
                assert(stack.size() >= 2);
                const bool b = stack.back();
                stack.pop_back();
                const bool a = stack.back();
                stack.back() = t.kind == WaitToken::Kind::And ? (a && b) : (a || b);
                break;
            }
        }
    }
    return !stack.empty() && stack.back();
}

// @orig 11E8:06E0
void Process::dependency_done(Pid pid, std::uint32_t result, bool failed) {
    bool changed = false;
    for (WaitToken& t : wait_expr_) {
        if (t.kind == WaitToken::Kind::Pending && t.pid == pid) {
            t.kind = WaitToken::Kind::Done;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    wake_result_ = result;
    wake_failed_ = failed;
    if (evaluate_wait()) {
        // Unregister from the dependencies still pending (OR satisfied early).
        for (const WaitToken& t : wait_expr_) {
            if (t.kind == WaitToken::Kind::Pending) {
                if (Process* other = kernel_->get(t.pid)) {
                    std::erase(other->waiters_, pid_);
                }
            }
        }
        wait_expr_.clear();
    }
}

void Process::terminate(std::uint32_t result) { finish(result, false); }

void Process::fail(std::uint32_t result) { finish(result, true); }

// @orig 11E8:112C 11E8:115C
void Process::finish(std::uint32_t result, bool failed) {
    if (terminated_) {
        return;
    }
    terminated_ = true;
    failed_ = failed;
    result_ = result;
    if (kernel_) {
        kernel_->on_terminated(*this);
    }
}

void TimedProcess::set_rate(std::uint32_t hz) {
    rate_hz_ = hz;
    period_us_ = hz ? 1'000'000u / hz : 0;
    accumulator_us_ = 0;
}

// @orig 11D8:2A86
void TimedProcess::accumulate(std::uint32_t elapsed_us) {
    if (period_us_ == 0) {
        return;
    }
    // Like the original: at most one tick per frame, the remainder carries over
    // (clamped so a long stall does not queue up a burst).
    accumulator_us_ = std::min(accumulator_us_ + elapsed_us, 2 * period_us_);
    if (accumulator_us_ < period_us_) {
        return;
    }
    accumulator_us_ -= period_us_;
    if (!in_tick_) {
        in_tick_ = true;
        tick();
        in_tick_ = false;
    }
}

}  // namespace crusader
