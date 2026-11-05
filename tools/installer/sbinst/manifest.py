"""installer-manifest.toml: what a bundle installs, where, and how it is checked (docs/INSTALLER.md 3).

A bundle is a directory holding installer-manifest.toml and the files it names. Every file has a
size and a SHA-256; every item has exactly one destination from a fixed grammar, and the grammar
cannot name a protected area. Verification never writes anything except the hash cache."""
import hashlib
import importlib.util
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

from . import tomlmini
from .common import (MIB, MODELS, DEVICES, MIN_CARD_BYTES, NATIVE_PARTS, PART_WRITE_PY, PROTECTED,
                     PROTECTED_RAW, WRITABLE_PARTS, Refused, TOOLS)

MANIFEST_NAME = 'installer-manifest.toml'
SLOT_BYTES = 16 << 30                     # USERDATA 0..16 GiB: the Apple (an earlier port iOS 16) slot
ACTIONS = ('write', 'mkfs-metadata')
_HEX64 = re.compile(r'^[0-9a-f]{64}$')
_KEY = re.compile(r'^[a-z0-9][a-z0-9-]{0,31}$')
_CARD_PATH = re.compile(r'^/[A-Za-z0-9._/-]+$')


@dataclass(frozen=True)
class Dest:
    kind: str          # part | slot | store | card-lba | card-file
    name: str = ''     # partition name, store medium, or card path
    offset: int = 0    # slot: byte offset; card-lba: start sector

    def __str__(self):
        return {'part': f'part:{self.name}', 'slot': f'slot:USERDATA@{self.offset}',
                'store': f'store:{self.name}', 'card-lba': f'card:lba:{self.offset}',
                'card-file': f'card:file:{self.name}'}[self.kind]

    def where(self):
        """Plain words for the plan screen."""
        return {'part': f'UFS partition {self.name}',
                'slot': f'UFS USERDATA bytes {self.offset}.. (Apple slot, below 16 GiB)',
                'store': f'{"UFS" if self.name == "ufs" else "microSD p2"} OS store',
                'card-lba': f'microSD sectors {self.offset}..',
                'card-file': f'microSD p1 (exFAT) {self.name}'}[self.kind]


def parse_dest(s):
    """One destination string -> Dest, or Refused with the reason. The only way anything is
    written is through a Dest, so this is the installer's allowlist."""
    if not isinstance(s, str):
        raise Refused(f'destination {s!r} is not a string')
    kind, _, rest = s.partition(':')
    if kind == 'part':
        if rest.upper() in PROTECTED or rest.lower() in PROTECTED_RAW or rest.upper().startswith('BOTA'):
            raise Refused(f'destination {s}: {rest} is a protected area; the installer never writes it')
        if rest not in WRITABLE_PARTS:
            raise Refused(f'destination {s}: {rest} is not one of the writable partitions {sorted(WRITABLE_PARTS)}')
        return Dest('part', rest)
    if kind == 'slot':
        m = re.fullmatch(r'USERDATA@(\d+)', rest)
        if not m:
            raise Refused(f'destination {s}: expected slot:USERDATA@<byte offset>')
        off = int(m.group(1))
        if off % 4096:
            raise Refused(f'destination {s}: offset is not a multiple of 4096')
        if off >= SLOT_BYTES:
            raise Refused(f'destination {s}: offset is past the 16 GiB Apple slot (the OS store starts there)')
        return Dest('slot', 'USERDATA', off)
    if kind == 'store':
        if rest not in ('ufs', 'sd'):
            raise Refused(f'destination {s}: expected store:ufs or store:sd')
        return Dest('store', rest)
    if kind == 'card':
        sub, _, val = rest.partition(':')
        if sub == 'lba':
            if not val.isdigit():
                raise Refused(f'destination {s}: expected card:lba:<start sector>')
            start = int(val)
            if start % 2048:
                raise Refused(f'destination {s}: start sector is not 1 MiB aligned')
            if start == 0:
                raise Refused(f'destination {s}: sector 0 is the card MBR (written only by the card layout step)')
            return Dest('card-lba', '', start)
        if sub == 'file':
            if not _CARD_PATH.match(val) or '..' in val.split('/') or val.endswith('/'):
                raise Refused(f'destination {s}: card paths are absolute, plain characters, no ".."')
            return Dest('card-file', val)
    raise Refused(f'destination {s!r}: unknown kind (part: slot: store: card:lba: card:file:)')


@dataclass
class Item:
    key: str
    title: str
    os: str
    dest: Dest
    action: str = 'write'
    file: str = ''
    size: int = 0
    sha256: str = ''
    data: bool = False          # holds user data once its OS has run: never replaced without --replace
    optional: bool = False
    note: str = ''
    path: Path = None           # resolved in the bundle (None: not present)
    present: bool = False


@dataclass
class Manifest:
    path: Path
    bundle: Path
    raw: dict
    name: str
    phone: dict
    card: dict
    layout: list
    items: list = field(default_factory=list)
    sha256: str = ''

    def item(self, key):
        return next(i for i in self.items if i.key == key)


def _need(d, k, typ, where):
    if k not in d:
        raise Refused(f'{where}: missing "{k}"')
    if not isinstance(d[k], typ) or (typ is int and isinstance(d[k], bool)):
        raise Refused(f'{where}: "{k}" must be {typ.__name__}')
    return d[k]


def load(bundle):
    """Parse and check the manifest's structure (no file access beyond the manifest itself)."""
    bundle = Path(bundle)
    path = bundle / MANIFEST_NAME
    if not path.is_file():
        raise Refused(f'no {MANIFEST_NAME} in {bundle}')
    text = path.read_text()
    try:
        raw = tomlmini.loads(text)
    except tomlmini.TOMLError as e:
        raise Refused(f'{path}: {e}') from None
    if raw.get('format') != 1:
        raise Refused(f'{path}: format must be 1 (this installer reads format 1)')
    phone = raw.get('phone', {})
    for k in ('models', 'devices'):
        phone.setdefault(k, list(MODELS if k == 'models' else DEVICES))
    if not set(phone['models']) <= set(MODELS):
        raise Refused(f'{path}: [phone] models {phone["models"]} include a model this installer does not know')
    phone.setdefault('min_battery', 50)
    card = raw.get('card', {})
    card.setdefault('min_bytes', MIN_CARD_BYTES)
    if card['min_bytes'] < MIN_CARD_BYTES:
        raise Refused(f'{path}: [card] min_bytes is below the 64 GB rule ({MIN_CARD_BYTES})')
    for p in ('p1', 'p2'):
        if p in card:
            for k in ('type', 'start', 'sectors'):
                _need(card[p], k, int, f'[card] {p}')
            if card[p]['start'] % 2048:
                raise Refused(f'[card] {p} does not start on a 1 MiB boundary')
    layout = raw.get('layout', [])
    for i, L in enumerate(layout):
        _need(L, 'name', str, f'[[layout]] {i}')
        _need(L, 'blocks', int, f'[[layout]] {L["name"]}')
    m = Manifest(path=path, bundle=bundle, raw=raw, name=raw.get('name', bundle.name), phone=phone,
                 card=card, layout=layout, sha256=hashlib.sha256(text.encode()).hexdigest())
    seen_keys, seen_dest = set(), {}
    for n, it in enumerate(raw.get('item', [])):
        where = f'[[item]] {n + 1}'
        key = _need(it, 'key', str, where)
        if not _KEY.match(key):
            raise Refused(f'{where}: key {key!r} must be lower case letters, digits and "-"')
        if key in seen_keys:
            raise Refused(f'{where}: duplicate key {key}')
        seen_keys.add(key)
        dest = parse_dest(_need(it, 'dest', str, f'item {key}'))
        action = it.get('action', 'write')
        if action not in ACTIONS:
            raise Refused(f'item {key}: action {action!r} is not one of {ACTIONS}')
        if action == 'mkfs-metadata' and str(dest) != 'part:ODM':
            raise Refused(f'item {key}: mkfs-metadata only applies to part:ODM')
        if str(dest) == 'part:ODM' and action != 'mkfs-metadata':
            raise Refused(f'item {key}: ODM (Android /metadata) is only ever made fresh (action = "mkfs-metadata")')
        if str(dest) in seen_dest:
            raise Refused(f'item {key}: destination {dest} is also used by item {seen_dest[str(dest)]}')
        seen_dest[str(dest)] = key
        item = Item(key=key, title=it.get('title', key), os=it.get('os', 'starbite'), dest=dest,
                    action=action, data=bool(it.get('data', False)) or action == 'mkfs-metadata', optional=bool(it.get('optional', False)),
                    note=it.get('note', ''))
        if action == 'write':
            item.file = _need(it, 'file', str, f'item {key}')
            item.size = _need(it, 'size', int, f'item {key}')
            item.sha256 = _need(it, 'sha256', str, f'item {key}').lower()
            if not _HEX64.match(item.sha256):
                raise Refused(f'item {key}: sha256 must be 64 hex digits')
            f = Path(item.file)
            if f.is_absolute() or '..' in f.parts:
                raise Refused(f'item {key}: file must be a path inside the bundle')
            if item.size <= 0:
                raise Refused(f'item {key}: size must be positive')
            if dest.kind in ('part', 'slot', 'store') and item.size % 4096:
                raise Refused(f'item {key}: a UFS image must be a multiple of 4096 bytes')
            if dest.kind == 'card-lba' and item.size % MIB:
                raise Refused(f'item {key}: a card range image must be a multiple of 1 MiB')
            if dest.kind == 'slot' and dest.offset + item.size > SLOT_BYTES:
                raise Refused(f'item {key}: {dest.offset}+{item.size} runs past the 16 GiB Apple slot')
        m.items.append(item)
    _check_card_ranges(m)
    return m


def _check_card_ranges(m):
    """Card ranges must lie inside p2 and not overlap each other (card-range-flash.py checks the
    store's used end on the phone)."""
    p2 = m.card.get('p2')
    ranges = []
    for it in m.items:
        if it.dest.kind != 'card-lba':
            continue
        s, e = it.dest.offset, it.dest.offset + it.size // 512
        if p2 and not (p2['start'] <= s and e <= p2['start'] + p2['sectors']):
            raise Refused(f'item {it.key}: card sectors {s}..{e - 1} are not inside [card] p2')
        for k, (s2, e2) in ranges:
            if s < e2 and s2 < e:
                raise Refused(f'item {it.key}: card sectors overlap item {k}')
        ranges.append((it.key, (s, e)))


class HashCache:
    """sha256 of bundle files, keyed by (path, size, mtime): a 40 GB bundle is hashed once."""

    def __init__(self, path):
        self.path = Path(path) if path else None
        self.d = {}
        if self.path and self.path.is_file():
            try:
                self.d = json.loads(self.path.read_text())
            except ValueError:
                self.d = {}

    def sha256(self, p, progress=None):
        st = p.stat()
        k = f'{p.resolve()}|{st.st_size}|{st.st_mtime_ns}'
        if k in self.d:
            return self.d[k]
        h = hashlib.sha256()
        with open(p, 'rb') as f:
            done = 0
            while True:
                b = f.read(8 * MIB)
                if not b:
                    break
                h.update(b)
                done += len(b)
                if progress and done % (1024 * MIB) < 8 * MIB:
                    progress(done, st.st_size)
        self.d[k] = h.hexdigest()
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_text(json.dumps(self.d, indent=0))
        return self.d[k]


def verify_bundle(m, cache=None, progress=None):
    """Check every file: present, inside the bundle, size and SHA-256. Returns the problem list;
    missing optional items are not problems (they are skipped and reported)."""
    cache = cache or HashCache(None)
    problems = []
    root = m.bundle.resolve()
    for it in m.items:
        if it.action != 'write':
            it.present = True
            continue
        p = (m.bundle / it.file)
        if not p.exists():
            it.present = False
            if not it.optional:
                problems.append(f'{it.key}: missing file {it.file}')
            continue
        rp = p.resolve()
        if root not in rp.parents:
            problems.append(f'{it.key}: {it.file} resolves outside the bundle ({rp})')
            continue
        if not rp.is_file():
            problems.append(f'{it.key}: {it.file} is not a regular file')
            continue
        size = rp.stat().st_size
        if size != it.size:
            problems.append(f'{it.key}: {it.file} is {size} bytes, the manifest says {it.size}')
            continue
        got = cache.sha256(rp, progress)
        if got != it.sha256:
            problems.append(f'{it.key}: {it.file} sha256 {got[:16]}..., the manifest says {it.sha256[:16]}...')
            continue
        it.path, it.present = rp, True
    return problems


def update_hashes(bundle):
    """Rewrite size/sha256 lines of every item whose file exists (for whoever builds a bundle).
    Text-level, so comments and order survive."""
    path = Path(bundle) / MANIFEST_NAME
    lines = path.read_text().splitlines(keepends=True)
    out, cur_file, changed = [], None, []
    blocks, cur = [], []
    for ln in lines:                       # split into [[item]] blocks
        if ln.strip() == '[[item]]' and cur:
            blocks.append(cur)
            cur = []
        cur.append(ln)
    blocks.append(cur)
    for b in blocks:
        text = ''.join(b)
        mf = re.search(r'^file\s*=\s*"([^"]+)"', text, re.M)
        if text.lstrip().startswith('[[item]]') and mf and (Path(bundle) / mf.group(1)).is_file():
            p = Path(bundle) / mf.group(1)
            h = HashCache(None).sha256(p)
            size = p.stat().st_size
            text = re.sub(r'^size\s*=.*$', f'size = {size}', text, flags=re.M)
            text = re.sub(r'^sha256\s*=.*$', f'sha256 = "{h}"', text, flags=re.M)
            changed.append(mf.group(1))
        out.append(text)
    path.write_text(''.join(out))
    return changed


def _sbstore2():
    spec = importlib.util.spec_from_file_location('sbstore2', TOOLS / 'store' / 'sbstore2.py')
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
    except Exception:              # tomllib missing on old Pythons: coverage is then skipped
        return None
    return mod


def coverage(m):
    """Which catalog entries this bundle can boot: each visible entry's files must be in one of
    the bundle's stores, and its root must be an item. Returns (rows, warnings). Needs the
    bundle's catalog (manifest key `catalog`, a catalog.toml) and its store images."""
    rows, warns = [], []
    cat_file = m.raw.get('catalog')
    if not cat_file:
        return rows, ['the manifest names no catalog: which OS entries the bundle covers is not checked']
    cp = m.bundle / cat_file
    if not cp.is_file():
        return rows, [f'catalog {cat_file} is not in the bundle']
    entries = tomlmini.loads(cp.read_text()).get('entry', [])
    sb = _sbstore2()
    names = set()
    store_keys = {}
    for it in m.items:
        if it.dest.kind == 'store' and it.present and sb is not None:
            try:
                store = sb.read_store(it.path)
            except SystemExit as e:
                warns.append(f'{it.key}: not a valid store image ({e})')
                continue
            names |= {n for n, _, _ in store}
            cat = {n: b for n, _, b in store}.get('catalog')
            if cat is not None:
                store_keys[it.dest.name] = set(sb.catalog_keys(cat))
    if sb is None:
        warns.append('tools/store/sbstore2.py needs Python 3.11+: store contents not checked')
    elif 'ufs' in store_keys:
        want = {e['key'] for e in entries}
        if store_keys['ufs'] != want:
            warns.append(f'the UFS store\'s catalog differs from {cat_file}: '
                         f'only in store {sorted(store_keys["ufs"] - want)}, only in toml {sorted(want - store_keys["ufs"])}')
    dests = {str(i.dest): i for i in m.items if i.present}
    for e in entries:
        if 'hidden' in e.get('flags', []):
            continue
        missing = []
        if sb is not None and names:
            for f in e.get('files', []):
                alts = [a.split(':', 1)[-1] for a in f.split('|')]
                if '@bundle' not in alts and not any(a in names for a in alts):
                    missing.append(f'store file {f}')
        root = e.get('root', '')
        need = None
        mm = re.match(r'sd:p1 (/\S+)', root)
        if mm:
            need = f'card:file:{mm.group(1)}'
        mm = re.match(r'sd:lba (\d+)\+', root)
        if mm:
            need = f'card:lba:{mm.group(1)}'
        mm = re.match(r'ufs:(SYSTEM|WINDOWS)\b', root)
        if mm:
            need = f'part:{mm.group(1)}'
        if root.startswith('ufs:USERDATA slot'):
            need = 'slot:USERDATA@0'
        if need and need not in dests:
            missing.append(f'root {need}')
        rows.append((e['key'], e.get('name', e['key']), 'covered' if not missing else 'MISSING ' + ', '.join(missing)))
        if missing:
            warns.append(f'{e.get("name", e["key"])} ({e["key"]}) will not boot from this bundle: {", ".join(missing)}')
    return rows, warns


def writer_of(item):
    """Which program writes an item (shown on the plan screen)."""
    d = item.dest
    if item.action == 'mkfs-metadata':
        return 'tools/android/pixel/metadata-reset.sh'
    if d.kind == 'part' and d.name in PART_WRITE_PY:
        return 'tools/android/pixel/part-write.py'
    if d.kind == 'part' and d.name in NATIVE_PARTS:
        return 'installer chunk writer'
    if d.kind == 'slot':
        return 'tools/refport/slot-write.py'
    if d.kind == 'store':
        return 'tools/store/store-flash.sh' if d.name == 'ufs' else 'tools/store/store-flash-sd.sh'
    if d.kind == 'card-lba':
        return 'tools/macos/card-range-flash.py'
    return 'installer chunk writer (file)'

