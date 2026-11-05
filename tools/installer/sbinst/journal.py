"""The install journal: what this installer has written and verified on one phone, so a re-run
skips finished work, resumes interrupted work and never mistakes its own earlier write for
foreign data. One JSON file per phone: <state>/<phone-id>/journal.json (written atomically)."""
import json
import os
import time
from pathlib import Path


class Journal:
    def __init__(self, path):
        self.path = Path(path)
        self.d = {'version': 1, 'items': {}, 'steps': {}, 'dry_runs': [], 'history': []}
        if self.path.is_file():
            self.d.update(json.loads(self.path.read_text()))

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix('.tmp')
        tmp.write_text(json.dumps(self.d, indent=1, sort_keys=True))
        os.replace(tmp, self.path)

    # -- items ---------------------------------------------------------------------------------
    @staticmethod
    def ident(item):
        return {'dest': str(item.dest), 'sha256': item.sha256 or item.action}

    def item_rec(self, item):
        return self.d['items'].get(item.key)

    def item_done(self, item):
        """True when this exact content (same key, destination and SHA-256) was written and
        verified earlier."""
        r = self.item_rec(item)
        return bool(r and r.get('status') == 'done' and all(r.get(k) == v for k, v in self.ident(item).items()))

    def wrote_dest_before(self, item):
        """This installer wrote the item's destination before (maybe another version)."""
        r = self.item_rec(item)
        return bool(r and r.get('dest') == str(item.dest) and r.get('status') in ('done', 'started'))

    def mark_item(self, item, status, **extra):
        rec = dict(self.ident(item), status=status, at=time.strftime('%Y-%m-%d %H:%M:%S'), **extra)
        self.d['items'][item.key] = rec
        self.d['history'].append({'item': item.key, **rec})
        self.save()

    def chunk_state(self, item):
        """Per-item chunk state for resumable writers; tied to the content, so a new version of
        an image never resumes from an older version's chunks."""
        return self.path.parent / 'chunks' / f'{item.key}-{(item.sha256 or item.action)[:12]}.state'

    # -- steps ---------------------------------------------------------------------------------
    def step(self, name):
        return self.d['steps'].get(name, {})

    def mark_step(self, name, status, **extra):
        self.d['steps'][name] = dict(status=status, at=time.strftime('%Y-%m-%d %H:%M:%S'), **extra)
        self.save()

    # -- dry runs ------------------------------------------------------------------------------
    def record_dry_run(self, manifest_sha):
        if manifest_sha not in self.d['dry_runs']:
            self.d['dry_runs'].append(manifest_sha)
        self.save()

    def had_dry_run(self, manifest_sha):
        return manifest_sha in self.d['dry_runs']
