#include "engine/usecode/uc_machine.hpp"

#include "engine/usecode/uc_process.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace crusader {

namespace {

// Bounds-checked operand reader over a class's bytecode.
struct CodeReader {
    std::span<const std::uint8_t> code;
    std::uint32_t pos;

    std::uint8_t u8() {
        if (pos >= code.size()) throw std::runtime_error("ip past end of class");
        return code[pos++];
    }
    std::int8_t s8() { return static_cast<std::int8_t>(u8()); }
    std::uint16_t u16() {
        const std::uint16_t lo = u8();
        return static_cast<std::uint16_t>(lo | u8() << 8);
    }
    std::int16_t s16() { return static_cast<std::int16_t>(u16()); }
    std::uint32_t u32() {
        const std::uint32_t lo = u16();
        return lo | std::uint32_t{u16()} << 16;
    }
    // @orig none: byte reader
    std::span<const std::uint8_t> bytes(std::size_t n) {
        if (pos + n > code.size()) throw std::runtime_error("ip past end of class");
        auto s = code.subspan(pos, n);
        pos += static_cast<std::uint32_t>(n);
        return s;
    }
};

std::uint16_t offset16(std::uint16_t base, int delta) {
    return static_cast<std::uint16_t>(static_cast<int>(base) + delta);
}

// Search area reserved on the stack by the `loop` opcode, per search type.
// @orig 1418:038B; loop opcodes
std::size_t loop_area_size(int type) {
    switch (type) {
        case 2:
        case 3: return 0x3A;  // area search around an item
        case 4: return 0x2A;  // container search
        case 5: return 0x2C;  // recursive container search
        case 6: return 0x43;  // surface search
        default: return 0;
    }
}

}  // namespace

UCMachine::UCMachine(Kernel& kernel, const UsecodeFile& usecode)
    : kernel_(kernel), usecode_(usecode), globals_(0x100, 0), intrinsics_(0x200) {}

void UCMachine::set_intrinsic(std::uint16_t number, Intrinsic fn) {
    if (number >= intrinsics_.size()) intrinsics_.resize(number + 1);
    intrinsics_[number] = std::move(fn);
}

// Item::callUsecodeEvent (US 0086:0E3A): the process that UCProcess's
// constructor (0086:0EEC) makes runs at once through vtable slot 0
// (UCProcess::run, 0086:10DA), as a spawned one does; the call answers its
// pid, or 0 when it is already over. An event started while a map loads
// (cachein, event 4) thus runs before the teleport moves the fast area and
// fails the processes of the items left behind: mission 7's egg (MISS7, in
// a corner of map 13) gives its fusion pack there.
// @orig 1420:0E3A
Pid UCMachine::call_event(std::uint16_t cid, std::uint32_t event, ObjId item, std::span<const std::uint8_t> args) {
    const Pid pid = start_event(cid, event, item, args);
    auto* p = dynamic_cast<UCProcess*>(kernel_.get(pid));
    if (!p) return 0;
    execute(*p);
    return p->terminated() ? 0 : pid;
}

// The process alone, queued for the next pass (UCProcess, 0086:0EEC, without
// its first run).
// @orig 1420:0EEC
Pid UCMachine::start_event(std::uint16_t cid, std::uint32_t event, ObjId item, std::span<const std::uint8_t> args) {
    const std::uint32_t offset = usecode_.event_offset(cid, event);
    if (offset == 0) {
        return 0;
    }
    auto p = std::make_unique<UCProcess>(*this, cid, static_cast<std::uint16_t>(offset), object_ptr(item), 2, args);
    p->set_owner(item);
    return kernel_.add(std::move(p));
}

// @orig 1418:038B; pointer opcodes
bool UCMachine::dereference(std::uint32_t ptr, std::span<std::uint8_t> out) {
    const std::uint32_t seg = ptr >> 16;
    const std::uint16_t off = static_cast<std::uint16_t>(ptr);
    std::fill(out.begin(), out.end(), std::uint8_t{0});
    if (seg == kSegGlobal) {
        if (off + out.size() > globals_.size()) return false;
        std::copy_n(globals_.begin() + off, out.size(), out.begin());
        return true;
    }
    if (seg == kSegObject) {
        if (out.size() >= 1) out[0] = static_cast<std::uint8_t>(off);
        if (out.size() >= 2) out[1] = static_cast<std::uint8_t>(off >> 8);
        return true;
    }
    if (seg == kSegString) {
        return false;
    }
    auto* owner = dynamic_cast<UCProcess*>(kernel_.get(static_cast<Pid>(seg)));
    if (!owner) return false;
    auto src = owner->stack().bytes(off, out.size());
    std::copy(src.begin(), src.end(), out.begin());
    return true;
}

// @orig 1418:038B; pointer opcodes
bool UCMachine::assign(std::uint32_t ptr, std::span<const std::uint8_t> in) {
    const std::uint32_t seg = ptr >> 16;
    const std::uint16_t off = static_cast<std::uint16_t>(ptr);
    if (seg == kSegGlobal) {
        if (off + in.size() > globals_.size()) return false;
        std::copy(in.begin(), in.end(), globals_.begin() + off);
        return true;
    }
    if (seg == kSegObject || seg == kSegString) {
        return false;
    }
    auto* owner = dynamic_cast<UCProcess*>(kernel_.get(static_cast<Pid>(seg)));
    if (!owner) return false;
    auto dst = owner->stack().bytes(off, in.size());
    std::copy(in.begin(), in.end(), dst.begin());
    return true;
}

void UCMachine::fault(UCProcess& p, std::string message) {
    faults_.push_back({p.pid(), p.class_id_, p.ip_, std::move(message)});
    p.fail(0);
}

// @orig 1418:038B
void UCMachine::execute(UCProcess& p) {
    std::uint32_t count = 0;
    bool yield = false;
    // A process blocked on entry does not run. One that blocks itself with
    // `implies` keeps running: the original's loop ends only on suspend, ret
    // and the like (local_36), never on the wait state, so the pop_temp and
    // suspend that follow an implies run in the same slice.
    if (p.waiting() || p.sleeping()) return;
    while (!yield && !p.terminated() && !kernel_.slice_interrupted()) {
        if (++count > budget_) {
            break;  // runaway loop: give other processes a chance
        }
        try {
            if (!step(p, yield)) {
                return;
            }
        } catch (const std::exception& e) {
            fault(p, e.what());
            return;
        }
    }
}

// Executes one instruction. Returns false after a fault.
// @orig 1418:038B
bool UCMachine::step(UCProcess& p, bool& yield) {
    if (!usecode_.has_class(p.class_id_)) {
        fault(p, "no such class");
        return false;
    }
    CodeReader in{usecode_.code(p.class_id_), p.ip_};
    UCStack& st = p.stack_;
    const std::uint8_t op = in.u8();
    ++executed_;

    auto pop_s16 = [&] { return static_cast<std::int16_t>(st.pop2()); };
    auto pop_s32 = [&] { return static_cast<std::int32_t>(st.pop4()); };
    auto push_bool = [&](bool v) { st.push2(v ? 1 : 0); };
    auto bp = [&](int delta) { return offset16(p.bp_, delta); };

    switch (op) {
        // ---- pops into locals / registers
        case 0x00: { const auto d = in.s8(); st.write(bp(d), st.pop2() & 0xFF, 1); break; }
        case 0x01: { const auto d = in.s8(); st.write(bp(d), st.pop2(), 2); break; }
        case 0x02: { const auto d = in.s8(); st.write(bp(d), st.pop4(), 4); break; }
        case 0x03: {
            const auto d = in.s8();
            const auto n = in.u8();
            const auto src = st.top(n);
            auto dst = st.bytes(bp(d), n);
            std::copy(src.begin(), src.end(), dst.begin());
            st.add_sp(n);
            break;
        }
        case 0x08: p.set_wake_result(st.pop4()); break;  // same register as 0x6D
        case 0x09: {
            const auto d = in.s8();
            const auto size = in.u8();
            const bool slist = in.s8() != 0;
            const std::uint16_t index = static_cast<std::uint16_t>(st.pop2() - 1);
            UCList* l = heap_.list(static_cast<std::uint16_t>(st.read(bp(d), 2)));
            if (!l) {
                fault(p, "pop_elem: invalid list");
                return false;
            }
            const auto elem = st.top(size);
            if (index >= l->size()) {
                l->data.resize((index + 1) * l->element_size, 0);
            } else if (slist) {
                heap_.free_string(l->at2(index));
            }
            std::copy(elem.begin(), elem.end(), l->data.begin() + static_cast<std::ptrdiff_t>(index * l->element_size));
            st.add_sp(size);
            break;
        }

        // ---- pushes
        case 0x0A: st.push2(static_cast<std::uint16_t>(in.s8())); break;
        case 0x0B: st.push2(in.u16()); break;
        case 0x0C: st.push4(in.u32()); break;
        case 0x0D: {
            const auto len = in.u16();
            const auto text = in.bytes(len);
            in.u8();  // NUL terminator
            st.push2(heap_.new_string(std::string(text.begin(), text.end())));
            break;
        }
        case 0x0E: {
            const auto size = in.u8();
            const auto count = in.u8();
            // Elements are on the stack, first pushed deepest; list order = push order.
            UCList l{size, {}};
            for (int i = count - 1; i >= 0; --i) {
                l.append(st.bytes(static_cast<std::uint32_t>(st.sp() + i * size), size));
            }
            st.add_sp(size * count);
            st.push2(heap_.new_list(std::move(l)));
            break;
        }

        // ---- calls
        case 0x0F: {
            const auto arg_bytes = in.u8();
            const auto called = in.u16();
            const std::uint16_t number = called < numbering_.size() ? numbering_[called] : called;
            const std::vector<std::uint8_t> args(st.top(arg_bytes).begin(), st.top(arg_bytes).end());
            p.ip_ = static_cast<std::uint16_t>(in.pos);
            if (number < intrinsics_.size() && intrinsics_[number]) {
                p.temp_ = intrinsics_[number](*this, p, args);
            } else {
                ++unbound_calls_[number];
                p.temp_ = 0;
            }
            return !p.failed();
        }
        case 0x11: {
            const auto cid = in.u16();
            const auto fn = in.u16();
            const std::uint32_t target = usecode_.event_offset(cid, fn);
            if (target == 0) {
                fault(p, "call: class has no such function");
                return false;
            }
            p.ip_ = static_cast<std::uint16_t>(in.pos);
            p.call(cid, static_cast<std::uint16_t>(target));
            return true;
        }
        case 0x12: p.temp_ = st.pop2(); break;
        case 0x13: p.temp_ = st.pop4(); break;

        // ---- arithmetic (rhs on top)
        case 0x14: { const auto b = st.pop2(); st.push2(static_cast<std::uint16_t>(st.pop2() + b)); break; }
        case 0x15: { const auto b = st.pop4(); st.push4(st.pop4() + b); break; }
        case 0x16: {
            const auto b = st.pop2();
            const auto a = st.pop2();
            const std::string* sa = heap_.string(a);
            const std::string* sb = heap_.string(b);
            const std::string joined = (sa ? *sa : std::string()) + (sb ? *sb : std::string());
            heap_.free_string(a);
            heap_.free_string(b);
            st.push2(heap_.new_string(joined));
            break;
        }
        case 0x17: {
            const auto b = st.pop2();
            const auto a = st.pop2();
            UCList* la = heap_.list(a);
            UCList* lb = heap_.list(b);
            if (la && lb) {
                la->data.insert(la->data.end(), lb->data.begin(), lb->data.end());
                heap_.free_list(b);
                st.push2(a);
            } else {
                st.push2(la ? a : b);
            }
            break;
        }
        case 0x19:
        case 0x1A: {
            in.u8();  // element size, always 2 for string lists
            const auto b = st.pop2();
            const auto a = st.pop2();
            UCList* la = heap_.list(a);
            UCList* lb = heap_.list(b);
            if (!la || !lb) {
                st.push2(la ? a : b);
                break;
            }
            auto has = [&](const UCList& l, const std::string& s) {
                for (std::size_t i = 0; i < l.size(); ++i) {
                    const std::string* e = heap_.string(l.at2(i));
                    if (e && *e == s) return true;
                }
                return false;
            };
            UCList result{2, {}};
            if (op == 0x19) {  // union without duplicates
                result = *la;
                for (std::size_t i = 0; i < lb->size(); ++i) {
                    const std::string* s = heap_.string(lb->at2(i));
                    if (s && !has(result, *s)) result.append(lb->at(i));
                    else heap_.free_string(lb->at2(i));
                }
                heap_.free_list(a);
                heap_.free_list(b);
            } else {  // a minus b
                for (std::size_t i = 0; i < la->size(); ++i) {
                    const std::string* s = heap_.string(la->at2(i));
                    if (s && !has(*lb, *s)) result.append(la->at(i));
                    else heap_.free_string(la->at2(i));
                }
                heap_.free_list(a);
                heap_.free_string_list(b);
            }
            st.push2(heap_.new_list(std::move(result)));
            break;
        }
        case 0x1B: {
            in.u8();
            const auto b = st.pop2();
            const auto a = st.pop2();
            UCList* la = heap_.list(a);
            UCList* lb = heap_.list(b);
            if (la && lb) {
                for (std::size_t i = 0; i < lb->size(); ++i) la->remove(lb->at(i));
                heap_.free_list(b);
                st.push2(a);
            } else {
                st.push2(la ? a : b);
            }
            break;
        }
        case 0x1C: { const auto b = st.pop2(); st.push2(static_cast<std::uint16_t>(st.pop2() - b)); break; }
        case 0x1D: { const auto b = st.pop4(); st.push4(st.pop4() - b); break; }
        case 0x1E: { const auto b = st.pop2(); st.push2(static_cast<std::uint16_t>(st.pop2() * b)); break; }
        case 0x1F: { const auto b = st.pop4(); st.push4(st.pop4() * b); break; }
        // Division and remainder by zero give 0 (UCMachine::execProcess,
        // opcodes 0x20..0x23): HOVER's move (class 0xA0D, fn 0x20) divides
        // by the largest of its deltas, all 0 when the platform is already
        // on its waypoint.
        case 0x20:
        case 0x22: {
            const auto b = pop_s16();
            const auto a = pop_s16();
            st.push2(b == 0 ? 0 : static_cast<std::uint16_t>(op == 0x20 ? a / b : a % b));
            break;
        }
        case 0x21:
        case 0x23: {
            const auto b = pop_s32();
            const auto a = pop_s32();
            st.push4(b == 0 ? 0 : static_cast<std::uint32_t>(op == 0x21 ? a / b : a % b));
            break;
        }

        // ---- comparisons and logic
        case 0x24: { const auto b = st.pop2(); push_bool(st.pop2() == b); break; }
        case 0x25: { const auto b = st.pop4(); push_bool(st.pop4() == b); break; }
        case 0x26: {
            const auto b = st.pop2();
            const auto a = st.pop2();
            const std::string* sa = heap_.string(a);
            const std::string* sb = heap_.string(b);
            push_bool(sa && sb && *sa == *sb);
            heap_.free_string(a);
            heap_.free_string(b);
            break;
        }
        case 0x28: { const auto b = pop_s16(); push_bool(pop_s16() < b); break; }
        case 0x29: { const auto b = pop_s32(); push_bool(pop_s32() < b); break; }
        case 0x2A: { const auto b = pop_s16(); push_bool(pop_s16() <= b); break; }
        case 0x2B: { const auto b = pop_s32(); push_bool(pop_s32() <= b); break; }
        case 0x2C: { const auto b = pop_s16(); push_bool(pop_s16() > b); break; }
        case 0x2D: { const auto b = pop_s32(); push_bool(pop_s32() > b); break; }
        case 0x2E: { const auto b = pop_s16(); push_bool(pop_s16() >= b); break; }
        case 0x2F: { const auto b = pop_s32(); push_bool(pop_s32() >= b); break; }
        case 0x30: push_bool(st.pop2() == 0); break;
        case 0x31: st.push4(st.pop4() == 0 ? 1 : 0); break;
        case 0x32: { const auto b = st.pop2(); const auto a = st.pop2(); push_bool(a && b); break; }
        case 0x33: { const auto b = st.pop4(); const auto a = st.pop4(); st.push4(a && b ? 1 : 0); break; }
        case 0x34: { const auto b = st.pop2(); const auto a = st.pop2(); push_bool(a || b); break; }
        case 0x35: { const auto b = st.pop4(); const auto a = st.pop4(); st.push4(a || b ? 1 : 0); break; }
        case 0x36: { const auto b = st.pop2(); push_bool(st.pop2() != b); break; }
        case 0x37: { const auto b = st.pop4(); push_bool(st.pop4() != b); break; }
        case 0x38: {
            const auto size = in.u8();
            const bool slist = in.u8() != 0;
            const auto list_id = st.pop2();
            const std::vector<std::uint8_t> elem(st.top(size).begin(), st.top(size).end());
            st.add_sp(size);
            bool found = false;
            if (UCList* l = heap_.list(list_id)) {
                if (slist) {
                    const std::string* s = heap_.string(static_cast<std::uint16_t>(elem[0] | elem[1] << 8));
                    for (std::size_t i = 0; s && i < l->size() && !found; ++i) {
                        const std::string* e = heap_.string(l->at2(i));
                        found = e && *e == *s;
                    }
                } else {
                    found = l->contains(elem);
                }
            }
            if (slist) {
                heap_.free_string(static_cast<std::uint16_t>(elem[0] | elem[1] << 8));
                heap_.free_string_list(list_id);
            } else {
                heap_.free_list(list_id);
            }
            push_bool(found);
            break;
        }
        case 0x39: { const auto b = st.pop2(); st.push2(st.pop2() & b); break; }
        case 0x3A: { const auto b = st.pop2(); st.push2(st.pop2() | b); break; }
        case 0x3B: st.push2(static_cast<std::uint16_t>(~st.pop2())); break;
        case 0x3C: { const auto b = st.pop2(); st.push2(static_cast<std::uint16_t>(st.pop2() << (b & 15))); break; }
        case 0x3D: { const auto b = st.pop2(); st.push2(static_cast<std::uint16_t>(pop_s16() >> (b & 15))); break; }

        // ---- locals
        case 0x3E: { const auto d = in.s8(); st.push2(static_cast<std::uint16_t>(static_cast<std::int8_t>(st.read(bp(d), 1)))); break; }
        case 0x3F: { const auto d = in.s8(); st.push2(static_cast<std::uint16_t>(st.read(bp(d), 2))); break; }
        case 0x40: { const auto d = in.s8(); st.push4(st.read(bp(d), 4)); break; }
        case 0x41: { const auto d = in.s8(); st.push2(heap_.duplicate_string(static_cast<std::uint16_t>(st.read(bp(d), 2)))); break; }
        case 0x42: {
            const auto d = in.s8();
            const auto size = in.u8();
            const auto id = heap_.duplicate_list(static_cast<std::uint16_t>(st.read(bp(d), 2)));
            st.push2(id ? id : heap_.new_list(UCList{size, {}}));
            break;
        }
        case 0x43: {
            const auto d = in.s8();
            const auto id = heap_.duplicate_string_list(static_cast<std::uint16_t>(st.read(bp(d), 2)));
            st.push2(id ? id : heap_.new_list(UCList{2, {}}));
            break;
        }
        case 0x44: {
            const auto size = in.u8();
            const bool slist = in.u8() != 0;
            const std::uint16_t index = static_cast<std::uint16_t>(st.pop2() - 1);
            const UCList* l = heap_.list(st.pop2());
            if (!l || index >= l->size()) {
                st.push_zero(size);
            } else if (slist) {
                st.push2(heap_.duplicate_string(l->at2(index)));
            } else {
                st.push(l->at(index));
            }
            break;
        }
        case 0x45: {
            const auto d = in.s8();
            const auto n = in.u8();
            const std::vector<std::uint8_t> copy(st.bytes(bp(d), n).begin(), st.bytes(bp(d), n).end());
            st.push(copy);
            break;
        }
        case 0x4B: { const auto d = in.s8(); st.push4(stack_ptr(p.pid(), bp(d))); break; }
        case 0x4C: {
            const auto n = in.u8();
            const auto ptr = st.pop4();
            st.push_zero(n);
            if (!dereference(ptr, st.bytes(st.sp(), n))) {
                fault(p, "push_indirect: bad pointer");
                return false;
            }
            break;
        }
        case 0x4D: {
            const auto n = in.u8();
            const auto ptr = st.pop4();
            if (!assign(ptr, st.top(n))) {
                fault(p, "pop_indirect: bad pointer");
                return false;
            }
            st.add_sp(n);
            break;
        }
        case 0x4E: {
            const auto addr = in.u16();
            const auto size = in.u8();  // bytes in Crusader
            std::uint16_t v = 0;
            for (std::size_t i = 0; i < size && i < 2 && addr + i < globals_.size(); ++i) {
                v = static_cast<std::uint16_t>(v | globals_[addr + i] << (8 * i));
            }
            st.push2(v);
            break;
        }
        case 0x4F: {
            const auto addr = in.u16();
            const auto size = in.u8();
            const auto v = st.pop2();
            for (std::size_t i = 0; i < size && addr + i < globals_.size(); ++i) {
                globals_[addr + i] = i < 2 ? static_cast<std::uint8_t>(v >> (8 * i)) : 0;
            }
            break;
        }

        // ---- control flow
        case 0x50:
            if (p.ret()) {
                p.terminate(p.wake_result());
                yield = true;
            }
            return true;
        case 0x51: {
            const auto rel = in.s16();
            if (st.pop2() == 0) in.pos = static_cast<std::uint32_t>(static_cast<int>(in.pos) + rel);
            break;
        }
        case 0x52: {
            const auto rel = in.s16();
            in.pos = static_cast<std::uint32_t>(static_cast<int>(in.pos) + rel);
            break;
        }
        case 0x53: yield = true; break;
        case 0x54: {
            in.u16();  // always 01 01
            const Pid target = st.pop2();
            const Pid waiter = st.pop2();
            st.push2(target);
            if (Process* w = kernel_.get(waiter)) {
                // KERNEL_11d8_1327: no owner passed on, unlike Process_wait_for.
                // A no-op if the target is already gone.
                w->wait_for(target, false);
            }
            break;
        }
        case 0x57: {
            const auto arg_bytes = in.u8();
            const auto this_size = in.u8();
            const auto cid = in.u16();
            const auto fn = in.u16();
            const auto this_ptr = st.pop4();
            const std::uint32_t target = usecode_.event_offset(cid, fn);
            p.ip_ = static_cast<std::uint16_t>(in.pos);
            if (target == 0) {
                p.temp_ = 0;
                return true;
            }
            const std::vector<std::uint8_t> args(st.top(arg_bytes).begin(), st.top(arg_bytes).end());
            auto child = std::make_unique<UCProcess>(*this, cid, static_cast<std::uint16_t>(target), this_ptr,
                                                     this_size, args);
            UCProcess& ref = *child;
            p.temp_ = kernel_.add(std::move(child));
            execute(ref);  // a spawned process runs at once until it first yields
            // Already over: the pid pushed is 0 and its result goes where
            // push_proc_result (0x6D) reads it, as no wake-up will bring it.
            if (ref.terminated()) {
                p.temp_ = 0;
                p.set_wake_result(ref.result());
            }
            return !p.failed();
        }
        case 0x59: st.push2(p.pid()); break;
        case 0x5A: {
            auto n = in.u8();
            if (n & 1) ++n;
            st.push_zero(n);
            break;
        }
        case 0x5B: in.u16(); break;
        case 0x5C: in.u16(); in.bytes(9); break;
        case 0x5D: st.push2(static_cast<std::uint16_t>(p.temp_ & 0xFF)); break;
        case 0x5E: st.push2(static_cast<std::uint16_t>(p.temp_)); break;
        case 0x5F: st.push4(p.temp_); break;
        case 0x60: st.push4(static_cast<std::uint32_t>(static_cast<std::int32_t>(pop_s16()))); break;
        case 0x61: st.push2(static_cast<std::uint16_t>(st.pop4())); break;
        case 0x62: { const auto d = in.s8(); heap_.free_string(static_cast<std::uint16_t>(st.read(bp(d), 2))); break; }
        case 0x63: { const auto d = in.s8(); heap_.free_string_list(static_cast<std::uint16_t>(st.read(bp(d), 2))); break; }
        case 0x64: { const auto d = in.s8(); heap_.free_list(static_cast<std::uint16_t>(st.read(bp(d), 2))); break; }
        case 0x65: { const auto d = in.s8(); heap_.free_string(static_cast<std::uint16_t>(st.read(offset16(st.sp(), d), 2))); break; }
        case 0x66: { const auto d = in.s8(); heap_.free_list(static_cast<std::uint16_t>(st.read(offset16(st.sp(), d), 2))); break; }
        case 0x67: { const auto d = in.s8(); heap_.free_string_list(static_cast<std::uint16_t>(st.read(offset16(st.sp(), d), 2))); break; }
        case 0x69: { const auto d = in.s8(); st.push4(string_ptr(static_cast<std::uint16_t>(st.read(bp(d), 2)))); break; }
        case 0x6B: st.push4(string_ptr(st.pop2())); break;
        case 0x6C: {
            const auto d = in.s8();
            const auto kind = in.u8();
            const auto handle = static_cast<std::uint16_t>(st.read(bp(d), 2));
            std::uint16_t copy = 0;
            switch (kind) {
                case 1: copy = heap_.duplicate_string(handle); break;
                case 2: copy = heap_.duplicate_string_list(handle); break;
                case 3: copy = heap_.duplicate_list(handle); break;
                default: fault(p, "param_pid_change: bad type"); return false;
            }
            st.write(bp(d), copy, 2);
            p.free_on_terminate_.emplace_back(copy, kind);
            break;
        }
        case 0x6D: st.push4(p.wake_result()); break;
        case 0x6E: st.add_sp(-in.s8()); break;
        case 0x6F: { const auto d = in.s8(); st.push4(stack_ptr(p.pid(), offset16(st.sp(), -d))); break; }

        // ---- item search loops
        case 0x70: {
            const auto var = in.s8();
            const auto script_size = in.u8();
            const auto type = in.u8();
            const auto a = st.pop2();
            const auto b = st.pop2();
            const std::size_t area = loop_area_size(type);
            if (area == 0 || script_size > 0x20 || area < script_size + 8u) {
                fault(p, "loop: unsupported search");
                return false;
            }
            const std::vector<std::uint8_t> script(st.top(script_size).begin(), st.top(script_size).end());
            st.add_sp(script_size);
            UCList found{2, {}};
            if (item_search_) {
                for (ObjId id : item_search_(type, a, b, script)) {
                    const std::uint8_t e[2] = {static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id >> 8)};
                    found.append(e);
                }
            }
            st.push_zero(area - script_size - 8);
            st.push(script);
            st.push2(script_size);
            st.push2(static_cast<std::uint16_t>(var));
            st.push2(0);
            st.push2(heap_.new_list(std::move(found)));
            [[fallthrough]];
        }
        case 0x73: {
            const std::uint16_t sp0 = st.sp();
            const auto list_id = static_cast<std::uint16_t>(st.read(sp0, 2));
            auto index = static_cast<std::uint16_t>(st.read(sp0 + 2, 2));
            const auto var = static_cast<std::int16_t>(st.read(sp0 + 4, 2));
            const UCList* l = heap_.list(list_id);
            if (!l) {
                fault(p, "loopnext: invalid item list");
                return false;
            }
            bool valid = false;
            while (index < l->size()) {
                const ObjId id = l->at2(index);
                st.write(bp(var), id, 2);
                if (!item_exists_ || item_exists_(id)) {
                    valid = true;
                    break;
                }
                ++index;
            }
            if (valid) {
                st.write(sp0 + 2, index + 1u, 2);
                st.push2(1);
            } else {
                heap_.free_list(list_id);
                st.push2(0);
            }
            break;
        }
        case 0x74: st.push1(in.u8()); break;
        case 0x75:
        case 0x76: {
            const auto var = in.s8();
            const auto size = in.u8();
            const auto rel = in.s16();
            auto index = static_cast<std::uint16_t>(st.read(st.sp(), 2));
            const auto list_id = static_cast<std::uint16_t>(st.read(st.sp() + 2u, 2));
            index = index == 0xFFFF ? 0 : static_cast<std::uint16_t>(index + 1);
            const UCList* l = heap_.list(list_id);
            if (!l || index >= l->size()) {
                if (op == 0x75) heap_.free_list(list_id);
                else heap_.free_string_list(list_id);
                st.add_sp(4);
                in.pos = static_cast<std::uint32_t>(static_cast<int>(in.pos) + rel);
            } else {
                st.write(st.sp(), index, 2);
                const auto elem = l->at(index);
                auto dst = st.bytes(bp(var), std::min<std::size_t>(size, elem.size()));
                std::copy_n(elem.begin(), dst.size(), dst.begin());
            }
            break;
        }
        case 0x77: {
            p.set_owner(st.pop2());
            p.set_type(st.pop2());
            break;
        }
        case 0x78:
            if (kernel_.count_processes(p.owner(), p.type()) > 1) {
                p.terminate(0);
                yield = true;
            }
            break;
        case 0x79: st.push4(global_ptr(in.u16())); break;

        default: {
            char msg[48];
            std::snprintf(msg, sizeof msg, "invalid opcode 0x%02X", op);
            fault(p, msg);
            return false;
        }
    }
    p.ip_ = static_cast<std::uint16_t>(in.pos);
    return true;
}

}  // namespace crusader
