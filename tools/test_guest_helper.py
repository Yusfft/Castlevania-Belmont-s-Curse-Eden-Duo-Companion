#!/usr/bin/env python3
"""Runs the guest helper of the load plan in an AArch64 emulator (unicorn), the way Eden Duo
installs it: the plan's patch and relocations applied to a main image, the helper's game calls
replaced by stubs that record their arguments.

    python3 tools/test_guest_helper.py <decompressed main image of build 1.1.0>

The image is the player's own (the main NSO's segments at their memory offsets); it is only
read to check the hooked instruction and run the real prologue around the patch.
"""
import json
import struct
import sys
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

ROOT = Path(__file__).resolve().parent.parent
PLAN = json.loads((ROOT / "package/dualscreen/guest/0F1169AA7D0FA4529C861F96ADEC306E00000000000000000000000000000000/load-plan.json").read_text())

MAIN = 0x80000000
MAIN_SIZE = 0x7400000
CODE = MAIN + MAIN_SIZE + 0x10000
MAILBOX = CODE + 0x10000
HEAP = 0xA0000000
STACK = 0xB0000000

FN = {"init": 0x5BE410, "new": 0x5BE780, "ctor": 0x1C311A0, "add": 0x1C2E7A0, "remove": 0x1C2EF60,
      "change": 0x1C2FDB0, "weapon": 0x1F546E0, "spell": 0x1F547B0, "charm": 0x1F54880, "uncharm": 0x1F53520}
RESUME = 0x40D4F04
AFM_SLOT, MMD_SLOT = 0x6D3F780, 0x6D444D8


def reloc(code, base, r, mailbox, code_addr):
    at = base + r["offset"]
    ins = struct.unpack_from("<I", code, r["offset"])[0]
    tgt = {"mailbox": mailbox, "main": MAIN, "code": code_addr}[r["target"]] + r["addend"]
    k = r["kind"]
    if k == "aarch64_adrp":
        pages = (tgt >> 12) - (at >> 12)
        imm = pages & 0x1FFFFF
        ins = (ins & 0x9F00001F) | ((imm & 3) << 29) | ((imm >> 2) << 5)
    elif k in ("aarch64_ldr64_lo12", "aarch64_ldr32_lo12"):
        sc = 3 if k.endswith("64_lo12") else 2
        ins = (ins & ~(0xFFF << 10)) | (((tgt & 0xFFF) >> sc) << 10)
    elif k == "aarch64_add_lo12":
        ins = (ins & ~(0xFFF << 10)) | ((tgt & 0xFFF) << 10)
    elif k == "aarch64_branch26":
        ins = (ins & 0xFC000000) | (((tgt - at) // 4) & 0x03FFFFFF)
    struct.pack_into("<I", code, r["offset"], ins)


class World:
    def __init__(self, image):
        self.uc = uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(MAIN, MAIN_SIZE)
        uc.mem_write(MAIN, image[:MAIN_SIZE])
        uc.mem_map(CODE, 0x10000)
        uc.mem_map(MAILBOX, 0x1000)
        uc.mem_map(HEAP, 0x100000)
        uc.mem_map(STACK - 0x10000, 0x10000)
        self.heap = HEAP
        # Patch and helper, relocated as Eden does.
        for p in PLAN["patches"]:
            assert image[p["offset"]:p["offset"] + 4].hex() == p["expected"], "hook bytes differ"
            rep = bytearray.fromhex(p["replacement"])
            for r in p["relocations"]:
                reloc(rep, MAIN + p["offset"], r, MAILBOX, CODE)
            uc.mem_write(MAIN + p["offset"], bytes(rep))
        code = bytearray.fromhex(PLAN["guest_code"]["bytes"])
        for r in PLAN["guest_code"]["relocations"]:
            reloc(code, CODE, r, MAILBOX, CODE)
        uc.mem_write(CODE, bytes(code))
        # Game functions -> stubs (a RET each; the hook records the call).
        for a in FN.values():
            uc.mem_write(MAIN + a, struct.pack("<I", 0xD65F03C0))
        self.calls = []
        uc.hook_add(UC_HOOK_CODE, self.on_code, begin=MAIN, end=MAIN + MAIN_SIZE)
        # ApplicationFlowManager: resolved class, statics with _isLoading.
        self.afm_statics = self.alloc(16)
        afm = self.cls(statics=self.afm_statics)
        self.put(MAIN + AFM_SLOT, afm)
        self.mmd_class = self.cls()
        self.put(MAIN + MMD_SLOT, self.mmd_class)

    def alloc(self, n):
        a = self.heap
        self.heap += (n + 15) & ~15
        return a

    def put(self, at, v, fmt="<Q"):
        self.uc.mem_write(at, struct.pack(fmt, v))

    def get(self, at, fmt="<Q"):
        return struct.unpack(fmt, self.uc.mem_read(at, struct.calcsize(fmt)))[0]

    def cls(self, parent=0, statics=0):
        k = self.alloc(0x140)
        self.put(k + 0x58, parent)
        self.put(k + 0xB8, statics)
        return k

    def obj(self, klass, size=0x80):
        o = self.alloc(size)
        self.put(o, klass)
        return o

    def on_code(self, uc, address, size, _):
        off = address - MAIN
        for name, a in FN.items():
            if off == a:
                regs = [uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                                 UC_ARM64_REG_X3, UC_ARM64_REG_X4)]
                s0 = struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM64_REG_S0) & 0xFFFFFFFF))[0]
                s1 = struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM64_REG_S1) & 0xFFFFFFFF))[0]
                self.calls.append((name, regs, s0, s1))
                # Clobber what a real call may clobber.
                for r in (UC_ARM64_REG_X9, UC_ARM64_REG_X10, UC_ARM64_REG_X11, UC_ARM64_REG_X12,
                          UC_ARM64_REG_X13, UC_ARM64_REG_X14, UC_ARM64_REG_X15, UC_ARM64_REG_X16,
                          UC_ARM64_REG_X17):
                    uc.reg_write(r, 0xDEAD0000DEAD0000)
                if name == "new":
                    o = self.obj(regs[0], 0x40)
                    uc.reg_write(UC_ARM64_REG_X0, o)
                    self.new_obj = o
                elif name == "add":
                    self.put(regs[2] + 0x10, 41, "<i")
                elif name == "charm":
                    uc.reg_write(UC_ARM64_REG_X0, regs[2])
        if off == RESUME:
            uc.emu_stop()

    def frame(self, mb):
        """One call of the hooked Update; returns the calls made."""
        uc = self.uc
        self.calls = []
        for off, v, fmt in mb:
            self.put(MAILBOX + off, v, fmt)
        sp = STACK - 0x100
        regs = {UC_ARM64_REG_SP: sp, UC_ARM64_REG_X0: 0x1111, UC_ARM64_REG_X1: 0x2222,
                UC_ARM64_REG_X19: 0x1919, UC_ARM64_REG_X20: 0x2020, UC_ARM64_REG_X21: 0x2121,
                UC_ARM64_REG_X22: 0x2222, UC_ARM64_REG_X29: 0x2929, UC_ARM64_REG_X30: 0x3030}
        for r, v in regs.items():
            uc.reg_write(r, v)
        uc.emu_start(MAIN + 0x40D4F00, MAIN + RESUME + 4, count=20000)
        assert uc.reg_read(UC_ARM64_REG_PC) == MAIN + RESUME, hex(uc.reg_read(UC_ARM64_REG_PC))
        assert uc.reg_read(UC_ARM64_REG_SP) == sp - 0x30, "stack"
        for r, v in regs.items():
            if r != UC_ARM64_REG_SP:
                assert uc.reg_read(r) == v, ("register not restored", r)
        assert self.get(sp - 0x30) == 0x2929 and self.get(sp - 0x28) == 0x3030, "displaced stp"
        return self.calls


def main():
    image = Path(sys.argv[1]).read_bytes()
    w = World(image)
    # An inventory with a weapon and a relic owned, two relic slots.
    k_item = w.cls()
    k_weapon, k_spell, k_charm = w.cls(k_item), w.cls(k_item), w.cls(k_item)
    k_sub = w.cls(k_weapon)  # a weapon subclass (WeaponFistItemData)
    k_inv = w.cls()
    weapon, fist, charm, spell = w.obj(k_weapon), w.obj(k_sub), w.obj(k_charm), w.obj(k_spell)
    stranger = w.obj(k_weapon)
    arr = w.obj(w.cls(), 0x20 + 8 * 8)
    w.put(arr + 0x18, 8)
    for i, o in enumerate([weapon, fist, charm, spell]):
        w.put(arr + 0x20 + 8 * i, o)
    lst = w.obj(w.cls(), 0x20)
    w.put(lst + 0x10, arr)
    w.put(lst + 0x18, 4, "<i")
    charms = w.obj(w.cls(), 0x30)
    w.put(charms + 0x18, 2)
    inv = w.obj(k_inv, 0x58)
    w.put(inv + 0x20, lst)
    w.put(inv + 0x40, charms)
    epoch = 7
    w.put(MAILBOX + 0xFFC, epoch, "<I")
    req = [0]

    def ask(cmd, target, tcls, o1=0, c1=0, o2=0, c2=0, o3=0, c3=0, i=0, x=0.0, y=0.0, token=epoch, magic=0x31484342):
        req[0] += 1
        calls = w.frame([(0, magic, "<I"), (0xC, cmd, "<I"), (0x10, token, "<I"), (0x14, i, "<i"),
                         (0x18, x, "<f"), (0x1C, y, "<f"), (0x20, target, "<Q"), (0x28, o1, "<Q"),
                         (0x30, o2, "<Q"), (0x38, o3, "<Q"), (0x40, tcls, "<Q"), (0x48, c1, "<Q"),
                         (0x50, c2, "<Q"), (0x58, c3, "<Q"), (0x8, req[0], "<I")])
        done = w.get(MAILBOX + 0x60, "<I")
        res = w.get(MAILBOX + 0x64, "<I")
        val = w.get(MAILBOX + 0x68, "<q")
        assert done == req[0], "done"
        return res, val, calls

    ok = 0
    count = 0
    def check(name, got, want_res, want_calls=None):
        nonlocal ok, count
        count += 1
        res, val, calls = got
        names = [c[0] for c in calls]
        good = res == want_res and (want_calls is None or names == want_calls)
        print("%-34s result %d value %d calls %s %s" % (name, res, val, names, "ok" if good else "FAILED"))
        ok += good
        return calls

    c = check("equip weapon", ask(1, inv, k_inv, weapon, k_weapon), 0, ["weapon"])
    assert c[0][1][:3] == [inv, weapon, 0]
    check("equip weapon subclass", ask(1, inv, k_inv, fist, k_weapon), 0, ["weapon"])
    check("weapon not owned", ask(1, inv, k_inv, stranger, k_weapon), 5, [])
    check("relic as weapon", ask(1, inv, k_inv, charm, k_weapon), 1, [])
    check("wrong inventory class", ask(1, inv, k_weapon, weapon, k_weapon), 1, [])
    check("equip arcana", ask(2, inv, k_inv, spell, k_spell), 0, ["spell"])
    c = check("equip relic slot 1", ask(3, inv, k_inv, charm, k_charm, i=1), 0, ["charm"])
    assert c[0][1][:4] == [inv, charm, 1, 0]
    check("relic slot 2 (only 2)", ask(3, inv, k_inv, charm, k_charm, i=2), 6, [])
    check("relic slot -1", ask(3, inv, k_inv, charm, k_charm, i=-1), 6, [])
    check("remove relic slot 0", ask(4, inv, k_inv, i=0), 0, ["uncharm"])
    check("stale token", ask(1, inv, k_inv, weapon, k_weapon, token=6), 3, [])
    check("bad command", ask(9, inv, k_inv), 2, [])
    # Before Eden's first Run the epoch is still 0: a request made then (token 0) is current.
    w.put(MAILBOX + 0xFFC, 0, "<I")
    check("epoch 0, token 0", ask(1, inv, k_inv, weapon, k_weapon, token=0), 0, ["weapon"])
    check("epoch 0, token 7", ask(1, inv, k_inv, weapon, k_weapon, token=7), 3, [])
    w.put(MAILBOX + 0xFFC, epoch, "<I")
    w.put(w.afm_statics, 1, "<B")
    check("game loading", ask(1, inv, k_inv, weapon, k_weapon), 4, [])
    w.put(w.afm_statics, 0, "<B")
    # Markers.
    k_comp, k_level, k_str, k_md, k_type = w.cls(), w.cls(), w.cls(), w.cls(), w.cls()
    comp, level, guid, md, typ = w.obj(k_comp), w.obj(k_level), w.obj(k_str), w.obj(k_md), w.obj(k_type)
    res = ask(5, comp, k_comp, level, k_level, guid, k_str, i=3, x=12.5, y=-4.25)
    c = check("add marker", res, 0, ["init", "new", "ctor", "add"])
    assert c[0][1][0] == MAIN + MMD_SLOT and c[1][1][0] == w.mmd_class
    ctor = c[2]
    assert ctor[1][0] == w.new_obj and ctor[1][1] == 3 and ctor[1][2] == guid and ctor[2] == 12.5 and ctor[3] == -4.25
    assert c[3][1][:3] == [comp, level, w.new_obj] and res[1] == 41
    check("add marker, bad level", ask(5, comp, k_comp, guid, k_level, guid, k_str), 1, [])
    c = check("remove marker", ask(6, comp, k_comp, level, k_level, i=41), 0, ["remove"])
    assert c[0][1][:3] == [comp, level, 41]
    c = check("change marker", ask(7, comp, k_comp, level, k_level, md, k_md, typ, k_type), 0, ["change"])
    assert c[0][1][:4] == [comp, level, md, typ]
    check("change marker, bad type", ask(7, comp, k_comp, level, k_level, md, k_md, md, k_type), 1, [])
    # No request: nothing happens, done stays.
    frames = w.get(MAILBOX + 4, "<I")
    calls = w.frame([])
    count += 1
    idle = calls == [] and w.get(MAILBOX + 4, "<I") == frames + 1
    print("%-34s %s" % ("idle frame (no new request)", "ok" if idle else "FAILED"))
    ok += idle
    # Without the magic the helper only counts frames.
    calls = w.frame([(0, 0, "<I"), (0x8, 999, "<I")])
    count += 1
    off = calls == [] and w.get(MAILBOX + 0x60, "<I") != 999
    print("%-34s %s" % ("mailbox not set up", "ok" if off else "FAILED"))
    ok += off
    print("%d / %d checks passed" % (ok, count))
    sys.exit(0 if ok == count else 1)


if __name__ == "__main__":
    main()
