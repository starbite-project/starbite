#!/usr/bin/env python3
# SPDX-License-Identifier: APSL-2.0
"""Regenerate the four event headers IOHIDFamily-2238.120.5 needs but does not publish, from IOHIDFamily-1915.140.3's
(the last tag that published them; pristine copies in upstream-1915.140.3/, APSL):
    python3 adapt-1915-to-2238.py          (run in this folder; rewrites ./IOHIDEvent{Types,StructDefs,MacroDefs,FieldDefs}.h)

What changes, and why (each change is forced by 2238 code, see NOTES.md "Event headers"):
  * IOHIDDigitizerEventData: 2238 layout, IOHIDDouble members and twist in an "angle" union with roll
    (0xa0 bytes; layout from the macOS 26.5 KDK IOHIDFamily.kext DWARF, kernel patch 0323). 2238 IOHIDEvent.cpp
    writes CAST_FIXED_TO_DOUBLE(...) into position/pressure/auxPressure/angle.twist/orientation.*.
  * IOHIDTranslationEventData, IOHIDPointerEventData: position.{x,y,z} are IOHIDDouble (2238 IOHIDEvent.cpp
    translationEvent / absolutePointerEvent / relativePointerEventWithFixed write CAST_FIXED_TO_DOUBLE into them).
    Layout of the rest unchanged; NOT verified against DWARF.
  * Field accessor macros (IOHIDEventMacroDefs.h) for every member above: storage conversions use
    CAST_x_TO_DOUBLE / CAST_DOUBLE_TO_x instead of the IOFixed ones (otherwise a getter would shift a double and a
    setter would store fixed-point integers into a double).
  * IOHIDCollectionEventData gains usagePage/usage/ungroupForLegacy; __IOHIDGameControllerEventData gains
    bottom.m1..m4; a heart-rate event type/struct is added. Layout NOT verified (not used on the S9).
  * kIOHIDEventTypeHeartRate (= 40, appended, so no existing number moves), kIOHIDEventOptionSynthetic (= 0: never
    set), kIOHIDDigitizerEventEstimatedRoll (1<<31), kIOHIDDigitizerEventUpdateRollMask (0),
    kIOHIDHeartRateSensorLocationUnknown (0). Values NOT verified.
IOHIDEventFieldDefs.h is copied unchanged."""
import re
from pathlib import Path

UP = Path('upstream-1915.140.3')


def sub1(s, old, new):
    if s.count(old) != 1:
        raise SystemExit('expected exactly one match for:\n' + old)
    return s.replace(old, new)


def struct_span(s, name):
    end = s.index('} %s;' % name) + len('} %s;' % name)
    start = s.rindex('typedef struct {', 0, end)
    return start, end


def types():
    s = (UP / 'IOHIDEventTypes.h').read_text()
    s = sub1(s, '    kIOHIDEventTypeGenericGesture,\n    kIOHIDEventTypeCount,',
             '    kIOHIDEventTypeGenericGesture,\n'
             '    kIOHIDEventTypeHeartRate,               // 40: Starbite, 2238 type; number NOT verified against 26.x\n'
             '    kIOHIDEventTypeCount,')
    s = sub1(s, '''    // misspellings
    kIOHIDEventOptionPixelUnits                             = kIOHIDEventOptionIsPixelUnits,
};''', '''    // misspellings
    kIOHIDEventOptionPixelUnits                             = kIOHIDEventOptionIsPixelUnits,

    // Starbite: 2238 option (IOHIDEventService keychord filter). Its bit is not published; 0 = never set, so
    // every event is treated as real (the 1915 behaviour).
    kIOHIDEventOptionSynthetic                              = 0,
};''')
    tail = '''
/* Starbite: 2238 constants used by IOHIDEvent.cpp / IOHIDEventData.h, not in 1915. Values NOT verified. */
#ifndef kIOHIDDigitizerEventEstimatedRoll
/* eventMask bit next to Estimated{Altitude,Azimuth,Pressure} (1<<28..30); the kernel only clears it. */
#define kIOHIDDigitizerEventEstimatedRoll   (1u << 31)
#endif
#ifndef kIOHIDDigitizerEventUpdateRollMask
/* 1915 has no kIOHIDEventFieldDigitizerRoll field number; 0 = a roll update is never reported. */
#define kIOHIDDigitizerEventUpdateRollMask  0u
#endif
#ifndef kIOHIDHeartRateSensorLocationUnknown
enum {
    kIOHIDHeartRateSensorLocationUnknown = 0,
};
#endif
'''
    i = s.rindex('#endif')
    return s[:i] + tail + s[i:]


def structdefs():
    s = (UP / 'IOHIDEventStructDefs.h').read_text()
    # digitizer
    a, b = struct_span(s, 'IOHIDDigitizerEventData')
    dig = s[a:b].replace('IOFixed', 'IOHIDDouble')
    dig = sub1(dig, '    IOHIDDouble twist;\n', '    union {\n        IOHIDDouble twist;\n        IOHIDDouble roll;\n    } angle;\n')
    dig = ('/*\n * Starbite: IOHIDFamily-2238 layout (0xa0 bytes; from the macOS 26.5 KDK IOHIDFamily.kext DWARF, kernel\n'
           ' * patch 0323): the 1915 IOFixed members are IOHIDDouble and twist sits in the "angle" union with roll.\n */\n'
           + dig +
           '\n_Static_assert(sizeof(IOHIDDigitizerEventData) == 0xa0, "IOHIDFamily-2238 digitizer event layout");\n'
           '_Static_assert(__builtin_offsetof(IOHIDDigitizerEventData, generationCount) == 0x90, '
           '"IOHIDFamily-2238 digitizer event layout");')
    s = s[:a] + dig + s[b:]
    # translation + pointer positions
    for name in ('IOHIDTranslationEventData', 'IOHIDPointerEventData'):
        a, b = struct_span(s, name)
        body = sub1(s[a:b], '''    struct {
        IOFixed x;
        IOFixed y;
        IOFixed z;
    } position;''', '''    struct {    /* Starbite: IOHIDDouble in 2238 (IOHIDEvent.cpp writes CAST_FIXED_TO_DOUBLE here) */
        IOHIDDouble x;
        IOHIDDouble y;
        IOHIDDouble z;
    } position;''')
        s = s[:a] + body + s[b:]
    s = sub1(s, '''    uint8_t depth;
    uint8_t reserved[3];
} IOHIDCollectionEventData;''', '''    uint8_t depth;
    uint8_t reserved[3];
    /* Starbite: 2238 members (IOHIDEvent::collectionEvent); layout NOT verified against 26.x */
    uint32_t usagePage;
    uint32_t usage;
    uint8_t ungroupForLegacy;
    uint8_t reserved2[3];
} IOHIDCollectionEventData;''')
    s = sub1(s, '''    IOFixed r4;
    IOFixed r5;
} __IOHIDGameControllerEventData;''', '''    IOFixed r4;
    IOFixed r5;
    /* Starbite: 2238 member (extendedGameControllerEventWithOptionalButtons); layout NOT verified */
    struct {
        IOFixed m1;
        IOFixed m2;
        IOFixed m3;
        IOFixed m4;
    } bottom;
} __IOHIDGameControllerEventData;''')
    s = sub1(s, '''typedef struct {
    IOHIDDigitizerEventData base;
} __IOHIDDigitizerEventData;''', '''typedef struct {
    IOHIDDigitizerEventData base;
} __IOHIDDigitizerEventData;

/* Starbite: 2238 event (IOHIDEvent::heartRateEvent); layout NOT verified against 26.x */
typedef struct {
    uint32_t size;
    IOHIDEventType type;
    uint32_t options;
    uint8_t depth;
    uint8_t reserved[3];
    uint32_t rate;
    IOHIDDouble confidence;
    uint32_t location;
    uint32_t generationCount;
} IOHIDHeartRateEventData;

typedef struct {
    IOHIDHeartRateEventData base;
} __IOHIDHeartRateEventData;''')
    return s


DOUBLE_MEMBERS = (r'\(\(IOHIDDigitizerEventData\*\)event\)->(?:position\.[xyz]|pressure|auxPressure|twist|'
                  r'orientation\.(?:tilt|polar|quality)\.[a-zA-Z]+)'
                  r'|\(\(IOHIDTranslationEventData\*\)event\)->position\.[xyz]'
                  r'|\(\(IOHIDPointerEventData\*\)event\)->position\.[xyz]')


def macrodefs():
    src = (UP / 'IOHIDEventMacroDefs.h').read_text()
    out, n = [], 0
    for line in src.split('\n'):
        if re.search(DOUBLE_MEMBERS, line):
            o = line
            if re.search('(?:' + DOUBLE_MEMBERS + r') = \(typeof', line):  # setter: value -> storage
                line = (line.replace('CAST_DOUBLE_TO_FIXED(value)', 'CAST_DOUBLE_TO_DOUBLE(value)')
                        .replace('CAST_FIXED_TO_FIXED(value)', 'CAST_FIXED_TO_DOUBLE(value)')
                        .replace('CAST_INTEGER_TO_FIXED(value)', 'CAST_INTEGER_TO_DOUBLE(value)'))
            elif 'value = (typeof(value))' in line:  # getter: storage -> value
                line = re.sub(r'CAST_FIXED_TO_(DOUBLE|FIXED|INTEGER)\(((?:' + DOUBLE_MEMBERS + r'))\)',
                              lambda m: 'CAST_DOUBLE_TO_' + m.group(1) + '(' + m.group(2) + ')', line)
            line = line.replace('((IOHIDDigitizerEventData*)event)->twist', '((IOHIDDigitizerEventData*)event)->angle.twist')
            n += line != o
        out.append(line)
    s = '\n'.join(out)
    hdr = ('/*\n * Starbite: IOHIDFamily-1915.140.3\'s generated field macros; the accessors of the members that are\n'
           ' * IOHIDDouble in 2238 (digitizer, translation and pointer positions; see IOHIDEventStructDefs.h) convert\n'
           ' * with CAST_x_TO_DOUBLE / CAST_DOUBLE_TO_x instead of the IOFixed ones (%d lines). The rest is 1915\'s.\n */\n' % n)
    i = s.index('#ifndef')
    return s[:i] + hdr + s[i:], n


def main():
    Path('IOHIDEventTypes.h').write_text(types())
    Path('IOHIDEventStructDefs.h').write_text(structdefs())
    m, n = macrodefs()
    Path('IOHIDEventMacroDefs.h').write_text(m)
    Path('IOHIDEventFieldDefs.h').write_text((UP / 'IOHIDEventFieldDefs.h').read_text())
    print('regenerated 4 headers; %d macro lines moved to double storage' % n)


if __name__ == '__main__':
    main()
