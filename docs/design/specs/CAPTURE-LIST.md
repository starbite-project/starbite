# iOS 27 capture session on the S9: what to capture (design-ref, 2026-11-13)

> **Status 2026-11-13 ~19:50: the user cancelled the attended iOS 27 capture (groups A-F).** Their decision, via
> the main session: the specs come from public iOS 27 sources (Apple's pages and HIG, press images, reviews,
> walkthroughs), each value marked "measured from <source>" or E. **Group G stays:** it is Starbite on our own phone,
> from the self test's calibration stills. Groups A-F are kept below only as a checklist of what the specs must cover.

**Goal:** one attended session on this S9 running iOS 27, to turn the specs' **E** values into **M**. Captures are for
study only. They stay local in `build/design-ref/ios27-captures/` in the design-ref worktree (git-ignored) and are
never committed or shipped.

## How to capture (the main session picks what works on our iOS 27 port)
1. **Screenshots in iOS:** Side + Volume Up. On the S9 that is probably Power + Volume Up; check the button mapping.
   Native 3x PNGs are exact.
2. **Screen recordings:** Control Centre's Screen Recording control, if ReplayKit works on our port. 60 fps H.264 is
   ideal. If the control isn't there, add it under Settings > Control Centre.
3. **Getting files off the phone:** iOS saves both to Photos. Pull them from the iOS 27 data volume the way the main
   session already reads the iOS root (TWRP / the card overlay window). If that doesn't work, a kernel framebuffer
   dump for stills is fine.
4. **Fallback for glass and motion:** a camera photo or video of the phone (Photo Booth, see the memory note),
   square-on, lights dimmed.
   - This is also the only way to compare **glass on the panel** between iOS 27 and Starbite.
   - Use the same wallpaper image on both.

**Same conditions for both OSes:**
- the same wallpaper: one plain photo we own, plus "Starbite Dawn" exported as an image;
- dark appearance;
- Liquid Glass slider at its default;
- brightness 50 %;
- notifications from our own test texts.

## A. System UI (stills, then the motion clips; ~25 min)
| # | Still | Measures |
|---|---|---|
| A1 | Lock with 2 notifications (one group of 2, one single) | platter, stack peeks, clock / date positions |
| A2 | Lock, a group expanded | the expanded stack, "Show Less", the x circle |
| A3 | Notification Centre pulled down, scrolled once | the header, the list |
| A4 | A banner over an app (send a text from another phone) | the banner y, size, radius |
| A5 | Home page 1 + page 2 + App Library | the grid, dock, search pill |
| A6 | Home with an icon long-pressed (Quick Actions menu) | menu width, rows, radius, blur |
| A7 | Control Centre page 1, the connectivity module long-pressed, a slider long-pressed, edit mode | every CC size |
| A8 | The app switcher with 4+ apps | the deck |
| A9 | Spotlight (swipe down on home) with "se" typed | search field, results |
| A10 | Keyboard: letters, 123, #+=, a long-pressed "e", the emoji panel; light and dark | keys, balloon, accents |
| A11 | An alert (e.g. delete a note), an action sheet / popover (Photos delete), a share sheet | §3 values |
| A12 | Settings root, scrolled to the bottom; search field focused | rows, search |

**Motion clips (screen recording, 3-5 s each):**
1. unlock;
2. open and close an app from the home screen and from the dock;
3. close with a fling vs a slow drag;
4. the app switcher: enter, browse, fling-close;
5. home-bar swipe to the previous app;
6. Control Centre open / close and a module expand;
7. Notification Centre pull;
8. banner in / out;
9. a sheet: medium → large → dismiss;
10. keyboard up / down;
11. push / pop + left-edge swipe back;
12. large-title collapse on scroll;
13. tab switch;
14. a glass button press-and-hold.

## B. App Store (~10 min; works signed out)
| # | Capture |
|---|---|
| B1 | Today tab top (date header, first card), scrolled to a list card |
| B2 | Tap a Today card: a **recording** of the card-to-story zoom, the story scrolled, and the close by the x and by dragging down |
| B3 | Apps tab, Games tab, Search tab (empty, then with "notes" typed: suggestions and results) |
| B4 | A product page: top, scrolled (the nav bar takes GET), ratings, Information |
| B5 | Tap GET while signed out (the sign-in sheet shows the sheet geometry); a free app's button states if any app can be fetched |
| B6 | Account / Updates page |

## C. Phone, Messages, Contacts (~10 min; the SIM is in, calls attended only)
| # | Capture |
|---|---|
| C1 | Phone: the Calls list (unified), the filter menu, Keypad (digits typed, the add-contact link), Contacts tab |
| C2 | An outgoing call to a test number: the in-call screen, the keypad overlay, the call capsule in another app, then end. Attended |
| C3 | An incoming call, locked and unlocked (a second phone or the user) |
| C4 | Messages: the list, a thread with sent and received runs of 3, the timestamp row, "Delivered", the long-press menu with reactions, swipe-left timestamps, the compose screen with the To: field |
| C5 | **Recording:** typing and sending a message (the bubble animation), keyboard drag-dismiss |
| C6 | Contacts: the list with the index scrubber, a contact card, edit mode, the new-contact form |

## D. Clock, Calculator, Weather (~8 min)
| # | Capture |
|---|---|
| D1 | Clock: each tab; the add-alarm sheet with the wheel; the alarm-firing screen (set one for +1 min; record it); Stopwatch running with laps; a Timer running + its lock Live Activity |
| D2 | Calculator: basic, the type menu, Scientific (if portrait-capable), history, Convert; a long number (the font shrink) |
| D3 | Weather: the main page top and scrolled, the locations list, offline state (Wi-Fi + data off) |

## E. Camera, Photos (~8 min)
| # | Capture |
|---|---|
| E1 | Camera (if the iOS camera works on our port): Photo and Video modes, the settings sheet, zoom pill, after a shot (the thumbnail). If it doesn't work, the error / unavailable screen is still useful |
| E2 | Photos: the Library grid at each zoom, Years / Months, Collections, select mode |
| E3 | **Recording:** a tap → viewer zoom, swipe between, pinch, swipe-down dismiss into the tile; the info panel; the delete action sheet |

## F. Settings, Notes, Music, Files, Maps (~10 min)
| # | Capture |
|---|---|
| F1 | Settings: every first-level page we spec (Wi-Fi, Bluetooth, Cellular, Battery, General > About, Display & Brightness incl. the **Liquid Glass slider**, Wallpaper, Sounds & Haptics, Accessibility > Display & Text Size, Motion) |
| F2 | Notes: folders, the list, the editor with the formatting toolbar + Aa panel, a checklist, the swipe actions |
| F3 | Music: the Library, an album, Now Playing (any local file), the MiniPlayer over the tab bar, the minimised tab bar on scroll |
| F4 | Files: Browse, On My iPhone, list / icon views, a context menu, a preview |
| F5 | Maps: the main screen with its sheet at each detent (even with no tiles) |

## G. Glass calibration pairs (camera photos; ~10 min, the most important for 8c)
Use the same wallpaper and the same framing, iOS 27 then Starbite:
- G1: the lock with one notification;
- G2: the home dock and search pill;
- G3: Control Centre over the home screen;
- G4: a sheet at the medium detent over a list;
- G5: a nav-bar back button over scrolled colourful content.

Then Starbite again at `LENS_GAIN` .33 / .66 / 1.0 for G1-G3.

- **From batch 8a (trunk ca7f0209 on) the Starbite half runs itself.** The self test ends with the calibration stills:
  lock, home and Control Centre, each at 33 / 66 / 100 % glass strength, 3 s apiece. The strength is the runtime
  `lens_gain`, 1 by default.
- Each still is announced in the self-test log as `next: glass still <view>, glass strength N % at +T ms (3 s)`.
  Match each Photo Booth shot to its still by that line's `+T`. Take the shot in the middle of the 3 s window, not
  at its edges.
- Name the files `starbite-<view>-<N>.jpg` (e.g. `starbite-cc-66.jpg`) and the iOS 27 pairs `ios27-<view>.jpg`, so
  the comparison can be scripted.

**Total ≈ 90 min attended.** If time is short, do **A (stills + clips 1-6), B, G** first.
