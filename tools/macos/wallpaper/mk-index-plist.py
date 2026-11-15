#!/usr/bin/env python3
"""mk-index-plist.py OUT [IMAGE_PATH]: a wallpaper store (~/Library/Application Support/com.apple.wallpaper/Store/
Index.plist) whose desktop, on every display and space, is one still image (provider com.apple.wallpaper.choice.image,
configuration {type: imageFile, url: {relative: file URL}}; layout as macOS writes it). Default image: Sequoia
Sunrise's still frame, which is on the card root, unlike the 171 MiB video that the default choice wants (S3zs)."""
import datetime, plistlib, sys, urllib.parse

out = sys.argv[1]
img = sys.argv[2] if len(sys.argv) > 2 else \
    '/System/Library/Desktop Pictures/.wallpapers/Sequoia Sunrise/Sequoia Sunrise.heic'
url = 'file://' + urllib.parse.quote(img)
now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
cfg = plistlib.dumps({'type': 'imageFile', 'url': {'relative': url}}, fmt=plistlib.FMT_BINARY)
opts = plistlib.dumps({'values': {}}, fmt=plistlib.FMT_BINARY)


def desktop():
    return {'Desktop': {'Content': {'Choices': [{'Configuration': cfg, 'Files': [],
                                                 'Provider': 'com.apple.wallpaper.choice.image'}],
                                    'EncodedOptionValues': opts, 'Shuffle': '$null'},
                        'LastSet': now, 'LastUse': now},
            'Type': 'individual'}


d = {'AllSpacesAndDisplays': desktop(), 'SystemDefault': desktop(), 'Displays': {}, 'Spaces': {}}
with open(out, 'wb') as f:
    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
print(out, url)
