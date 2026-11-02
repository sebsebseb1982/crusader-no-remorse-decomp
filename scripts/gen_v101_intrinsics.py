"""Rebuilds src/engine/usecode/v101_intrinsic_numbering.hpp from the 1.01 data.

The port binds the intrinsics under the numbers of the French 1.08 usecode, and
kUsToFrench (src/engine/usecode/intrinsic_numbering.cpp) gives, for each number
of the US 1.23 usecode, the French number of the same routine. The English 1.01
usecode (USECODE/UNKCOFF.DAT, 308 entries) numbers its intrinsics nearly as the
US one: this script finds, for each 1.01 number, the US number of the same
routine, and writes the French number kUsToFrench gives for it. Signals:

- addresses: UNKCOFF.DAT holds the routine of each entry as an unrelocated
  segment:offset. Segment s is French segment 0x0FF0 + 8 * s, and the offsets
  match the French executable's, so an address cited in the sources
  ("Actor::I_setDead (10F0:2010)") names the 1.01 entries at that address.
  They take the US number of that name closest to theirs, wherever it is: a
  few routines moved in the table;
- routines: 1.01 entries at the same address are one routine, and so are the
  French numbers of one bind({...}) list of src/engine/usecode, or else the US
  numbers ScummVM gives the same name;
- segments: the classes of ScummVM's names (Item::, Actor::...) live in a few
  code segments, learnt from the entries named by an address;
- order: the other entries follow the US order, with a few gaps.

An order-keeping alignment (Needleman-Wunsch) of the two tables, pinned by
the entries named by an address, scores the other signals, then is run again with each 1.01 address
tied to the bind list most of its entries reached, until it no longer changes.
Entries left without a match are written as 0xFFFF (unbound: the call is
counted, nothing runs).

Usage: python scripts/gen_v101_intrinsics.py <1.01 data dir>
The report (moved and split routines, doubtful and unmatched entries) goes to
stderr; matches settled by reviewing it go in OVERRIDES. Then check the table
against the 1.01 machine code: python scripts/check_v101_intrinsics.py <dir>.
"""
import collections
import re
import struct
import sys
from pathlib import Path

GAP = -3

# Matches settled by hand, which the signals above miss: 1.01 number -> US
# number, with the reason. They pin the alignment like the cited addresses.
OVERRIDES = {
    # 10A8:1311, before I_pop (1338), I_push (1352) and I_destroy (1365): the
    # one wrapper of ITEMCACH's stack left, called by the start pad's egg of
    # mission 1 (class 2317).
    0x01B: 0x020,  # Item::I_popToCoords
    # 10A8:38A2, after I_getQHi (383E), I_setQHi (385A), I_getQLo (3889);
    # called with an item and a value (6 bytes).
    0x01A: 0x03B,  # Item::I_setQLo
    # 10A8:0FC4, called with an item alone (4 bytes) for its result: FREE
    # (class 0xA0C) compares it with I_getFamilyOfType before a chest opens.
    0x03B: 0x03C,  # Item::I_getFamily
    # 10A8:10DB, after I_legalCreateAtPoint (104E); called with 14 bytes, the
    # arguments of I_legalCreateAtCoords.
    0x062: 0x019,  # Item::I_legalCreateAtCoords
    # 11D8:0972, in KERNEL.C (I_resetRef is 11D8:0C5E); called with an item
    # and a type (4 bytes) by RECHFLNS, waiting for the healer it started.
    0x0E8: 0x0EF,  # Kernel::I_getNumProcesses
    # 10A8:3AD7, with the movie intrinsics (3A5B, 3A9F, 3ABB): the same 12
    # bytes (name, flags) handed to another player of FMV.C (1458:4131).
    0x05C: 0x05E,  # MovieGump::I_playMovieOverlay
    # 11B8:0000 and 0006: stubs calling 11B8:098F and 0932, the routines of
    # I_hideStatusGump and I_showStatusGump (world_intrinsics.cpp).
    0x0F6: 0x05D,  # CruStatusGump::I_hideStatusGump
    0x0F7: 0x05F,  # CruStatusGump::I_showStatusGump
    # 1000:0064 and 006A set and clear the byte DS:00A1, which the game view's
    # key handler (1000:1553, 19C6) reads as the French DS:5FFB: no menu on
    # Escape, no key. The 1.01 table has no I_*CrusaderTeleporting.
    0x06E: 0x072,  # Ultima8Engine::I_setCruStasis
    0x070: 0x075,  # Ultima8Engine::I_clrCruStasis
    # 1430:06E6: fadeToBlack(rate) (1430:04EA) at 40 Hz, as I_fadeFromBlack
    # (1430:07CC) is fadeToGamePal(rate) (1430:06F4) at 40 Hz.
    0x071: 0x077,  # PaletteFaderProcess::I_fadeToBlack
}

root = Path(__file__).resolve().parent.parent
data_dir = Path(sys.argv[1])
usecode = root / "src/engine/usecode"


def french_address(segment, offset):
    return (0x0FF0 + segment * 8, offset)


def us_table():
    """kUsToFrench: the French number and ScummVM's name of each US number."""
    text = (usecode / "intrinsic_numbering.cpp").read_text()
    rows = re.findall(r"^\s*0x([0-9A-F]{3}),\s*//\s*([0-9A-F]{3})\s+(\S+)", text, re.M)
    assert [int(us, 16) for _, us, _ in rows] == list(range(len(rows))), "kUsToFrench out of order"
    return [int(fr, 16) for fr, _, _ in rows], [name for _, _, name in rows]


def bind_lists():
    """The French numbers of each bind({...}) / for (n : {...}) list."""
    lists = []
    for path in sorted(usecode.glob("*_intrinsics.cpp")):
        text = path.read_text(errors="ignore")
        for m in re.finditer(r"(?:bind\w*\(|:)\s*\{", text):
            depth, end = 1, m.end()
            while depth and end < len(text):
                depth += {"{": 1, "}": -1}.get(text[end], 0)
                end += 1
            numbers = [int(n, 16) for n in re.findall(r"0x([0-9A-Fa-f]{3})\b", text[m.end():end - 1])]
            if len(numbers) > 1:
                lists.append(numbers)
    return lists


def cited_names():
    names = collections.defaultdict(set)
    # "Name (SEG:OFF" or "Name (0xNNN, SEG:OFF", the French number first; the
    # name may carry its arguments ("I_hasKeycard(n) (10F0:3CE8)").
    pattern = re.compile(r"\b([A-Z]\w*::I_\w+)(?:\([^)]*\))?\s*\((?:0x[0-9A-F]{3},\s*)?([0-9A-F]{4}):([0-9A-F]{4})")
    for path in (root / "src").rglob("*.[ch]pp"):
        for m in pattern.finditer(path.read_text(errors="ignore")):
            names[(int(m[2], 16), int(m[3], 16))].add(m[1])
    return names


raw = (data_dir / "USECODE/UNKCOFF.DAT").read_bytes()
entries = [french_address(*struct.unpack_from("<HH", raw, i * 4)[::-1]) for i in range(len(raw) // 4)]
us_to_french, us_names = us_table()
french_group = {}
for g, numbers in enumerate(bind_lists()):
    for n in numbers:
        french_group.setdefault(n, g)
# The routine of a US number: its bind list, else ScummVM's name ("0": unknown).
group_of = {}
for u, f in enumerate(us_to_french):
    if f in french_group:
        group_of[u] = french_group[f]
    elif us_names[u] != "0":
        group_of[u] = us_names[u]
group_size = collections.Counter(group_of.values())
names_at = cited_names()
uses = collections.Counter(entries)

# Code moved within a segment between 1.01 and 1.08 (ACTOR.C from about
# 10F0:2200 on is 0x55 bytes earlier in 1.01): a cited address with no entry
# of its own is looked for shifted, by a shift that 5 cited addresses or more
# of its segment agree on (fewer happen by chance).
entry_set = set(entries)
votes = collections.Counter()
for (segment, offset) in names_at:
    if (segment, offset) in entry_set:
        continue
    for delta in range(-0x200, 0x201):
        if delta and (segment, offset + delta) in entry_set:
            votes[segment, delta] += 1
shifts = collections.defaultdict(list)
for (segment, delta), n in votes.items():
    if n >= 5:
        shifts[segment].append(delta)
for (segment, offset), names in list(names_at.items()):
    if (segment, offset) in entry_set:
        continue
    for delta in sorted(shifts.get(segment, ()), key=abs):
        moved = (segment, offset + delta)
        if moved in entry_set and moved not in names_at:
            names_at[moved] = set(names)
            break

# Cited addresses: each entry takes the closest US number of that name.
anchors = {}
for i, address in enumerate(entries):
    candidates = [u for u, name in enumerate(us_names) if name in names_at.get(address, ())]
    if candidates:
        anchors[i] = min(candidates, key=lambda u: (u in anchors.values(), abs(u - i)))
cited = dict(anchors)
anchors.update(OVERRIDES)
anchored_us = set(anchors.values())
# The code segments each class of ScummVM's names (Item::, Actor::...) was
# seen in among the anchors: a class seen in 3 entries or more is looked for
# in another segment only reluctantly.
class_segments = collections.defaultdict(collections.Counter)
for i, u in cited.items():
    class_segments[us_names[u].split("::")[0]][entries[i][0]] += 1


def score(i, u, tied):
    # The anchors pin the alignment where they keep the order; a moved one is
    # left out of it (a gap on both sides).
    if i in anchors:
        return 20 if u == anchors[i] else -20
    if u in anchored_us:
        return -20
    address = entries[i]
    seen = class_segments[us_names[u].split("::")[0]]
    s = -2 if sum(seen.values()) >= 3 and address[0] not in seen else 0
    g = tied.get(address)
    if g is not None:
        return s + (8 if group_of.get(u) == g else -8)
    shared_here, shared_there = uses[address] > 1, group_size[group_of.get(u)] > 1
    if shared_here and shared_there:
        return s + (3 if uses[address] == group_size[group_of[u]] else 1)
    return s + (-3 if shared_here != shared_there else 1)


def align(tied):
    n, m = len(entries), len(us_names)
    best = [[0] * (m + 1) for _ in range(n + 1)]
    move = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        best[i][0], move[i][0] = i * GAP, 1
    for j in range(1, m + 1):
        best[0][j], move[0][j] = j * GAP, 2
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            best[i][j], move[i][j] = max((best[i - 1][j - 1] + score(i - 1, j - 1, tied), 0),
                                         (best[i - 1][j] + GAP, 1), (best[i][j - 1] + GAP, 2))
    mapping, i, j = {}, n, m
    while i or j:
        if move[i][j] == 0:
            i, j = i - 1, j - 1
            mapping[i] = j
        elif move[i][j] == 1:
            i -= 1
        else:
            j -= 1
    return mapping


def ties(mapping):
    votes = collections.defaultdict(collections.Counter)
    for i, u in list(mapping.items()) + list(anchors.items()):
        if uses[entries[i]] > 1 and u in group_of:
            votes[entries[i]][group_of[u]] += 1
    return {address: c.most_common(1)[0][0] for address, c in votes.items()}


tied, mapping = {}, None
for _ in range(10):
    new = align(tied)
    if new == mapping:
        break
    mapping, tied = new, ties(new)

# Every 1.01 entry of a tied address takes a US number of its bind list: the
# one aligned with it, else the first of the list.
first_of = {}
for u, g in sorted(group_of.items()):
    first_of.setdefault(g, u)
result = dict(anchors)
for i, address in enumerate(entries):
    if i in anchors:
        continue
    u = mapping.get(i)
    g = tied.get(address)
    if g is not None and group_of.get(u) != g:
        u = first_of[g]
    if u is not None:
        result[i] = u

report = sys.stderr
for segment, deltas in sorted(shifts.items()):
    print(f"segment {segment:04X} shifted by " + ", ".join(f"{d:+#x}" for d in deltas), file=report)
print(f"{len(entries)} entries, {len(result)} matched, {len(cited)} by a cited address, "
      f"{len(OVERRIDES)} by hand", file=report)
for i, u in sorted(anchors.items()):
    if abs(u - i) > 8:
        print(f"  moved: {i:03X} {entries[i][0]:04X}:{entries[i][1]:04X} is US {u:03X} {us_names[u]}", file=report)
by_address = collections.defaultdict(list)
for i, address in enumerate(entries):
    by_address[address].append(i)
for address, ids in sorted(by_address.items()):
    reached = {group_of.get(result[i], ("alone", result[i])) for i in ids if i in result}
    if len(ids) > 1 and len(reached) > 1:
        print(f"  routine {address[0]:04X}:{address[1]:04X} split: " +
              ", ".join(f"{i:03X}->{us_names[result[i]]}" if i in result else f"{i:03X}->?" for i in ids), file=report)
# Matches to review: a class of ScummVM's names whose cited routines all live
# in one segment, found in another.
for i, u in sorted(result.items()):
    seen = class_segments[us_names[u].split("::")[0]]
    if i not in anchors and sum(seen.values()) >= 3 and len(seen) == 1 and entries[i][0] not in seen:
        print(f"  doubtful: {i:03X} {entries[i][0]:04X}:{entries[i][1]:04X} as US {u:03X} {us_names[u]} "
              f"(that class cited in {' '.join(f'{s:04X}' for s in sorted(seen))})", file=report)
missing = [i for i in range(len(entries)) if i not in result]
print("unmatched 1.01 entries: " + (" ".join(f"{i:03X}" for i in missing) or "none"), file=report)
reached = {us_to_french[u] for u in result.values()} | {f for f, g in french_group.items() if g in tied.values()}
unused = sorted({u for u in range(len(us_names)) if us_to_french[u] not in reached})
print("US numbers no 1.01 entry reached: " + " ".join(f"{u:03X} {us_names[u]}" for u in unused), file=report)

rows = []
for i, (segment, offset) in enumerate(entries):
    u = result.get(i)
    if u is None:
        rows.append(f"    0xFFFF, // {i:03X} {segment:04X}:{offset:04X} unmatched")
    else:
        how = " (by hand)" if i in OVERRIDES else ""
        rows.append(f"    0x{us_to_french[u]:03X},  // {i:03X} {segment:04X}:{offset:04X} US {u:03X} {us_names[u]}{how}")

out = usecode / "v101_intrinsic_numbering.hpp"
out.write_text(
    "#pragma once\n"
    "// Generated by scripts/gen_v101_intrinsics.py from USECODE/UNKCOFF.DAT of the\n"
    "// 1.01 data: do not edit. Index: number called by the 1.01 usecode; value: the\n"
    "// French number bound to the same routine (0xFFFF: none). Comments: the 1.01\n"
    "// number, the routine's address in the French executable, the US number\n"
    "// matched and ScummVM's name for it.\n\n"
    "#include <array>\n"
    "#include <cstdint>\n\n"
    "namespace crusader {\n\n"
    f"constexpr std::array<std::uint16_t, {len(entries)}> kV101ToFrench = {{\n" + "\n".join(rows) + "\n};\n\n"
    "}  // namespace crusader\n",
    newline="\n",
)
print(f"wrote {out.relative_to(root)}", file=report)
