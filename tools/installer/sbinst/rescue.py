"""The Starbite rescue USB disk: build the image (tools/rescue/mk-rescue-usb.sh: MBR + FAT32
"SB-RESCUE" + starbite-rescue/rescue.sh + rescue.sha256) and write it to a USB stick, only after
showing the disk and having the user type its name. The phone side (the rescue ramdisk in the OS
store, the Bixby > Starbite Recovery > Rescue mode path) is docs/STARBITE-RECOVERY.md."""
import hashlib
import os
import platform
import shutil
import subprocess
from pathlib import Path

from . import disks
from .common import MIB, REPO, Refused, Stopped, human

MK = REPO / 'tools' / 'rescue' / 'mk-rescue-usb.sh'


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(8 * MIB), b''):
            h.update(b)
    return h.hexdigest()


def build_image(out, ui, size_mib=64):
    """Build the image file. Linux: mk-rescue-usb.sh directly (it uses sudo for a loop mount).
    macOS: the same script in the Lima VM "starbite", streamed back to the Mac. Never a device."""
    out = Path(out).resolve()
    if str(out).startswith('/dev/'):
        raise Refused(f'{out}: --make-rescue-image writes an image FILE; use --make-rescue-usb for a stick')
    if out.exists() and not out.is_file():
        raise Refused(f'{out} exists and is not a regular file')
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + '.part')
    ui.h1('Building the rescue USB image')
    if platform.system() == 'Linux' and all(shutil.which(t) for t in ('sfdisk', 'mkfs.vfat', 'fsck.vfat')):
        ui.cmd(f'bash {MK.relative_to(REPO)} --size {size_mib} {tmp}')
        p = subprocess.run(['bash', str(MK), '--size', str(size_mib), str(tmp)], text=True)
        if p.returncode:
            raise Stopped('mk-rescue-usb.sh failed')
        os.replace(tmp, out)
    elif shutil.which('limactl'):
        script = (f'set -e; d=$(mktemp -d /var/tmp/sbinst-rescue.XXXX); trap "rm -rf $d" EXIT; '
                  f'bash "{MK}" --size {size_mib} "$d/r.img" >&2; cat "$d/r.img.sha256" >&2; cat "$d/r.img"')
        ui.cmd(f'limactl shell starbite -- bash -c \'... mk-rescue-usb.sh --size {size_mib} $d/r.img; cat $d/r.img\' > {out.name}')
        with open(tmp, 'wb') as f:
            p = subprocess.run(['limactl', 'shell', 'starbite', '--', 'bash', '-c', script], stdout=f,
                               stderr=subprocess.PIPE, text=False)
        err = p.stderr.decode(errors='replace')
        for line in err.strip().splitlines()[-4:]:
            ui.info('| ' + line)
        want = next((l.split()[0] for l in err.splitlines() if l.endswith(' r.img')), None)
        if p.returncode or want is None:
            tmp.unlink(missing_ok=True)
            raise Stopped('building the rescue image in the Lima VM failed')
        got = sha256_file(tmp)
        if got != want:
            tmp.unlink(missing_ok=True)
            raise Stopped(f'the image changed on its way out of the VM ({got} != {want})')
        os.replace(tmp, out)
    else:
        raise Refused('building the image needs Linux with sfdisk + dosfstools (sudo for a loop mount), '
                      'or the Lima VM "starbite" on macOS')
    h = sha256_file(out)
    Path(str(out) + '.sha256').write_text(f'{h}  {out.name}\n')
    ui.ok(f'{out}: {human(out.stat().st_size)}, sha256 {h}')
    return out, h


def write_stick(image, dev, ui, mode):
    """Show the disk, refuse unsafe ones, require the typed device name, write, read back."""
    image = Path(image)
    if not image.is_file():
        raise Refused(f'no rescue image {image} (build it with --make-rescue-image FILE)')
    size = image.stat().st_size
    want = sha256_file(image)
    ui.h1(f'Rescue USB stick: {dev}')
    try:
        info = disks.disk_info(dev)
    except (ValueError, subprocess.CalledProcessError) as e:
        raise Refused(str(e)) from None
    ui.table(info.rows(), ('', ''))
    why = disks.refusals(info)
    if size > info.size > 0:
        why.append(f'the image ({human(size)}) is larger than the disk')
    if why:
        raise Refused(f'{dev} will not be written: ' + '; '.join(why))
    fake = os.environ.get('SBINST_FAKE_DISKS')
    if mode == 'rehearse' and not fake:
        raise Refused('--rehearse writes only to a fake disk (SBINST_FAKE_DISKS)')
    is_mac = disks.mac_dev_name(dev) is not None
    raw = f'/dev/r{info.name}' if is_mac else dev
    mib = (size + MIB - 1) // MIB
    if is_mac:
        steps = {'prepare': ['diskutil', 'unmountDisk', f'/dev/{info.name}'],
                 'write': ['sudo', 'dd', f'if={image}', f'of={raw}', 'bs=1m'],
                 'read back': ['sh', '-c', f'sudo dd if={raw} bs=1m count={mib} 2>/dev/null | head -c {size} | shasum -a 256'],
                 'finish': ['diskutil', 'eject', f'/dev/{info.name}']}
    else:
        steps = {'prepare': ['sync'],
                 'write': ['sudo', 'dd', f'if={image}', f'of={raw}', 'bs=4M', 'conv=fsync', 'status=progress'],
                 'read back': ['sh', '-c', f'sudo head -c {size} {raw} | sha256sum'],
                 'finish': ['sync']}
    ui.info(f'image: {image} ({human(size)}, sha256 {want[:16]}...)')
    ui.info('commands:')
    for k, c in steps.items():
        ui.cmd(f'{" ".join(c)}    ({k})')
    if mode == 'dry-run':
        ui.ok('dry run: nothing written. Run again with --real to write the stick.')
        return
    if not ui.typed(f'This ERASES everything on {dev} ({info.model or "unknown model"}, {human(info.size)}).',
                    info.name):
        raise Refused('the device name was not typed: nothing written')
    again = disks.disk_info(dev)           # the disk must still be the one that was shown
    if (again.size, again.model, again.protocol) != (info.size, info.model, info.protocol):
        raise Refused(f'{dev} changed while waiting (was {info.model} {info.size}, now {again.model} {again.size})')
    if fake:
        target = Path(fake) / 'dev' / info.name
        with open(image, 'rb') as s, open(target, 'r+b') as d:
            shutil.copyfileobj(s, d, 4 * MIB)
        with open(target, 'rb') as d:
            back = hashlib.sha256(d.read(size)).hexdigest()
    else:
        for k in ('prepare', 'write'):
            if subprocess.run(steps[k]).returncode:
                raise Stopped(f'{" ".join(steps[k])} failed')
        out = subprocess.run(steps['read back'], capture_output=True, text=True).stdout.split()
        back = out[0] if out else ''
        subprocess.run(steps['finish'])
    if back != want:
        raise Stopped(f'read back {back[:16]}... != image {want[:16]}...: the stick is not good; write it again')
    ui.ok(f'{dev}: rescue disk written and read back (sha256 {want[:16]}...). Label SB-RESCUE.')
