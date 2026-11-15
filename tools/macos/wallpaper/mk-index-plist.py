#!/usr/bin/env python3
"""mk-index-plist.py OUT [IMAGE_PATH]: a wallpaper store (~/Library/Application Support/com.apple.wallpaper/Store/
Index.plist) whose desktop, on every display and space, is one still image (provider com.apple.wallpaper.choice.image,
configuration {type: imageFile, url: {relative: file URL}}). Layout = what macOS 15.6's WallpaperAgent writes
itself (card copy read 2026-11-15, S3zt): Type "linked" with one "Linked" entry, no EncodedOptionValues; the macOS 27
layout ("individual" + "Desktop"/"Idle") was rejected and replaced by the default (S3zt). SystemDefault stays the
default choice. Files lists the image too (S3zu, empty Files: the agent kept the choice but drew its
"in-process.error" wallpaper, plain yellow, and never started the image extension). Default image: Sequoia Sunrise's still frame, which is on the card root, unlike the 171 MiB video
that the default choice wants (S3zs)."""
import datetime, plistlib, sys, urllib.parse

out = sys.argv[1]
img = sys.argv[2] if len(sys.argv) > 2 else \
    '/System/Library/Desktop Pictures/.wallpapers/Sequoia Sunrise/Sequoia Sunrise.heic'
url = 'file://' + urllib.parse.quote(img)
now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
cfg = plistlib.dumps({'type': 'imageFile', 'url': {'relative': url}}, fmt=plistlib.FMT_BINARY)


def linked(choice):
    return {'Type': 'linked',
            'Linked': {'LastSet': now, 'LastUse': now,
                       'Content': {'Choices': [choice], 'Shuffle': '$null'}}}


image = {'Provider': 'com.apple.wallpaper.choice.image', 'Files': [{'relative': url}], 'Configuration': cfg}
default = {'Provider': 'default', 'Files': [], 'Configuration': b''}
d = {'AllSpacesAndDisplays': linked(image), 'SystemDefault': linked(default), 'Spaces': {}, 'Displays': {}}
with open(out, 'wb') as f:
    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
print(out, url)
