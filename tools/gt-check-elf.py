#!/usr/bin/env python3
"""gt-check-elf.py - is this payload ELF really the UI, and is it loadable?

A PS5 payload cannot be run here, so "it compiled" is a weak claim.  This
script checks the properties that decide whether it works on the console
instead: the ELF header and segment layout a loader has to be able to map,
that every relocation is position-independent (one stray R_X86_64_64 and the
loader maps the image somewhere else and jumps into the void), that the
imports are PS5 modules rather than a desktop libc, and that the parts of the
UI - the files it serves, the tile registration, the log parser - are actually
in the binary and not only in the source tree.

  python3 tools/gt-check-elf.py dist/*.elf --payload ghost-toothAPI.elf

Works on any ELF64/x86-64 file, needs no tooling but python3.  Exit status is
0 only when every file passes every check.
"""

import argparse
import os
import struct
import sys

EM_X86_64 = 0x3E
ET_DYN = 3
PT_LOAD = 1
DT_NEEDED = 1
SHT_PROGBITS, SHT_REL, SHT_RELA = 1, 9, 4
SHT_STRTAB = 3
R_RELATIVE, R_GLOB_DAT, R_JUMP_SLOT = 8, 6, 7

# Present in a correct build; missing from one that was stripped or linked
# half-hearted.  (where it comes from, marker, why it matters)
MARKERS = [
    ("assets/index.html", 'id="device-list"', "the device picker markup"),
    ("assets/main.css", "--accent", "the stylesheet"),
    ("assets/main.js", "POLL_MS", "the poll loop and gamepad pump"),
    ("src/gt_tile.c", '"applicationCategoryType"', "Media-tile registration"),
    ("src/gt_tile.c", '"deeplinkUri"', "the tile opens the UI, no self needed"),
    ("src/gt_log.c", "avdtp: streaming", "the payload log parser"),
    ("src/gt_log.c", "no headphones yet, scanning again", "scan-state parsing"),
    ("src/gt_link.c", "ui-devices.txt", "the device cache next to the ini"),
    ("src/main.c", "--uninstall-tile", "the CLI around it all"),
    ("src/gt.h", "Ghost Tooth", "the default tile name"),
]

# libkernel_web = syscall wrappers, libSceLibcInternal = libc and pthreads,
# libSceNet = the sockets the HTTP server uses.
REQUIRED_IMPORTS = ["libkernel_web", "libSceLibcInternal", "libSceNet"]
FORBIDDEN_IMPORTS = ["libc.so", "ld-linux", "libpthread.so", "libc++.so.1"]


class Report:
    def __init__(self, path):
        self.path = path
        self.lines = []
        self.failed = 0

    def ok(self, msg):
        self.lines.append(("ok", msg))

    def bad(self, msg):
        self.lines.append(("FAIL", msg))
        self.failed += 1

    def dump(self):
        print(f"== {self.path}")
        for kind, msg in self.lines:
            print(f"  {kind:<4} {msg}")


def parse_elf(data, rep):
    """Return (entry, phdrs) or None; header/segment sanity is checked here."""
    if len(data) < 64 or data[:4] != b"\x7fELF":
        rep.bad("not an ELF (too short or missing the \\x7fELF magic)")
        return None
    if data[4] != 2 or data[5] != 1:
        rep.bad(f"not ELF64 little-endian (class {data[4]}, data {data[5]})")
        return None
    (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, _flags,
     _ehsize, e_phentsize, e_phnum, _shentsize, e_shnum, e_shstrndx) = \
        struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    if e_machine != EM_X86_64:
        rep.bad(f"e_machine is 0x{e_machine:x}, not x86-64")
        return None
    if e_type != ET_DYN:
        rep.bad(f"e_type is {e_type}, not ET_DYN - a payload has to be a PIE "
                "so the loader can map it anywhere")
        return None
    rep.ok("ELF64, little endian, x86-64, ET_DYN (PIE)")

    if e_phoff + e_phnum * e_phentsize > len(data) or e_phoff < 64:
        rep.bad(f"program headers point past the end of the file "
                f"({e_phnum} x {e_phentsize} B at {e_phoff:#x}) - truncated "
                "download?")
        return None
    phdrs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<QQQQQQ", data, off + 8)
        phdrs.append(dict(type=p_type, flags=p_flags, offset=p_offset,
                          vaddr=p_vaddr, filesz=p_filesz, memsz=p_memsz,
                          align=p_align))
    loads = [p for p in phdrs if p["type"] == PT_LOAD]
    if not loads:
        rep.bad("no PT_LOAD segment - nothing for the loader to map")
        return None
    broken = 0
    for idx, p in enumerate(loads):
        if p["align"] < 0x1000 or p["align"] & (p["align"] - 1):
            rep.bad(f"PT_LOAD #{idx} alignment {p['align']:#x} is not a "
                    "page-sized power of two")
            broken += 1
        if p["filesz"] > p["memsz"]:
            rep.bad(f"PT_LOAD #{idx} filesz exceeds memsz")
            broken += 1
        if p["offset"] + p["filesz"] > len(data):
            rep.bad(f"PT_LOAD #{idx} extends past the end of the file")
            broken += 1
    if not any(p["flags"] & 1 for p in loads):
        rep.bad("no executable PT_LOAD segment")
    if not any(p["vaddr"] <= e_entry < p["vaddr"] + max(p["memsz"], 1)
               for p in loads):
        rep.bad(f"entry point {e_entry:#x} is outside every PT_LOAD")
    if broken:
        return None
    rep.ok(f"{len(loads)} PT_LOAD segments, entry {e_entry:#x} mapped, "
           f"every segment page aligned and inside the file")
    return dict(entry=e_entry, phdrs=phdrs, loads=loads, e_shoff=e_shoff,
                e_shnum=e_shnum, e_shstrndx=e_shstrndx)


def section_names(data, hdr, rep):
    """.name -> (type, file offset, size).  Empty when the file was stripped."""
    off, count, idx = hdr["e_shoff"], hdr["e_shnum"], hdr["e_shstrndx"]
    if not off or not count:
        rep.bad("no section headers (stripped build) - cannot verify contents")
        return {}
    if off + count * 64 > len(data) or idx >= count:
        rep.bad(f"section headers are out of bounds ({count} x 64 B at "
                f"{off:#x}) - the file is truncated")
        return {}
    strtab = struct.unpack_from("<IIQQQQ", data, off + idx * 64)
    strtab_off = strtab[4]
    secs = {}
    for i in range(min(count, (len(data) - off) // 64)):
        base = off + i * 64
        if base + 64 > len(data):
            break
        sh_name, sh_type = struct.unpack_from("<II", data, base)
        sh_offset, sh_size = struct.unpack_from("<QQ", data, base + 24)
        end = data.find(b"\x00", strtab_off + sh_name)
        name = data[strtab_off + sh_name:end].decode("ascii", "replace")
        if name:
            secs[name] = (sh_type, sh_offset, sh_size)
    if not secs:
        rep.bad("section headers are unreadable")
    return secs


def needed(data, secs, rep):
    dyn, strtab = secs.get(".dynamic"), secs.get(".dynstr")
    if not dyn or not strtab:
        if dyn or strtab:
            rep.bad("only one of .dynamic/.dynstr is present - the file is "
                    "truncated or malformed")
            return []
        rep.bad("no .dynamic/.dynstr - a payload must import its symbols "
                "dynamically, a static link cannot be relocated by the loader")
        return []
    if dyn[1] + dyn[2] > len(data) or strtab[1] > len(data):
        rep.bad(".dynamic/.dynstr point past the end of the file - truncated")
        return []
    libs = []
    base_off, base_size = dyn[1], dyn[2]
    str_off = strtab[1]
    for i in range(min(base_size, 4096) // 16):
        tag, val = struct.unpack_from("<qQ", data, base_off + i * 16)
        if tag == 0:
            break
        if tag == DT_NEEDED:
            end = data.find(b"\x00", str_off + val)
            libs.append(data[str_off + val:end].decode("ascii", "replace"))
    return libs


def check_relocs(data, secs, rep):
    total, bad = 0, {}
    for name, (sh_type, off, size) in secs.items():
        # SHT_REL / SHT_RELA only: .relr.dyn (RELR, a compact delta encoding)
        # and debug sections start with ".rel" too but have other layouts.
        if sh_type not in (SHT_REL, SHT_RELA):
            continue
        is_rela = sh_type == SHT_RELA
        entsize = 24 if is_rela else 16
        if off + size > len(data):
            rep.bad(f"{name} points past the end of the file - truncated")
            continue
        for i in range(size // entsize):
            fmt = "<QQq" if is_rela else "<QQ"
            fields = struct.unpack_from(fmt, data, off + i * entsize)
            r_info = fields[1]
            total += 1
            r_type = r_info & 0xFFFFFFFF
            if r_type not in (R_RELATIVE, R_GLOB_DAT, R_JUMP_SLOT):
                bad[r_type] = bad.get(r_type, 0) + 1
    if not total:
        rep.bad("no REL/RELA relocation sections at all - a PIE has to have "
                "some, or it is not dynamically linked")
        return
    if bad:
        detail = ", ".join(f"type {t} x{n}" for t, n in sorted(bad.items()))
        rep.bad(f"non-PIC relocations ({detail}) - the loader cannot fix "
                "absolute addresses, this build is not relocatable")
        return
    rep.ok(f"{total} relocations, all RELATIVE / GLOB_DAT / JUMP_SLOT")


def check(path, payload_path, rep):
    try:
        data = open(path, "rb").read()
    except OSError as exc:
        rep.bad(f"cannot read: {exc}")
        return
    hdr = parse_elf(data, rep)
    if hdr is None:
        return
    secs = section_names(data, hdr, rep)
    libs = needed(data, secs, rep)
    if libs:
        joined = " ".join(libs)
        missing = [lib for lib in REQUIRED_IMPORTS if lib not in joined]
        if missing:
            rep.bad(f"imports are missing {', '.join(missing)} (got: {joined})")
        else:
            rep.ok(f"imports {len(libs)} PS5 modules: {joined}")
        hurt = [lib for lib in libs if any(b in lib for b in FORBIDDEN_IMPORTS)]
        if hurt:
            rep.bad(f"links desktop/system libraries that are not on a PS5: "
                    f"{', '.join(hurt)}")
        else:
            rep.ok("no desktop libc or loader dependency")
    if secs:
        check_relocs(data, secs, rep)
    missing = [m for _src, m, _why in MARKERS if m.encode() not in data]
    if missing:
        for marker, src, why in MARKERS:
            if marker in missing:
                rep.bad(f"{marker!r} from {src} is absent ({why})")
        rep.bad(f"{len(missing)} of {len(MARKERS)} UI markers are missing - "
                "this is not the UI that was built from this tree")
    else:
        rep.ok(f"all {len(MARKERS)} UI markers are in the image")
    if payload_path:
        img = open(payload_path, "rb").read()
        if img in data:
            rep.ok(f"embeds {payload_path} verbatim ({len(img)} B, "
                   "byte-identical) - it can install the payload itself")
        else:
            rep.ok(f"does not embed {payload_path} - the portable build, which "
                   "expects it in /data/ghost-toothAPI/ already")


def main(argv):
    ap = argparse.ArgumentParser(description="verify a ghost-tooth-ui payload ELF")
    ap.add_argument("elf", nargs="+", help="payload ELF(s) to inspect")
    ap.add_argument("--payload", default="ghost-toothAPI.elf",
                    help="payload image to look for (default: %(default)s, "
                         "skipped when the file is absent)")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="only print a line per file, on failure")
    args = ap.parse_args(argv)

    payload = args.payload if os.path.exists(args.payload) else None
    if args.payload and payload is None:
        print(f"note: {args.payload} not found - skipping the embed check")
    status = 0
    for path in args.elf:
        rep = Report(path)
        check(path, payload, rep)
        if rep.failed or not args.quiet:
            rep.dump()
        if rep.failed:
            status = 1
            print(f"  -> {path}: {rep.failed} problem(s)")
        elif not args.quiet:
            print(f"  -> {path}: ok ({len(rep.lines)} checks)")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
