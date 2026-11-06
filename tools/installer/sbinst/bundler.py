"""The bundle builder (docs/INSTALLER.md 12):

  starbite-install --build-bundle DIR --from-phone [--card-layout sd64] [--only KEY] [--skip KEY]
      the main phone in the installer environment (or, legacy, TWRP): every item is READ from it (never
      written) and streamed into DIR,
      hashed here and on the phone in the same pass (one read), then installer-manifest.toml.
  starbite-install --build-bundle DIR --from-local [--sources FILE] ...
      only local build outputs (SOURCES' `local` candidates, or a TOML file `key = "path"`).

DIR may be anywhere: an external disk, a USB stick (exFAT or APFS; FAT32 cannot hold the 4+ GiB
items and is refused before anything is copied), or iCloud Drive (~/Library/Mobile Documents/
com~apple~CloudDocs/...). Items are streamed straight into DIR (zero runs become holes where the
file system has them), local sources on the same volume are hard-linked instead of copied, and
.gz sources are decompressed on the fly. Every item is checked against the free space first; an
interrupted build keeps every finished item (DIR/.build-state.json) and a re-run goes on.

Never in a bundle (per phone or the user's own data): EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE,
PERSISTENT, STEADY, BOTA*, DTB/DTBO, MISC, RADIO, OMR, CACHE, HIDDEN, CP_DEBUG, NAD_REFER, DQMDBG,
the raw LUNs, A16DATA (Android's /data: a fresh empty one comes from make-a16data.sh), ODM
(/metadata: made fresh on the phone), and the OS stores' settings block (block 2: default entry,
last boot; zeroed, the phone keeps its own). `read_plan()` refuses any phone source outside the
allowlist, whatever SOURCES says."""
import gzip
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import manifest as mf, tomlmini, ufslayout
from .common import (GIB, MIB, PROTECTED, PROTECTED_RAW, REPO, TOOLS, Refused, Stopped, cardlayout,
                     default_state_dir, human)
from .device import Phone

# Partitions the builder may read from the phone (the rest is per phone, or the user's own data).
READABLE = ('BOOT', 'SYSTEM', 'VENDOR', 'WINESP', 'WINDOWS', 'USERDATA')
NEVER = PROTECTED + ('MISC', 'RADIO', 'OMR', 'CACHE', 'HIDDEN', 'CP_DEBUG', 'NAD_REFER', 'DQMDBG', 'A16DATA',
                     'ODM') + tuple(x.upper() for x in PROTECTED_RAW)
SLOT_BYTES = 16 * GIB
STORE_UFS_OFFSET = 16 * GIB
SETTINGS = (8192, 12288)               # store block 2: the phone's own settings record


@dataclass
class Src:
    key: str
    title: str
    os: str
    dest: str                          # manifest destination (card roots: card:root:KEY)
    file: str                          # path inside the bundle
    phone: tuple = None                # ('part', NAME) | ('slot',) | ('store', 'ufs'|'sd') | ('card-root', KEY)
                                       # | ('card-file', PATH) | None (never from the phone)
    local: tuple = ()                  # candidate paths (relative to the local root; globs)
    data: bool = False
    optional: bool = False
    note: str = ''
    action: str = 'write'


SOURCES = (
    Src('boot', 'Starbite loader + menu (BOOT)', 'starbite', 'part:BOOT', 'boot/starbite-boot.img', ('part', 'BOOT'),
        ('out/xnu-boot-*.img', 'build/boot/starbite-boot.img')),
    Src('ufs-store', 'OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system', 'starbite', 'store:ufs',
        'store/ufs-store.img', ('store', 'ufs'), ('build/store/ufs-store.img',)),
    Src('sd-store', 'OS store (microSD p2)', 'starbite', 'store:sd', 'store/sd-store.img', ('store', 'sd'),
        ('build/store/sd-store.img',), optional=True),
    Src('ios16-slot', 'iOS 16.5 (an earlier port) root: APFS system + HFSX /var', 'ios', 'slot:USERDATA@0', 'apple/ios16-slot.img',
        ('slot',), ('build/refport/ios16-slot.img',), data=True,
        note='from the main phone: its iOS /var (settings, logs) comes along; a clean slot needs the an earlier port build'),
    Src('android-system', 'Android system', 'android', 'part:SYSTEM', 'android/system.img', ('part', 'SYSTEM'),
        ('build/android/pixel/system.img', 'build/android/lineage/system.img')),
    Src('android-vendor', 'Android vendor (Starbite build)', 'android', 'part:VENDOR', 'android/vendor.img',
        ('part', 'VENDOR'), ('build/android/pixel/vendor.img', 'build/android/pixel/vendor-pixel3.img.gz')),
    Src('android-data', 'Android /data (empty f2fs)', 'android', 'part:A16DATA', 'android/a16data.img', None,
        ('build/android/pixel/a16data-8g.img', 'build/android/pixel/a16data-8g.img.gz'), data=True,
        note='never the main phone\'s: tools/android/pixel/make-a16data.sh (empty f2fs)'),
    Src('android-metadata', 'Android /metadata (fresh ext4)', 'android', 'part:ODM', '', None, (), action='mkfs-metadata'),
    Src('winesp', 'Windows 11 boot files (ESP)', 'windows', 'part:WINESP', 'windows/winesp.img', ('part', 'WINESP'),
        ('build/woa/winesp.img',), data=True, optional=True),
    Src('windows', 'Windows 11 (NTFS)', 'windows', 'part:WINDOWS', 'windows/windows.img', ('part', 'WINDOWS'),
        ('build/woa/windows.img',), data=True, optional=True,
        note='from the main phone: its Windows user profile comes along'),
    Src('mac13-root', 'macOS Ventura root (HFSX, 15 GiB)', 'macos', 'card:root:mac13', 'apple/mac13-root.img',
        ('card-root', 'mac13'), ('build/macos13/mac13-root.img',), data=True),
    Src('ios26-root', 'iOS 26 root (parked edge pair)', 'ios', 'card:root:ios26', 'apple/ios26-root.img',
        ('card-root', 'ios26'), (), data=True, optional=True),
    Src('env-kernel', 'Installer environment: Linux 7.1 kernel (arm64 Image)', 'starbite', 'env:kernel',
        'env/installer.image', None, ('build/installer-env/installer.image',)),
    Src('env-initrd', 'Installer environment: initramfs', 'starbite', 'env:initrd', 'env/installer.initrd', None,
        ('build/installer-env/installer.initrd',)),
    Src('env-dtb', 'Installer environment: device tree', 'starbite', 'env:dtb', 'env/installer.dtb', None,
        ('build/installer-env/installer.dtb',)),
    Src('pmos', 'postmarketOS (Phosh)', 'linux', 'card:file:/distros/pmos.img', 'card/distros/pmos.img',
        ('card-file', '/distros/pmos.img'), ('build/distros/pmos.img',), data=True),
    Src('ubuntu', 'Ubuntu (Phosh)', 'linux', 'card:file:/distros/ubuntu.img', 'card/distros/ubuntu.img',
        ('card-file', '/distros/ubuntu.img'), ('build/distros/ubuntu.img',), data=True),
    Src('chromeos', 'ChromiumOS (GPT disk image)', 'chromeos', 'card:file:/distros/chromeos.img',
        'card/distros/chromeos.img', ('card-file', '/distros/chromeos.img'), ('build/distros/chromeos.img',), data=True),
)


def check_source(s):
    """The allowlist: a phone source may only be a READABLE partition, the Apple slot, a store, a
    card root slot or a card file; never anything in NEVER."""
    if s.phone is None:
        return
    kind = s.phone[0]
    if kind == 'part' and (s.phone[1] not in READABLE or s.phone[1] in NEVER):
        raise Refused(f'{s.key}: the builder never reads {s.phone[1]} from a phone (per-phone or user data)')
    if kind not in ('part', 'slot', 'store', 'card-root', 'card-file'):
        raise Refused(f'{s.key}: unknown phone source {s.phone}')


@dataclass
class Read:
    """One item read from the phone: the node, byte offset and length, and how to fix it up."""
    src: Src
    node: str
    offset: int
    size: int
    zero: tuple = ()                   # byte range zeroed in the bundle copy (the store settings block)
    file_on_card: str = ''


@dataclass
class State:
    path: Path
    d: dict = field(default_factory=dict)

    @classmethod
    def load(cls, p):
        s = cls(p)
        if p.is_file():
            s.d = json.loads(p.read_text())
        return s

    def save(self):
        tmp = self.path.with_suffix('.tmp')
        tmp.write_text(json.dumps(self.d, indent=1, sort_keys=True))
        os.replace(tmp, self.path)


# ------------------------------------------------------------------------------------- destination

def fs_type(path):
    """The file system type of the volume holding `path` (msdos/exfat/apfs/hfs on macOS; vfat/exfat/
    ext4... on Linux), or ''."""
    p = Path(path)
    while not p.exists():
        p = p.parent
    if platform.system() == 'Darwin':
        out = subprocess.run(['df', str(p)], capture_output=True, text=True).stdout.splitlines()
        dev = out[-1].split()[0] if len(out) > 1 else ''
        mounts = subprocess.run(['mount'], capture_output=True, text=True).stdout
        m = re.search(re.escape(dev) + r' on .*? \((\w+)', mounts)
        return m.group(1) if m else ''
    best, typ = '', ''
    try:
        for ln in open('/proc/mounts'):
            _, mp, t = ln.split()[:3]
            if (str(p.resolve()) + '/').startswith(mp.rstrip('/') + '/') and len(mp) > len(best):
                best, typ = mp, t
    except OSError:
        pass
    return typ


def in_icloud(path):
    return 'Mobile Documents/com~apple~CloudDocs' in str(Path(path).resolve())


def free_bytes(path):
    p = Path(path)
    while not p.exists():
        p = p.parent
    return shutil.disk_usage(p).free


def wait_for_space(ui, out, need, done_files, minutes=60):
    """iCloud Drive keeps a local copy of every file until it is uploaded. Ask iCloud to evict the
    finished items (`brctl evict`, works once uploaded) and wait for the space; give up after
    `minutes` (STOPPED: a re-run goes on)."""
    t0 = time.time()
    while free_bytes(out) < need:
        if not in_icloud(out) or time.time() - t0 > minutes * 60:
            raise Stopped(f'{human(free_bytes(out))} free at {out}, {human(need)} needed for the next item: free space '
                          '(or use the USB stick / an external disk) and run again; finished items are kept')
        for f in done_files:
            subprocess.run(['brctl', 'evict', str(f)], capture_output=True)
        ui.info(f'iCloud Drive: {human(free_bytes(out))} free, {human(need)} needed: waiting for uploads to finish '
                'so finished items can be evicted...')
        time.sleep(30)


# ------------------------------------------------------------------------------------- streaming

def write_stream(chunks, dest, size, zero=(), progress=None):
    """Write an iterator of byte chunks to dest (sparse where 1 MiB runs are zero); returns
    (sha256 of the bytes as they came, sha256 of the file as written: with `zero` zeroed)."""
    raw, out = hashlib.sha256(), hashlib.sha256()
    pos = 0
    dest.parent.mkdir(parents=True, exist_ok=True)
    zblk = bytes(MIB)
    t0 = time.time()
    with open(dest, 'wb') as f:
        buf = b''
        for c in chunks:
            raw.update(c)
            if zero and pos < zero[1] and pos + len(c) > zero[0]:
                b = bytearray(c)
                a, e = max(zero[0] - pos, 0), min(zero[1] - pos, len(c))
                b[a:e] = bytes(e - a)
                c = bytes(b)
            out.update(c)
            buf += c
            while len(buf) >= MIB:
                blk, buf = buf[:MIB], buf[MIB:]
                if blk == zblk:
                    f.seek(MIB, 1)
                else:
                    f.write(blk)
            pos += len(c)
            if progress and pos % (256 * MIB) < len(c):
                progress(pos, size, time.time() - t0)
        f.write(buf)
        f.truncate(pos)
    if pos != size:
        raise Stopped(f'{dest.name}: {pos} bytes arrived, {size} expected')
    return raw.hexdigest(), out.hexdigest()


def adb_stream(cmd, bufsize=MIB):
    """The stdout of `adb exec-out CMD` as chunks (no staging file)."""
    p = subprocess.Popen(['adb', 'exec-out', cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        while True:
            c = p.stdout.read(bufsize)
            if not c:
                break
            yield c
    finally:
        p.stdout.close()
        rc = p.wait()
        if rc:
            raise Stopped(f'adb exec-out failed ({rc}): {p.stderr.read().decode(errors="replace")[-300:]}')


def file_stream(path, bufsize=8 * MIB):
    op = gzip.open if str(path).endswith('.gz') else open
    with op(path, 'rb') as f:
        while True:
            c = f.read(bufsize)
            if not c:
                return
            yield c


def gz_size(path):
    with gzip.open(path, 'rb') as f:
        n = 0
        while True:
            c = f.read(16 * MIB)
            if not c:
                return n
            n += len(c)


# ------------------------------------------------------------------------------------- the phone side

def read_plan(phone, facts, sources, ui):
    """[Read] for each source, from the phone's facts (read only)."""
    reads = []
    mbr = phone.exec_out('dd if=/dev/block/mmcblk0 bs=512 count=1 2>/dev/null')
    from .steps import mbr_parts
    parts = mbr_parts(mbr) or []
    card_name = cardlayout.identify(parts, facts.card or 0) if facts.card else None
    for s in sources:
        check_source(s)
        k = s.phone
        if k is None:
            continue
        if k[0] == 'part':
            if k[1] not in facts.parts:
                if s.optional:
                    ui.info(f'{s.key}: no {k[1]} on the phone, left out')
                    continue
                raise Refused(f'{s.key}: the phone has no {k[1]}')
            reads.append(Read(s, facts.node(k[1]), 0, facts.size(k[1])))
        elif k[0] == 'slot':
            reads.append(Read(s, facts.node('USERDATA'), 0, SLOT_BYTES))
        elif k[0] == 'store':
            node, off = (facts.node('USERDATA'), STORE_UFS_OFFSET) if k[1] == 'ufs' else ('/dev/block/mmcblk0p2', 0)
            hdr = phone.exec_out(f'dd if={node} bs=4096 skip={off // 4096 + 1} count=1 2>/dev/null')
            if hdr[:8] != b'SBSTORE2':
                if s.optional:
                    ui.info(f'{s.key}: no store on the phone, left out')
                    continue
                raise Refused(f'{s.key}: no version 2 store at {node}+{off}')
            used = int.from_bytes(hdr[24:32], 'little')
            reads.append(Read(s, node, off, used * 4096, zero=SETTINGS))
        elif k[0] == 'card-root':
            L = cardlayout.LAYOUTS.get(card_name)
            r = L.root(k[1]) if L is not None else None
            if r is None:
                if s.optional:
                    ui.info(f'{s.key}: the card ({card_name or "unknown layout"}) has no {k[1]} root, left out')
                    continue
                raise Refused(f'{s.key}: the phone\'s card ({card_name or "unknown layout"}) has no {k[1]} root slot')
            reads.append(Read(s, '/dev/block/mmcblk0', r.start * 512, r.sectors * 512))
        elif k[0] == 'card-file':
            path = '/external_sd' + k[1]
            n = phone.sh(f'[ -f {path} ] && wc -c < {path}', check=False).strip()
            if not n.isdigit():
                if s.optional:
                    continue
                raise Refused(f'{s.key}: no {path} on the phone\'s card')
            reads.append(Read(s, path, 0, int(n), file_on_card=path))
    return reads, card_name


def phone_cmd(r):
    if r.file_on_card:
        return f'cat {r.file_on_card}'
    if r.offset % MIB == 0 and r.size % MIB == 0:
        return f'dd if={r.node} bs=1048576 skip={r.offset // MIB} count={r.size // MIB} 2>/dev/null'
    return (f'dd if={r.node} bs=4096 skip={r.offset // 4096} count={-(-r.size // 4096)} 2>/dev/null | head -c {r.size}')


def stream_from_phone(phone, r, dest, progress):
    """One read on the phone: the bytes go to this computer and through sha256sum on the phone at
    the same time (tee into a FIFO); both hashes must agree."""
    tag = '/tmp/sbb-' + r.src.key
    cmd = (f'rm -f {tag}.fifo {tag}.sha; mkfifo {tag}.fifo && (sha256sum < {tag}.fifo > {tag}.sha.tmp; '
           f'mv {tag}.sha.tmp {tag}.sha) & {phone_cmd(r)} | tee {tag}.fifo')
    raw, out = write_stream(adb_stream(cmd), dest, r.size, r.zero, progress)
    there = ''
    for _ in range(40):
        there = (phone.sh(f'cat {tag}.sha 2>/dev/null', check=False).split() or [''])[0]
        if there:
            break
        time.sleep(0.25)
    phone.sh(f'rm -f {tag}.fifo {tag}.sha', check=False)
    if not there:                                        # no FIFO on this shell: a second read
        there = (phone.sh(f'{phone_cmd(r)} | sha256sum').split() or [''])[0]
    if there != raw:
        dest.unlink(missing_ok=True)
        raise Stopped(f'{r.src.key}: the phone says {there[:16]}..., this computer got {raw[:16]}...: item dropped; '
                      'run again')
    return out


# ------------------------------------------------------------------------------------- stores (card layouts)

def _sbstore2():
    import importlib.util
    spec = importlib.util.spec_from_file_location('sbstore2_b', TOOLS / 'store' / 'sbstore2.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def retarget_store(img, out, medium, layout, catalog_toml):
    """A store image rebuilt for another card layout: the catalog from catalog_toml (already
    retargeted), every SWEETXNU payload of an xnu entry with a card root moved to that root's slot
    on `layout`, payloads of entries without a slot left out (sbstore2.py build --import)."""
    sb = _sbstore2()
    entries = tomlmini.loads(Path(catalog_toml).read_text()).get('entry', [])
    blobs = {n: b for n, _, b in sb.read_store(img)}
    L = cardlayout.get(layout)
    args, omit = [], []
    with tempfile.TemporaryDirectory() as t:
        for e in entries:
            if e.get('method') != 'xnu' or not e.get('files'):
                continue
            for alt in (a.split(':', 1)[-1] for a in e['files'][0].split('|')):
                b = blobs.get(alt)
                if b is None or cardlayout.payload_cmdline(b) is None or cardlayout.sdroot_of(cardlayout.payload_cmdline(b)) is None:
                    continue
                if L.root(e['key']) is None:
                    omit.append(alt)
                    continue
                p = Path(t) / alt
                p.write_bytes(cardlayout.retarget_payload(b, layout, e['key']))
                args.append(f'{alt}={p}')
        cmd = [sys.executable, '-I', str(TOOLS / 'store' / 'sbstore2.py'), 'build', str(out), '--medium', medium,
               '--import', str(img)]
        if medium == 'ufs':
            cmd += ['--catalog', str(catalog_toml)]
        for o in sorted(set(omit)):
            if o in blobs:
                cmd += ['--omit', o]
        r = subprocess.run(cmd + args, capture_output=True, text=True)
        if r.returncode:
            raise Stopped(f'sbstore2.py build ({medium}, {layout}) failed: {(r.stderr or r.stdout)[-400:]}')


# ------------------------------------------------------------------------------------- manifest

def manifest_text(name, items, card_layout, ufs_layout):
    L = []
    L.append('# installer-manifest.toml written by starbite-install --build-bundle (docs/INSTALLER.md 12)')
    L.append(f'# {time.strftime("%Y-%m-%d %H:%M:%S")}. Personal bundle: never share it (Apple, Google and Microsoft')
    L.append('# images are licensed per device). Per-phone areas are never in it (sbinst/bundler.py NEVER).')
    L.append('format = 1')
    L.append(f'name = "{name}"')
    L.append('catalog = "store/catalog.toml"')
    L.append('')
    L.append('[phone]\nmodels = ["SM-G960F"]\ndevices = ["starlte"]\nmin_battery = 50')
    L.append('# no ufs_blocks: the [ufs] layout fits any SM-G960F UFS (its last row fills the disk)')
    L.append('')
    L.append(f'[ufs]\nlayout = "{ufs_layout}"')
    L.append('')
    L.append('[env]\ncmdline = "sb.installer=1"')
    L.append('')
    L.append(f'[card]\nmin_bytes = 60000000000\nlayout = "{card_layout}"')
    L.append('')
    for it in items:
        L.append('[[item]]')
        for k in ('key', 'title', 'os'):
            L.append(f'{k} = "{it[k]}"')
        if it.get('file'):
            L.append(f'file = "{it["file"]}"\nsize = {it["size"]}\nsha256 = "{it["sha256"]}"')
        L.append(f'dest = "{it["dest"]}"')
        if it.get('action', 'write') != 'write':
            L.append(f'action = "{it["action"]}"')
        if it.get('data'):
            L.append('data = true')
        if it.get('optional'):
            L.append('optional = true')
        if it.get('note'):
            L.append(f'note = "{it["note"]}"')
        L.append('')
    return '\n'.join(L)


# ------------------------------------------------------------------------------------- build

def local_root():
    """Where local build outputs are: SBINST_LOCAL_ROOT, else this checkout, else (a git worktree under
    .claude/worktrees/) the main checkout."""
    if os.environ.get('SBINST_LOCAL_ROOT'):
        return Path(os.environ['SBINST_LOCAL_ROOT'])
    if (REPO / 'build').is_dir() or REPO.parent.name != 'worktrees':
        return REPO
    return REPO.parents[2]


def find_local(s, overrides, root):
    if s.key in overrides:
        p = Path(os.path.expanduser(overrides[s.key]))
        return p if p.is_file() else None
    for pat in s.local:
        hits = sorted(root.glob(pat), key=lambda x: x.stat().st_mtime, reverse=True)
        if hits:
            return hits[0]
    return None


def build(ui, runner, out, from_phone, card_layout=None, only=(), skip=(), state_dir=None, sources_file=None):
    out = Path(os.path.expanduser(str(out))).resolve()
    if out.exists() and not out.is_dir():
        raise Refused(f'{out} is not a directory')
    if str(out).startswith(str(REPO.resolve()) + '/tools'):
        raise Refused('a bundle does not go into tools/')
    sel = [s for s in SOURCES if (not only or s.key in only) and s.key not in skip]
    for k in list(only) + list(skip):
        if k not in {s.key for s in SOURCES}:
            raise Refused(f'--only/--skip {k}: no such item ({", ".join(s.key for s in SOURCES)})')
    for s in sel:
        check_source(s)
    out.mkdir(parents=True, exist_ok=True)
    st = State.load(out / '.build-state.json')
    fs = fs_type(out)
    ui.h1(f'Building a bundle in {out}')
    ui.info(f'file system: {fs or "?"}' + ('; iCloud Drive (local copies until uploaded)' if in_icloud(out) else '') +
            f'; free {human(free_bytes(out))}')
    big_limit = (4 * GIB - 1) if fs.lower() in ('msdos', 'vfat', 'fat32', 'fat') else None
    items, done_files = [], []
    overrides = {}
    if sources_file:
        overrides = {k: v for k, v in tomlmini.loads(Path(sources_file).read_text()).items() if isinstance(v, str)}
    root = local_root()
    src_layout = 'sd128'
    reads = {}
    if from_phone:
        from .device import InstallerEnv
        phone = Phone(runner, InstallerEnv(runner, default_state_dir() / 'env'))
        dev = phone.detect()
        if dev.kind == 'env':
            phone.env.activate()
        if dev.kind not in ('recovery', 'env'):
            raise Refused(f'--from-phone needs the main phone in the installer environment (or TWRP, legacy) (found: '
                          f'{dev.kind}); nothing is written to it')
        facts = phone.facts()
        rl, src_layout = read_plan(phone, facts, sel, ui)
        reads = {r.src.key: r for r in rl}
        src_layout = src_layout or 'sd128'
        ui.ok(f'phone: {facts.props.get("ro.product.model")}, card layout {src_layout}; {len(rl)} items to read '
              f'({human(sum(r.size for r in rl))})')
    target = card_layout or src_layout
    cardlayout.get(target)
    (out / 'store').mkdir(parents=True, exist_ok=True)
    cat = out / 'store' / 'catalog.toml'
    cat.write_text(cardlayout.catalog_text((TOOLS / 'store' / 'catalog.toml').read_text(), target))
    for s in sel:
        rec = {'key': s.key, 'title': s.title, 'os': s.os, 'dest': s.dest, 'data': s.data, 'optional': s.optional,
               'note': s.note, 'action': s.action, 'file': s.file}
        if s.action != 'write':
            items.append(rec)
            continue
        if s.dest.startswith('card:root:') and cardlayout.get(target).root(s.dest.split(':')[2]) is None:
            ui.info(f'{s.key}: the {target} card layout has no slot for it: left out')
            continue
        dest = out / s.file
        prev = st.d.get(s.key)
        if prev and dest.is_file() and dest.stat().st_size == prev['size'] and prev.get('layout') == target:
            ui.ok(f'{s.key}: done earlier ({human(prev["size"])}, sha256 {prev["sha256"][:12]}...)')
            items.append(dict(rec, size=prev['size'], sha256=prev['sha256']))
            done_files.append(dest)
            continue
        prog = (lambda d, t, dt, k=s.key: ui.info(f'{k}: {human(d)} of {human(t)} ({d / max(dt, 1) / MIB:.0f} MiB/s)'))
        if from_phone:
            r = reads.get(s.key)
            if r is None:
                if s.phone is not None and not s.optional:
                    ui.warn(f'{s.key}: not read from the phone')
                lp = find_local(s, overrides, root)
                if lp is None:
                    (ui.info if s.optional else ui.warn)(f'{s.key}: no local source either ({", ".join(s.local) or "-"})'
                                                         f'{"; left out (optional)" if s.optional else "; the bundle will lack it"}')
                    continue
                size, sha = _local(ui, s, lp, dest, big_limit, done_files, prog)
            else:
                if big_limit and r.size > big_limit:
                    raise Refused(f'{s.key} is {human(r.size)}: {out} is FAT32, which holds files up to 4 GiB. Use an '
                                  'exFAT or APFS volume (the USB stick reformatted exFAT, or an external disk)')
                wait_for_space(ui, out, r.size + 512 * MIB, done_files)
                ui.info(f'{s.key}: reading {human(r.size)} from {r.node}' + (f' +{r.offset}' if r.offset else ''))
                tmp = dest.with_name(dest.name + '.part')
                sha = stream_from_phone(phone, r, tmp, prog)
                size = r.size
                if s.phone[0] == 'store' and target != src_layout:
                    medium = 'ufs' if s.phone[1] == 'ufs' else ('sd' if target == 'sd128' else target)
                    retarget_store(tmp, dest, medium, target, cat)
                    tmp.unlink()
                    size, sha = dest.stat().st_size, _sha(dest)
                    ui.ok(f'{s.key}: rebuilt for the {target} card layout (payload sdroot=, catalog)')
                else:
                    os.replace(tmp, dest)
                ui.ok(f'{s.key}: {human(size)}, sha256 {sha[:12]}... (phone and computer agree)')
        else:
            lp = find_local(s, overrides, root)
            if lp is None:
                (ui.info if s.optional else ui.warn)(f'{s.key}: no local source ({", ".join(s.local) or "-"}); '
                                                     f'{"left out (optional)" if s.optional else "the bundle will lack it"}')
                continue
            size, sha = _local(ui, s, lp, dest, big_limit, done_files, prog)
            if s.dest == 'store:ufs' and target != 'sd128' or s.dest == 'store:sd' and target != 'sd128':
                medium = 'ufs' if s.dest == 'store:ufs' else target
                tmp = dest.with_name(dest.name + '.src')
                os.replace(dest, tmp)
                retarget_store(tmp, dest, medium, target, cat)
                tmp.unlink()
                size, sha = dest.stat().st_size, _sha(dest)
        st.d[s.key] = {'size': size, 'sha256': sha, 'layout': target, 'at': time.strftime('%Y-%m-%d %H:%M:%S')}
        st.save()
        done_files.append(dest)
        items.append(dict(rec, size=size, sha256=sha))
    name = f'Starbite S9 bundle ({"from the main phone" if from_phone else "local build outputs"}, ' \
           f'{target} card layout, {time.strftime("%Y-%m-%d")})'
    (out / mf.MANIFEST_NAME).write_text(manifest_text(name, items, target, ufslayout.ACTIVE))
    m = mf.load(out)
    probs = mf.verify_bundle(m, mf.HashCache(state_dir / 'hash-cache.json' if state_dir else None))
    lay = mf.layout_problems(m)
    for p in probs + lay:
        ui.warn(p)
    ui.ok(f'{out / mf.MANIFEST_NAME}: {len(items)} items, {human(sum(i.get("size", 0) for i in items))}; '
          f'{"verified" if not probs and not lay else "NOT complete: see the warnings"}')
    return 0 if not probs and not lay else 1


def _sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(8 * MIB), b''):
            h.update(c)
    return h.hexdigest()


def _local(ui, s, lp, dest, big_limit, done_files, prog):
    """A local source into the bundle: a hard link when it is a plain file on the same volume, else
    streamed (a .gz decompressed on the way)."""
    gz = lp.suffix == '.gz'
    size = gz_size(lp) if gz else lp.stat().st_size
    if big_limit and size > big_limit:
        raise Refused(f'{s.key} is {human(size)}: the bundle\'s volume is FAT32 (files up to 4 GiB)')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.unlink(missing_ok=True)
    if not gz:
        try:
            os.link(lp, dest)
            ui.ok(f'{s.key}: {lp} (hard link, no copy)')
            return size, _sha(dest)
        except OSError:
            pass
    wait_for_space(ui, dest.parent, size + 512 * MIB, done_files)
    _, sha = write_stream(file_stream(lp), dest, size, progress=prog)
    ui.ok(f'{s.key}: {lp} ({"decompressed, " if gz else ""}{human(size)}, sha256 {sha[:12]}...)')
    return size, sha
