"""Checks src/engine/usecode/v101_intrinsic_numbering.hpp against the 1.01 code.

CRUSADER.EXE is a Phar Lap 286 program: an NE executable whose segment n has the
selector 0x0FF0 + 8 * n, the numbering of UNKCOFF.DAT and of the port's @orig
addresses. For each entry of the 1.01 intrinsic table, this script reads the
routine's machine code and measures the bytes of usecode arguments it reads
(the far call leaves them at [bp + 6] on; usecode pushes words, so an odd
count is rounded up), following the stubs that only hand them on ("push cs;
call near" at the entry). It compares that with the French binding the entry is
mapped to (src/engine/usecode/*_intrinsics.cpp, src/game/game.cpp):

- error: the binding reads further than the routine's arguments (a getter of
  one item mapped to a setter of a value, for instance);
- warning: the routine takes arguments and its binding ignores them (its
  Args is unnamed);
- warning: entries of one routine mapped to French numbers bound in separate
  places (they may do the same).

Usage: python scripts/check_v101_intrinsics.py <1.01 data dir>
Needs capstone (pip install capstone). Exits with 1 when there is an error.
"""
import collections
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_16, Cs

root = Path(__file__).resolve().parent.parent
data_dir = Path(sys.argv[1])
SIZES = {"byte": 1, "word": 2, "dword": 4}


def selector(segment):
    return 0x0FF0 + segment * 8


class NeImage:
    """The segments of the NE executable inside CRUSADER.EXE."""

    def __init__(self, path):
        self.data = data = path.read_bytes()
        ne = struct.unpack_from("<I", data, 0x3C)[0]
        if data[ne:ne + 2] != b"NE":
            raise SystemExit(f"{path}: no NE header")
        count, table, shift = (struct.unpack_from("<H", data, ne + o)[0] for o in (0x1C, 0x22, 0x32))
        self.segments = {}
        for i in range(count):
            offset, size, _flags, _minalloc = struct.unpack_from("<HHHH", data, ne + table + i * 8)
            self.segments[i + 1] = data[offset << shift:(offset << shift) + (size or 0x10000)] if offset else b""


class Routine:
    """What a routine reads of its arguments, following its near stubs."""

    def __init__(self, image, segment, offset):
        self.args = 0
        self.size = 0
        self._walk(image, segment, offset, set())
        self.args += self.args & 1

    def _walk(self, image, segment, offset, visited):
        if (segment, offset) in visited:
            return
        visited.add((segment, offset))
        code = image.segments.get(segment, b"")
        disassembler = Cs(CS_ARCH_X86, CS_MODE_16)
        todo, seen, previous, at_entry = [offset], set(), None, True
        while todo:
            pc = todo.pop()
            for ins in disassembler.disasm(code[pc:], pc):
                if ins.address in seen or len(seen) > 600:
                    break
                seen.add(ins.address)
                self.size += 1
                for m in re.finditer(r"(?:(byte|word|dword) ptr )?\[bp \+ (0x[0-9a-f]+|\d+)\]", ins.op_str):
                    k = int(m[2], 0)
                    size = SIZES.get(m[1], 4 if ins.mnemonic in ("les", "lds") else 2)
                    if k >= 6:
                        self.args = max(self.args, k - 6 + size)
                # A stub (nothing pushed yet but cs, then a near call): the far
                # call goes on within the segment, on the same frame.
                if ins.mnemonic == "call" and ins.op_str.startswith("0x") and previous == "push cs" and at_entry:
                    self._walk(image, segment, int(ins.op_str, 16), visited)
                if ins.mnemonic not in ("nop",) and f"{ins.mnemonic} {ins.op_str}" != "push cs":
                    at_entry = False
                if ins.mnemonic.startswith("j") and ins.op_str.startswith("0x"):
                    target = int(ins.op_str, 16)
                    if target > ins.address:
                        todo.append(target)
                    if ins.mnemonic == "jmp":
                        break
                previous = f"{ins.mnemonic} {ins.op_str}"
                if ins.mnemonic in ("ret", "retf", "iret"):
                    break


def bindings():
    """French number -> (binding id, bytes of arguments it reads, or None when
    it hands them on unread here)."""
    out = {}
    files = sorted((root / "src/engine/usecode").glob("*_intrinsics.cpp")) + [root / "src/game/game.cpp"]
    for path in files:
        text = path.read_text(errors="ignore")
        for m in re.finditer(r"\b(bind\w*|set_intrinsic)\(\s*(\{[^}]*\}|0x[0-9A-Fa-f]{3}\b)", text):
            if m[1] == "set_intrinsic" and text[m.start() - 1] not in ".>":
                continue  # the wrappers' own calls
            depth, end = 1, text.index("(", m.start()) + 1
            while depth and end < len(text):
                depth += {"(": 1, ")": -1}.get(text[end], 0)
                end += 1
            body = text[m.end():end]
            reads = [int(n) + 2 for n in re.findall(r"arg16\(a, (\d+)\)", body)]
            reads += [int(n) + 4 for n in re.findall(r"arg32\(a, (\d+)\)", body)]
            reads += [int(n or 0) + 4 for n in re.findall(r"item_arg\([^)]*\ba(?:, (\d+))?\)", body)]
            if "Item& i" in body or "ObjId id" in body:
                reads.append(4)
            if m[1] == "bind_pair":
                reads.append(6)  # the other item's id at offset 4
            need = max(reads, default=0)
            if not reads and not re.search(r"\bArgs\s*\)|\bArgs\s*/\*", body):
                need = None  # named Args handed on to a helper
            for n in re.findall(r"0x([0-9A-Fa-f]{3})\b", m[2]):
                out[int(n, 16)] = (f"{path.name}:{text.count(chr(10), 0, m.start()) + 1}", need)
    return out


image = NeImage(data_dir / "CRUSADER.EXE")
raw = (data_dir / "USECODE/UNKCOFF.DAT").read_bytes()
table = re.findall(r"^\s*0x([0-9A-F]{3,4}),\s*//\s*([0-9A-F]{3}) (\S+)(?: US \S+ (\S+))?",
                   (root / "src/engine/usecode/v101_intrinsic_numbering.hpp").read_text(), re.M)
bound = bindings()

errors = warnings = 0
routines, by_address = {}, collections.defaultdict(list)
for value, number, address, name in table:
    i, french = int(number, 16), int(value, 16)
    offset, segment = struct.unpack_from("<HH", raw, i * 4)
    if (segment, offset) not in routines:
        routines[segment, offset] = Routine(image, segment, offset)
    by_address[segment, offset].append((i, french))
    if french == 0xFFFF:
        continue
    r = routines[segment, offset]
    where, need = bound.get(french, (None, 0))
    label = f"{i:03X} {address} -> {french:03X} {name or ''}"
    if where is None:
        print(f"note: {label}: French number not bound")
    elif need is not None and need > r.args:
        errors += 1
        print(f"error: {label}: the binding ({where}) reads {need} bytes, the routine takes {r.args}")
    elif need == 0 and r.args > 0:
        warnings += 1
        print(f"warning: {label}: the routine takes {r.args} bytes, the binding ({where}) ignores them")
for (segment, offset), ids in sorted(by_address.items()):
    reached = {bound[f][0] for _, f in ids if f in bound}
    if len(reached) > 1:
        warnings += 1
        print(f"warning: routine {selector(segment):04X}:{offset:04X} reaches bindings in several places: "
              + ", ".join(f"{i:03X}->{f:03X}" for i, f in ids))
print(f"{len(table)} entries, {len(routines)} routines: {errors} error(s), {warnings} warning(s)")
sys.exit(1 if errors else 0)
