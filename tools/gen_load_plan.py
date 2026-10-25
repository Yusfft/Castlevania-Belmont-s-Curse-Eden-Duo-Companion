#!/usr/bin/env python3
"""Writes the package's load plan (guest helper) from native/helper/bc_guest_helper.S.

    python3 tools/gen_load_plan.py            # needs clang (aarch64 target) and pyelftools

The helper is assembled to an ELF object; its relocations against the symbols `MB` (the guest
mailbox) and `M` (the main module) become the load plan's "mailbox" and "main" relocations.
The plan's one patch replaces the first instruction of ApplicationFlowManager::Update (build
1.1.0) with a branch to the helper; the game's 4 bytes appear only as the expected value.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "native" / "helper" / "bc_guest_helper.S"
BUILD = "0F1169AA7D0FA4529C861F96ADEC306E00000000000000000000000000000000"
OUT = ROOT / "package" / "dualscreen" / "guest" / BUILD / "load-plan.json"

# The main module of build 1.1.0 as loaded: text, rodata, data (memory offset, size) and bss.
LAYOUT = {"segments": [{"location": 0, "size": 85144424},
                       {"location": 85147648, "size": 24555520},
                       {"location": 109703168, "size": 7150208}],
          "bss_size": 3782016}
HOOK = 0x40D4F00          # MotherBase.ApplicationFlowManager.ApplicationFlowManager::Update
HOOK_EXPECTED = "fd7bbda9"  # stp x29, x30, [sp, #-0x30]!  (the helper's first instruction)
MAILBOX_SIZE = 4096
EPOCH_OFFSET = 0xFFC

KINDS = {
    "R_AARCH64_ADR_PREL_PG_HI21": "aarch64_adrp",
    "R_AARCH64_ADD_ABS_LO12_NC": "aarch64_add_lo12",
    "R_AARCH64_LDST64_ABS_LO12_NC": "aarch64_ldr64_lo12",
    "R_AARCH64_LDST32_ABS_LO12_NC": "aarch64_ldr32_lo12",
    "R_AARCH64_CALL26": "aarch64_branch26",
    "R_AARCH64_JUMP26": "aarch64_branch26",
}
TARGETS = {"MB": "mailbox", "M": "main"}


def main():
    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / "helper.o"
        subprocess.run(["clang", "--target=aarch64-linux-gnu", "-c", str(SRC), "-o", str(obj)], check=True)
        with open(obj, "rb") as f:
            elf = ELFFile(f)
            text = elf.get_section_by_name(".text")
            code = text.data()
            symtab = elf.get_section_by_name(".symtab")
            relocs = []
            for sec in elf.iter_sections():
                if not isinstance(sec, RelocationSection) or sec.name != ".rela.text":
                    continue
                for r in sec.iter_relocations():
                    sym = symtab.get_symbol(r["r_info_sym"])
                    from elftools.elf.descriptions import describe_reloc_type
                    kind = describe_reloc_type(r["r_info_type"], elf)
                    if kind not in KINDS or sym.name not in TARGETS:
                        sys.exit("unexpected relocation %s against %r at %#x" % (kind, sym.name, r["r_offset"]))
                    relocs.append({"offset": r["r_offset"], "kind": KINDS[kind],
                                   "target": TARGETS[sym.name], "addend": r["r_addend"]})
            entry = next(s for s in symtab.iter_symbols() if s.name == "bc_helper")["st_value"]
    if entry != 0:
        sys.exit("bc_helper must be at the start of .text")
    if code[:4].hex() != HOOK_EXPECTED:
        sys.exit("the helper must start with the instruction it displaces")
    plan = {
        "title_id": "0100BA4026E94000",
        "build_id": BUILD,
        "module": "main",
        "format": 2,
        "layout": LAYOUT,
        "mailbox_size": MAILBOX_SIZE,
        "mailbox_epoch_offset": EPOCH_OFFSET,
        "patches": [{"offset": HOOK, "expected": HOOK_EXPECTED, "replacement": "00000014",
                     "relocations": [{"offset": 0, "kind": "aarch64_branch26", "target": "code", "addend": 0}]}],
        "guest_code": {"bytes": code.hex(), "relocations": sorted(relocs, key=lambda r: r["offset"])},
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(plan, indent=1) + "\n", encoding="utf-8")
    print("wrote", OUT, len(code), "bytes,", len(relocs), "relocations")


if __name__ == "__main__":
    main()
