#!/usr/bin/env python3
"""
qt-host-identity — assert that the logos-qt-host a consumer actually LINKS is
the one in that consumer's own closure.

WHY THIS SHAPE (all three points measured, see the header of the nix check):

  * The artifact keeps NO store path.  logos_host_qt links liblogos_qt_host.a
    statically; its entire runtime closure contains zero "-logos-qt-host-"
    paths.  A resolved-store-path comparison is blind to the one artifact that
    actually broke.

  * Symbol-set containment is UNSOUND.  A correctly built logos_host_qt is
    missing 223 of its own qt-host's 609 defined symbols to function-level DCE
    (logos_qt_arg_decode.cpp.o contributes 154 of 342).  No subset or superset
    predicate survives that.

  * A moc metaobject blob is ONE INDIVISIBLE LINKER OBJECT.  --gc-sections
    takes it whole or not at all, so byte-identity is DCE-proof; and the blob
    encodes every Q_INVOKABLE name, signature and parameter of the class.  It
    is also the exact ABI that broke: the failure was
    QMetaObject::invokeMethod(logosAPI, "currentCallerJson") -> "No such
    method", which IS a metaobject-content mismatch.  Asserting metaobject
    identity therefore catches the NEXT Qt-facing API for free, rather than
    only currentCallerJson.

Exit 0 = PASS, 1 = FAIL (identity violated), 2 = FAIL (vacuous / could not look).
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# Minimal ELF reader.
#
# Deliberately not a shell-out to objdump: `objdump -s --start-address` on a
# RELOCATABLE object dumps every section that overlaps the address range, since
# each .o section starts at 0 -- it silently returns the wrong bytes with no
# error.  That was measured while designing this check.  Reading the section
# headers directly makes the .o and the .so cases the same code path.
# ---------------------------------------------------------------------------

class ElfError(Exception):
    pass


class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        d = self.data
        if len(d) < 64 or d[:4] != b"\x7fELF":
            raise ElfError("not an ELF file")
        if d[4] != 2:
            raise ElfError("only ELF64 is supported")
        little = d[5] == 1
        self.end = "<" if little else ">"
        self.etype = struct.unpack_from(self.end + "H", d, 16)[0]
        e_shoff, = struct.unpack_from(self.end + "Q", d, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(self.end + "HHH", d, 0x3A)
        if e_shoff == 0 or e_shnum == 0:
            raise ElfError("no section headers (stripped to segments only)")
        self.sections = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
             sh_link, sh_info, sh_align, sh_entsize) = struct.unpack_from(
                self.end + "IIQQQQIIQQ", d, off)
            self.sections.append(dict(
                name_off=sh_name, type=sh_type, flags=sh_flags, addr=sh_addr,
                offset=sh_offset, size=sh_size, link=sh_link, info=sh_info,
                entsize=sh_entsize))
        shstr = self.sections[e_shstrndx]
        strtab = d[shstr["offset"]:shstr["offset"] + shstr["size"]]
        for s in self.sections:
            end = strtab.find(b"\0", s["name_off"])
            s["name"] = strtab[s["name_off"]:end].decode("utf-8", "replace")

    def symbols(self):
        """Yield (name, shndx, value, size) for every defined symbol.

        Both SHT_SYMTAB and SHT_DYNSYM are read.  A nixpkgs-stripped shared
        library keeps only .dynsym, and a static archive member keeps only
        .symtab -- reading one and not the other is how this check would go
        green over an empty set.
        """
        SHT_SYMTAB, SHT_DYNSYM = 2, 11
        seen = set()
        for sec in self.sections:
            if sec["type"] not in (SHT_SYMTAB, SHT_DYNSYM):
                continue
            strsec = self.sections[sec["link"]]
            strtab = self.data[strsec["offset"]:strsec["offset"] + strsec["size"]]
            n = sec["size"] // 24 if sec["entsize"] == 0 else sec["size"] // sec["entsize"]
            for i in range(n):
                off = sec["offset"] + i * 24
                st_name, st_info, st_other, st_shndx, st_value, st_size = \
                    struct.unpack_from(self.end + "IBBHQQ", self.data, off)
                if st_shndx == 0 or st_shndx >= len(self.sections):
                    continue  # undefined, or a reserved index (ABS/COMMON)
                end = strtab.find(b"\0", st_name)
                name = strtab[st_name:end].decode("utf-8", "replace")
                if not name:
                    continue
                key = (name, st_value, st_size)
                if key in seen:
                    continue
                seen.add(key)
                yield name, st_shndx, st_value, st_size

    def symbol_bytes(self, shndx, value, size):
        """The bytes a symbol occupies.

        ET_REL (a .o) stores st_value as an offset INTO its section; every other
        ELF type stores a virtual address.  Normalising on sh_addr covers both
        without the caller having to know which it is holding.
        """
        sec = self.sections[shndx]
        if sec["type"] == 8:  # SHT_NOBITS (.bss) has no file content
            return None
        base = value - sec["addr"] if sec["addr"] else value
        if base < 0 or base + size > sec["size"]:
            return None
        start = sec["offset"] + base
        return self.data[start:start + size]


# ---------------------------------------------------------------------------
# Metaobject extraction
# ---------------------------------------------------------------------------

# moc emits the per-class metaobject payload as
#   <Class>::qt_staticMetaObjectStaticContent<qt_meta_tag_ZN<mangled>E_t>
# which mangles to
#   _ZN8LogosAPI32qt_staticMetaObjectStaticContentIN12_GLOBAL__N_126qt_meta_tag_...
#
# The MANGLED name is matched rather than a demangled one on purpose: c++filt
# may be absent, and a demangler that renders these differently would make the
# class set silently empty rather than wrong-and-loud.
MOC_COMPONENT = "qt_staticMetaObjectStaticContent"


def meta_symbol_class(sym):
    """Fully-qualified class name for a metaobject symbol, else None.

    Walks the <length><chars> components of an Itanium nested name rather than
    pattern-matching a fixed length prefix.  An earlier version hardcoded the
    length of the moc component (it is 32, not the 36 that was assumed); the
    mismatch did not raise -- it produced plausible-looking GARBAGE class names
    like 'APIE_tEE' and a silently narrowed comparison set.
    """
    if MOC_COMPONENT not in sym or not sym.startswith("_ZN"):
        return None
    i = 3
    parts = []
    while i < len(sym) and sym[i].isdigit():
        j = i
        while j < len(sym) and sym[j].isdigit():
            j += 1
        try:
            ln = int(sym[i:j])
        except ValueError:
            return None
        name = sym[j:j + ln]
        if len(name) != ln:
            return None
        if name == MOC_COMPONENT:
            # the components collected so far are the enclosing class
            return "::".join(parts) if parts else None
        parts.append(name)
        i = j + ln
    return None


def metaobjects(path):
    """{class_name: blob_bytes} for one ELF image."""
    out = {}
    try:
        elf = Elf(path)
    except (ElfError, OSError):
        return None
    for name, shndx, value, size in elf.symbols():
        cls = meta_symbol_class(name)
        if cls is None or size == 0:
            continue
        blob = elf.symbol_bytes(shndx, value, size)
        if blob is None:
            continue
        prev = out.get(cls)
        # A class can appear more than once (symtab + dynsym, or several archive
        # members).  Identical is fine; genuinely conflicting copies inside ONE
        # image are reported by the caller as a split image.
        if prev is not None and prev != blob:
            out[cls] = b"<<CONFLICT>>"
        else:
            out[cls] = blob
    return out


def metaobjects_of_prefix(prefix):
    """{class: blob} unioned over every qt-host library under a prefix.

    Both the shared library and the static archive are read: the .so alone is
    NOT enough (LogosAPIClient and TokenManager have no metaobject symbol in
    liblogos_qt_host.so but do in liblogos_qt_host.a), and reading only the .so
    would quietly narrow the comparison to a couple of classes.
    """
    found = {}
    libs = []
    for root, _dirs, files in os.walk(prefix):
        for f in files:
            if re.match(r"^liblogos_qt_host\.(so|a|dylib)(\.|$)", f):
                libs.append(os.path.join(root, f))
    for lib in sorted(libs):
        if lib.endswith(".a"):
            with tempfile.TemporaryDirectory() as td:
                try:
                    subprocess.run(["ar", "x", os.path.abspath(lib)],
                                   cwd=td, check=True,
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
                except (subprocess.CalledProcessError, FileNotFoundError):
                    continue
                for m in sorted(os.listdir(td)):
                    mo = metaobjects(os.path.join(td, m))
                    if mo:
                        for k, v in mo.items():
                            found.setdefault(k, v)
        else:
            mo = metaobjects(lib)
            if mo:
                for k, v in mo.items():
                    found.setdefault(k, v)
    return found, libs


# ---------------------------------------------------------------------------
# Store-path scanning (routes 1, 2 and 3 -- all three are text naming a path)
# ---------------------------------------------------------------------------

QT_HOST_PATH = re.compile(rb"/nix/store/[a-z0-9]{32}-logos-qt-host[A-Za-z0-9._+-]*")
ANY_STORE_PATH = re.compile(rb"/nix/store/[a-z0-9]{32}-")


def scan_paths(root):
    """Every logos-qt-host store path named by any file under `root`.

    Returns (hits, files_read, any_store_paths).

    SYMLINKS ARE FOLLOWED AND THEIR TARGET CONTENT IS READ.  This is the whole
    reason the scan exists in this shape: logos-qt-sdk is a symlinkJoin, so
    lndir links nix-support/ in from the -lib derivation, and
    nix-support/propagated-build-inputs -- the file that names the stale
    logos-qt-host -- is a SYMLINK pointing into another store path.  `grep -r`
    does not follow it either: on the real prefix both `grep -rl` and the first
    version of this scanner reported ZERO hits while
    hglm1jn6...-logos-qt-host-0.1.0 was sitting in the target's content.  A
    scanner that reads only link TEXT reports a broken prefix as clean.

    `any_store_paths` is the liveness counter: every real nix prefix names some
    store path (its own deps, an RPATH, a propagated input).  Zero of those
    means the scan read nothing readable, which is reported as vacuous rather
    than as a pass.
    """
    hits = {}
    files_read = 0
    any_store = 0
    seen_inodes = set()
    for dirpath, _dirs, files in os.walk(root, followlinks=False):
        for f in files:
            p = os.path.join(dirpath, f)
            real = os.path.realpath(p)
            try:
                st = os.stat(real)          # stat() follows the link
            except OSError:
                continue
            if not os.path.isfile(real):
                continue
            key = (st.st_dev, st.st_ino)
            if key in seen_inodes:
                continue
            seen_inodes.add(key)
            try:
                if st.st_size > 64 * 1024 * 1024:
                    continue
                with open(real, "rb") as fh:  # open() follows the link
                    blob = fh.read()
            except OSError:
                continue
            files_read += 1
            any_store += len(ANY_STORE_PATH.findall(blob))
            for m in QT_HOST_PATH.findall(blob):
                hits.setdefault(m.decode(), set()).add(p)
            if os.path.islink(p):
                for m in QT_HOST_PATH.findall(os.readlink(p).encode()):
                    hits.setdefault(m.decode(), set()).add(p)
    return hits, files_read, any_store


def store_prefix(path):
    m = re.match(r"(/nix/store/[a-z0-9]{32}-[^/]*)", path)
    return m.group(1) if m else path


# ---------------------------------------------------------------------------
# Report helpers
# ---------------------------------------------------------------------------

def blob_strings(blob):
    """The printable strings inside a metaobject blob -- these are the method,
    signal, property and parameter NAMES.  Used only to make a failure name the
    API that is missing rather than just a byte length."""
    if blob is None or blob == b"<<CONFLICT>>":
        return []
    out = []
    for chunk in re.findall(rb"[\x20-\x7e]{3,}", blob):
        out.extend(x for x in chunk.split(b"\0") if x)
    return [x.decode("ascii", "replace") for x in out]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--declared", required=True,
                    help="the logos-qt-host prefix this consumer DECLARES "
                         "(its LOGOS_QT_HOST_ROOT)")
    ap.add_argument("--image", action="append", default=[],
                    help="a built image to check (repeatable)")
    ap.add_argument("--image-dir", action="append", default=[],
                    help="a directory to search for images (repeatable)")
    ap.add_argument("--scan", action="append", default=[],
                    help="a prefix to scan for foreign qt-host store paths, "
                         "e.g. the logos-qt-sdk prefix (repeatable)")
    ap.add_argument("--closure-file", default=None,
                    help="a file listing the subject's closure, one store path "
                         "per line (pkgs.closureInfo's store-paths). Enables "
                         "layer 0.")
    ap.add_argument("--allow-classes", default="",
                    help="comma-separated classes to compare; default is every "
                         "class the declared qt-host defines")
    args = ap.parse_args()

    fail = []      # identity violations
    vacuous = []   # "we looked at nothing" -- reported separately, exit 2
    print("=" * 72)
    print("qt-host-identity: the logos-qt-host a consumer LINKS must be the one")
    print("                  in that consumer's own closure")
    print("=" * 72)

    declared = os.path.realpath(args.declared)
    print(f"\ndeclared logos-qt-host:\n  {store_prefix(args.declared)}")

    # -- anti-vacuity 1: the reference must exist and define metaobjects ------
    if not os.path.isdir(declared):
        print(f"\nFAIL(vacuous): declared qt-host prefix does not exist: {declared}")
        return 2
    ref, ref_libs = metaobjects_of_prefix(declared)
    print(f"  libraries read: {len(ref_libs)}")
    for l in ref_libs:
        print(f"    {l[len(declared):].lstrip('/')}")
    print(f"  Q_OBJECT classes with a metaobject: {len(ref)}")
    if not ref_libs:
        vacuous.append("the declared qt-host prefix contains no "
                       "liblogos_qt_host.{so,a,dylib}; nothing to compare "
                       "against")
    elif not ref:
        vacuous.append("the declared qt-host defines ZERO metaobject blobs. "
                       "Either the symbol naming changed (moc / Qt upgrade) or "
                       "the libraries were stripped of section headers -- "
                       "either way every comparison below would be vacuous")
    if vacuous:
        print()
        for v in vacuous:
            print(f"FAIL(vacuous): {v}")
        return 2

    only = [c.strip() for c in args.allow_classes.split(",") if c.strip()]
    if only:
        ref = {k: v for k, v in ref.items() if k in only}
    print("  classes: " + ", ".join(sorted(ref)))

    # -- layer 0: how many logos-qt-host are in the closure at all? ----------
    # Catches DUPLICATION: two qt-hosts in one build, even when their Qt API is
    # identical.  Layer 2 cannot see that case (measured: a tree with two
    # qt-hosts whose metaobjects match byte-for-byte), and layer 0 cannot see
    # STALENESS (measured: the tree where currentCallerJson is invisible
    # fleet-wide has exactly ONE qt-host in its closure).  Neither layer is the
    # guard on its own.
    closure_hosts = []
    if args.closure_file:
        print("\n--- layer 0: logos-qt-host prefixes in the subject's closure ---")
        try:
            with open(args.closure_file) as fh:
                paths = [l.strip() for l in fh if l.strip()]
        except OSError as e:
            print(f"FAIL(vacuous): cannot read closure file: {e}")
            return 2
        if not paths:
            vacuous.append(f"the closure file {args.closure_file} is empty; a "
                           f"'one qt-host' verdict over an empty closure is "
                           f"meaningless")
        closure_hosts = sorted({p for p in paths
                                if re.search(r"-logos-qt-host-", p)})
        print(f"  closure size: {len(paths)} store path(s)")
        for h in closure_hosts:
            mark = "declared" if os.path.realpath(h) == declared else "FOREIGN"
            print(f"    {h}  [{mark}]")
        if not closure_hosts:
            print("    (none -- the qt-host is linked statically, so it leaves "
                  "no store reference; layer 2 is what covers that)")
        foreign = [h for h in closure_hosts
                   if os.path.realpath(h) != declared]
        if foreign:
            fail.append(
                "the subject's closure carries " + str(len(closure_hosts)) +
                " logos-qt-host prefix(es), " + str(len(foreign)) +
                " of them foreign:\n" +
                "".join(f"        {h}   [{'declared' if os.path.realpath(h) == declared else 'FOREIGN'}]\n"
                        for h in closure_hosts) +
                "      This consumer declares\n"
                f"        {store_prefix(args.declared)}\n"
                "      Exactly one logos-qt-host is allowed in a closure. Two\n"
                "      copies of the Qt host runtime in one process means two\n"
                "      TokenManager generations, hence two token stores, hence\n"
                "      'rejecting unauthorized call' at runtime. Find the edge:\n" +
                "".join(f"        nix why-depends <subject> {h}\n" for h in foreign))

    # -- layer 1: store paths named by scanned prefixes -----------------------
    # Catches the propagated build input, the baked CMake HINTS and the
    # forwarding headers in one pass, because all three are TEXT naming a store
    # path.  This is the layer that names two store paths.
    if args.scan:
        print("\n--- layer 1: qt-host store paths named by consumed prefixes ---")
    for s in args.scan:
        s = os.path.realpath(s)
        if not os.path.isdir(s):
            vacuous.append(f"scan prefix does not exist: {s}")
            continue
        hits, files_read, any_store = scan_paths(s)
        label = store_prefix(s)
        # Liveness: a "names no qt-host" verdict is a PASS, so it must be
        # distinguishable from "read nothing".
        if files_read == 0 or any_store == 0:
            vacuous.append(
                f"scanning {label} read {files_read} file(s) and found "
                f"{any_store} store-path reference(s) of ANY kind. A real nix "
                f"prefix always names some store path, so this scan saw "
                f"nothing -- its 'no qt-host here' verdict would be worthless")
            continue
        if not hits:
            print(f"  {label}\n      names no logos-qt-host store path (good) "
                  f"[read {files_read} file(s), {any_store} store refs]")
            continue
        for path, where in sorted(hits.items()):
            same = os.path.realpath(path) == declared
            verdict = "OK" if same else "MISMATCH"
            print(f"  {label}\n      -> {path}  [{verdict}]")
            for w in sorted(where)[:6]:
                print(f"         named in: {w[len(s):].lstrip('/')}")
            if len(where) > 6:
                print(f"         ... and {len(where) - 6} more file(s)")
            if not same:
                fail.append(
                    f"{label}\n"
                    f"      exports logos-qt-host {path}\n"
                    f"      but this consumer declares {store_prefix(args.declared)}\n"
                    f"      ({len(where)} file(s) in that prefix name it)")

    # -- collect images ------------------------------------------------------
    images = list(args.image)
    for d in args.image_dir:
        if not os.path.isdir(d):
            continue
        for root, _dirs, files in os.walk(d):
            for f in files:
                p = os.path.join(root, f)
                if os.path.islink(p):
                    p = os.path.realpath(p)
                if not os.path.isfile(p):
                    continue
                try:
                    with open(p, "rb") as fh:
                        if fh.read(4) != b"\x7fELF":
                            continue
                except OSError:
                    continue
                images.append(p)
    images = sorted(set(images))

    # -- layer 2 + 3: what each image actually carries ------------------------
    print(f"\n--- layer 2: metaobject identity, per image ({len(images)} ELF image(s)) ---")
    compared = 0
    carriers = 0
    for img in images:
        got = metaobjects(img)
        if not got:
            continue
        shared = {c: b for c, b in got.items() if c in ref}
        if not shared:
            continue
        carriers += 1
        bad = {c: b for c, b in shared.items() if b != ref[c]}
        compared += len(shared)
        short = img
        for pre in [os.path.realpath(x) for x in args.image_dir]:
            if short.startswith(pre):
                short = short[len(pre):].lstrip("/")
        tag = "OK" if not bad else "MISMATCH"
        print(f"  [{tag}] {store_prefix(img)}")
        if img != store_prefix(img):
            print(f"         {img[len(store_prefix(img)):].lstrip('/')}")
        print(f"         qt-host classes carried: {len(shared)}/{len(ref)}")
        for c in sorted(bad):
            want, have = ref[c], bad[c]
            ws, hs = set(blob_strings(want)), set(blob_strings(have))
            missing = sorted(ws - hs)
            extra = sorted(hs - ws)
            print(f"         class {c}: metaobject differs "
                  f"(declared {len(want)} bytes, linked {len(have)} bytes)")
            if missing:
                print(f"           MISSING from the linked image: {', '.join(missing)}")
            if extra:
                print(f"           present only in the linked image: {', '.join(extra)}")
            fail.append(
                f"{store_prefix(img)}\n"
                f"      {img[len(store_prefix(img)):].lstrip('/')}\n"
                f"      links a DIFFERENT logos-qt-host than this consumer declares.\n"
                f"      {c}'s metaobject is {len(have)} bytes here and "
                f"{len(want)} bytes in\n"
                f"      {store_prefix(args.declared)}"
                + (f"\n      Qt-facing API invisible to this image: "
                   f"{', '.join(missing)}" if missing else ""))

    # -- anti-vacuity 2: a PASS must mean "compared something" ---------------
    if not images and not closure_hosts and not args.scan:
        vacuous.append("nothing was examined at all: no image, no closure, no "
                       "scan prefix. This run asserted nothing")
    if not images:
        vacuous.append("no ELF image was examined. A pass here would mean the "
                       "image search found nothing, not that the images agree")
    elif carriers == 0:
        vacuous.append(
            f"none of the {len(images)} image(s) examined carries ANY of the "
            f"declared qt-host's {len(ref)} Q_OBJECT classes. Either the images "
            f"do not link the Qt host runtime at all (wrong subjects) or the "
            f"metaobject symbols are no longer being found -- both make every "
            f"comparison above vacuous")
    elif compared == 0:
        vacuous.append("zero metaobject blobs were compared")

    print("\n" + "=" * 72)
    if vacuous:
        for v in vacuous:
            print(f"FAIL (vacuous): {v}")
        print("=" * 72)
        return 2
    if fail:
        print(f"FAIL: {len(fail)} logos-qt-host identity violation(s)\n")
        for i, f in enumerate(fail, 1):
            print(f"  {i}. {f}\n")
        print("  The logos-qt-host a consumer links MUST be the one in that")
        print("  consumer's own closure. Two of them in one build means the")
        print("  images disagree about LogosAPI's metaobject, so a method added")
        print("  to the Qt host runtime is invisible to whichever image linked")
        print("  the older copy -- QMetaObject::invokeMethod fails at RUNTIME")
        print("  with 'No such method', and the build stays green.")
        print("=" * 72)
        return 1
    print(f"PASS: {carriers} image(s) carry the Qt host runtime; "
          f"{compared} metaobject blob(s) compared; all identical to")
    print(f"      {store_prefix(args.declared)}")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
