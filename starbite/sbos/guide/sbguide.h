/* SPDX-License-Identifier: MIT
 * Starbite OS offline survival guide (1.2-alpha28): the text, shared by the classic Guide app (apps/guide.c) and the
 * kernel's API for Starbite OS Next's Emergency mode (kern/guide.c). Plain data, no code.
 *
 * Content rules: mainstream first-aid and outdoor guidance (as taught by national Red Cross / resuscitation councils
 * and wilderness courses), the simplest safe action first, no folk remedies, every page short enough for one phone
 * screen (each line at most ~120 characters: the classic UI wraps a line to three rows). Always "call 112 first" where
 * help can come. Not a substitute for training: the first page says so. */
#ifndef SBGUIDE_H
#define SBGUIDE_H

struct sbg_page {
    const char *title;
    const char *const *lines;               /* NULL-terminated; a line starting with "! " is a warning (red) */
};
struct sbg_chapter {
    const char *title, *sub;
    unsigned argb;                          /* the chapter's colour */
    const struct sbg_page *pages;
    int n;
};

#define SBG_L(...) ((const char *const[]){ __VA_ARGS__, 0 })

static const struct sbg_page sbg_first[] = {
    { "Start here", SBG_L(
        "! Danger to life: call 112 if any network works. 112 works on any operator's network (some countries need a SIM).",
        "This guide is offline help, not a substitute for training or a doctor.",
        "STOP: Stop. Think. Observe. Plan. Panic wastes energy and leads to bad choices.",
        "Order of needs (rule of threes): about 3 minutes without air, 3 hours without shelter in harsh cold or heat, "
        "3 days without water, 3 weeks without food.",
        "So: first safety and first aid, then shelter and warmth, then water, then signalling, then food.") },
    { "Stay or move?", SBG_L(
        "If someone knows your route or plan, stay where you are: searchers start from your last known place.",
        "Move only if where you are is dangerous (rising water, rockfall, no shelter) or nobody will look for you.",
        "If you move, leave a sign: an arrow of stones or sticks, a note with the time and your direction.",
        "Travel in daylight. Rest before you are exhausted. Follow a valley or stream downhill toward people.") },
    { "Save the phone", SBG_L(
        "Emergency > Last 2 %: everything off except the clock, the torch and SOS. It runs on to 1 %.",
        "Keep the screen dim and short. Switch the phone off between checks if help is hours away.",
        "Keep it warm (inside your clothes): a cold battery dies early.",
        "The torch and the SOS beacon cost the most. Use them when someone may see them, not all night.") },
};

static const struct sbg_page sbg_aid[] = {
    { "Check first", SBG_L(
        "! Make sure the place is safe for you first (traffic, fire, water, falling rock, electricity).",
        "Talk to the person and tap their shoulders. No answer: shout for help and call 112.",
        "Open the airway: one hand on the forehead, two fingers lift the chin, tilt the head back.",
        "Look, listen and feel for normal breathing for up to 10 seconds. Gasping is not normal breathing.",
        "Breathing normally: recovery position (next page). Not breathing normally: start CPR.") },
    { "CPR (adult)", SBG_L(
        "! Not breathing normally: call 112 (speaker on), then start at once.",
        "Heel of one hand in the centre of the chest, the other hand on top, arms straight.",
        "Press 5-6 cm deep, 100-120 a minute (the beat of 'Stayin' Alive'). Let the chest come back up fully.",
        "Trained and willing: 30 presses, then 2 rescue breaths. Otherwise press without stopping (hands-only CPR).",
        "Keep going until help takes over, the person breathes normally, or you are too exhausted to go on.",
        "Children: press 1/3 of the chest's depth; give 5 rescue breaths first if you can.") },
    { "Recovery position", SBG_L(
        "For someone unconscious who breathes normally.",
        "Kneel beside them. Near arm at a right angle, palm up. Far hand against their near cheek.",
        "Bend the far knee up, pull it toward you so they roll onto their side.",
        "Tilt the head back so the airway stays open and fluid can drain from the mouth.",
        "Keep them warm and check their breathing every minute.") },
    { "Severe bleeding", SBG_L(
        "! Call 112. Bleeding that spurts or soaks through fast can kill within minutes.",
        "Press hard directly on the wound with a cloth or your hand. Keep pressing for at least 10 minutes.",
        "Blood soaks through: add more cloth on top and press harder. Do not lift the first layer.",
        "Wrap a tight bandage over the pad to keep the pressure.",
        "Arm or leg and pressure fails: a tourniquet 5-7 cm above the wound, tightened until the bleeding stops. "
        "Write the time on the skin. Do not loosen it.",
        "Lay the person down and keep them warm (shock).") },
    { "Shock", SBG_L(
        "Signs: pale, cold, clammy skin; fast weak pulse; fast breathing; confusion; thirst.",
        "Lay them flat; raise the legs a little if that causes no pain.",
        "Keep them warm, insulated from the ground. Calm them. Treat the cause (bleeding).",
        "Nothing to eat or drink (they may need surgery). Call 112 and watch the breathing.") },
    { "Choking (adult)", SBG_L(
        "Can cough: encourage coughing.",
        "Cannot cough, speak or breathe: up to 5 sharp back blows between the shoulder blades.",
        "Still blocked: up to 5 abdominal thrusts: from behind, a fist above the navel, pull sharply in and up.",
        "Repeat 5 and 5. Unconscious: call 112 and start CPR.",
        "Babies under 1: back blows and chest thrusts only, never abdominal thrusts.") },
    { "Burns", SBG_L(
        "Cool the burn under cool running (or clean) water for 20 minutes. No ice, butter or creams.",
        "Remove rings and watches before swelling; leave anything stuck to the skin.",
        "Cover loosely with cling film or a clean plastic bag; keep the person warm.",
        "Call 112 for large burns, burns to the face, hands, feet or genitals, and all chemical or electrical burns.") },
    { "Fractures and sprains", SBG_L(
        "Do not straighten a broken limb. Support it in the position found.",
        "Immobilise with a splint (sticks, a rolled mat) padded and tied above and below the injury.",
        "Check the fingers or toes stay warm and pink; loosen ties if not.",
        "Sprains: rest, cold (wrapped, 20 min at a time), firm bandage, raise it.",
        "! Suspected neck or back injury: do not move them unless their life is in danger.") },
    { "Hypothermia", SBG_L(
        "Signs: shivering, clumsiness, slurred speech, confusion. Severe: shivering stops, drowsy.",
        "Out of wind and wet. Replace wet clothes. Insulate from the ground first.",
        "Cover head and neck. Share body warmth in a sleeping bag. Warm sweet drinks only if fully awake.",
        "No alcohol. No rubbing the limbs. Handle a severely cold person gently and keep them lying down.") },
    { "Heat illness", SBG_L(
        "Heat exhaustion: headache, dizziness, sweating, cramps. Rest in the shade, drink, cool the skin.",
        "! Heat stroke: hot, confused or unconscious. Call 112 and cool them fast: wet the skin and fan it, "
        "cold packs at the neck, armpits and groin.",
        "Prevent it: work in the cool hours, drink before you are thirsty, cover your head.") },
    { "Allergic reaction", SBG_L(
        "! Swelling of the face or throat, wheezing, faintness after a sting or food: call 112.",
        "If they carry an adrenaline auto-injector (EpiPen), help them use it into the outer thigh, through clothes.",
        "A second dose after 5 minutes if no better and a second pen is there.",
        "Breathing difficulty: let them sit up. Faint: lie flat with legs raised.") },
    { "Wounds and bites", SBG_L(
        "Small wounds: rinse with clean water, cover with a clean dressing. Watch for redness, heat, pus, fever.",
        "Snake bite: keep still and calm, immobilise the limb, remove rings, call 112. Do not cut, suck or ice it.",
        "Tick: pull it straight out close to the skin with fine tweezers; note the date.",
        "Animal bite that broke the skin: wash 15 minutes with soap and water; a doctor soon (rabies, tetanus).") },
};

static const struct sbg_page sbg_water[] = {
    { "How much", SBG_L(
        "An adult needs about 2-3 litres a day, much more in heat or hard work.",
        "Ration sweat, not water: rest in the shade, move in the cool hours, keep your mouth closed.",
        "Without water, do not eat much: digestion uses water.",
        "! Never drink seawater, urine or alcohol: they make dehydration worse.") },
    { "Finding water", SBG_L(
        "Flowing water beats standing water. Follow valleys, animal paths and green plants downhill.",
        "Rain: catch it with a sheet, a jacket or a bag. Morning dew: wipe grass with a cloth, wring it out.",
        "Snow and ice: melt before drinking (eating snow chills you). Melt a little with some water first.",
        "Dig at the outside bend of a dry riverbed: water may seep in.") },
    { "Making it safe", SBG_L(
        "Boil: a rolling boil for 1 minute (3 minutes above 2000 m). This is the safest method.",
        "Tablets (chlorine, chlorine dioxide, iodine): exactly as the label says, including the waiting time.",
        "A filter removes bacteria and parasites, not always viruses; boil or treat as well if you can.",
        "Cloudy water: let it settle or strain it through cloth first.",
        "In the sun with no other way: clear PET bottles of clear water, 6 hours in full sun (2 days if cloudy).") },
};

static const struct sbg_page sbg_shelter[] = {
    { "Shelter", SBG_L(
        "Out of wind, rain and the ground's cold. Most body heat goes into the ground: insulate below you first.",
        "Use what is there: a rock overhang, a fallen tree, dense spruce.",
        "Avoid hollows (cold air and floods), dry riverbeds, dead trees and slopes below loose rock.",
        "Small is warm: just big enough to lie in.",
        "Debris hut: a ridge pole on a stump, sticks along both sides, then leaves or branches a forearm thick.") },
    { "Staying warm", SBG_L(
        "Layers you can open before you sweat. Wet clothes lose most of their warmth.",
        "Cover head, neck and hands. Eat and drink: the body heats with fuel.",
        "Sleep on boughs, leaves or a pack, never on bare ground or snow.",
        "Hot stones from beside a fire, wrapped, warm a sleeping place (never stones from a river: they can burst).") },
    { "Fire", SBG_L(
        "Clear ground, away from overhanging branches, sheltered from wind. Keep water or soil at hand.",
        "Gather everything first: tinder (dry grass, birch bark, fluff), kindling (pencil thin), fuel (wrist thick).",
        "Light the tinder, add kindling gently, then fuel. Fire needs air: do not smother it.",
        "This phone can light tinder: see the next page (Emergency > USB power out and a pencil lead).",
        "! Never leave a fire alone. Put it out with water and stir until cold to the touch.") },
    { "Fire from the phone", SBG_L(
        "The USB-C port gives 5 V at most 1.5 A (the charger chip's own limit). A pencil lead across a cut cable's two "
        "bare wires glows when it takes about 1.5 A: around 3.5 ohm, up to about 7.5 W in a few cm.",
        "Contacts matter most: wrap each bare wire 3-4 tight turns around the lead (or a metal paper clip pinched on "
        "it). Wires only touching spark and waste the heat at the contact instead of in the lead.",
        "Length: if the phone counts 'the chip limited the current' every second, the element conducts too well: "
        "make it LONGER or thinner. 0.3 mm lead: 2.5 cm, then 2, then 1.5. 0.5 mm: 6 cm, then 5, then 4.",
        "Best of all: nichrome wire from an old toaster or hair dryer, about 15 cm coiled. Or a few fibres of fine "
        "steel wool stretched between the wires (never a thick wad). A 0.7 mm lead is too thick to glow well.",
        "Hold it in the air (not lying on stone: that cools it). Softer leads (2B) conduct better than HB: longer.",
        "No glow but sparks? Use the sparks: hold char cloth, cotton fluff or shredded tissue right at the spark.",
        "A copper strand or foil is a short for this phone: the chip cuts it to pulses and it will not glow.",
        "! Hold only insulated parts. Work over stone or soil. Unplug any charger first. Never short the port itself.",
        "Emergency > USB power out: 10 s bursts; Extreme fire mode runs while you hold it, up to 2 minutes.") },
    { "Heat and sun", SBG_L(
        "Shade first. Cover the head and neck. Rest in the hot hours, travel at dawn and dusk.",
        "Keep clothes on: they slow sweat loss and sunburn.",
        "Sit a little above the hot ground (on a pack or branches).") },
};

static const struct sbg_page sbg_signal[] = {
    { "Be seen", SBG_L(
        "Three of anything means distress: three fires in a triangle, three whistle blasts, three flashes.",
        "Emergency > SOS: the torch and the screen flash S-O-S (... --- ...), the motor buzzes it.",
        "A signal mirror (or this phone's screen, or anything shiny) can be seen for kilometres in sunshine: "
        "aim the flash at aircraft or distant people.",
        "Bright colours in the open; move to a clearing, a ridge or a shore.",
        "Smoke by day (green leaves on a fire), flame by night.") },
    { "Ground signs", SBG_L(
        "Large (3 m or more) and in contrast with the ground: stones, logs, dug trenches, clothing.",
        "V  = we need help.   X  = we need medical help.",
        "N  = no.   Y  = yes.   An arrow = we went this way.",
        "SOS in big letters is understood everywhere.") },
    { "Sound", SBG_L(
        "A whistle carries far further than a voice and costs no energy: three blasts, pause, repeat.",
        "Answer to a distress call: two blasts.",
        "Shout at intervals, then listen: searchers call names and stop to listen too.") },
    { "Helicopters", SBG_L(
        "Stand with both arms raised in a Y: yes, we need help. One arm up, one down: no help needed.",
        "Secure loose things (tarps, clothing) and protect your eyes from the downdraft.",
        "Approach a helicopter only when the crew signals you, from the front, in their view, crouched.") },
};

static const struct sbg_page sbg_nav[] = {
    { "Directions", SBG_L(
        "The sun rises roughly in the east and sets roughly in the west; at midday it is south "
        "(north in the southern hemisphere).",
        "Emergency > Sun gives today's sunrise and sunset for your location: plan to stop an hour before dark.",
        "Shadow stick: mark a stick's shadow tip, wait 15 minutes, mark again. The line from the first mark "
        "to the second points roughly east.") },
    { "At night", SBG_L(
        "North: find the Big Dipper; the two stars at the end of its bowl point to Polaris, the North Star.",
        "South (southern hemisphere): the long axis of the Southern Cross, extended 4.5 times, points near south.",
        "The moon's lit side faces the sun: in the evening that is roughly west, before dawn roughly east.") },
    { "Walking out", SBG_L(
        "Decide once, then keep one direction: pick a far landmark, walk to it, pick the next.",
        "Water flows toward people: follow streams downhill, but go around waterfalls and cliffs.",
        "Paths, power lines and fences lead somewhere. Note every junction you take.",
        "! Never cross fast water above knee depth.") },
};

static const struct sbg_page sbg_food[] = {
    { "Food", SBG_L(
        "Food matters least: people last weeks without it. Water and warmth come first.",
        "! Never eat wild mushrooms or unknown plants: some deadly ones look like safe ones, "
        "and 'taste tests' are not safe.",
        "Eat only what you know with certainty (e.g. blueberries, lingonberries, nettles boiled).",
        "Cook any meat or fish well. Avoid fish from stagnant water.") },
    { "Weather", SBG_L(
        "Thunder: lightning reach is about 10 km. Count seconds from flash to thunder: under 30, take shelter.",
        "In the open: off ridges and summits, away from lone trees, crouch on your pack, feet together.",
        "Fast-falling cloud, rising wind, a sudden temperature drop: shelter before the weather arrives.",
        "Survival kit > Barometer: a fall of 3.5 hPa in 3 hours means worse weather within a day; 6 hPa, a storm.") },
};

static const struct sbg_page sbg_knots[] = {
    { "Knots: the basics", SBG_L(
        "Working end: the end you tie with. Standing part: the rest. Bight: a U-shaped fold. Loop: a crossed bight.",
        "Tie slowly, then dress it (strands lying neatly side by side) and pull every strand tight.",
        "Leave a tail at least 10 times the rope's thickness. Test every knot with your weight before trusting it.",
        "Eight knots cover almost everything outdoors: the next pages, one each.") },
    { "Bowline (a fixed loop)", SBG_L(
        "A loop that does not slip or jam: around a waist, a tree, a post; it unties easily even after a load.",
        "Make a small loop in the standing part, the standing part underneath ('the rabbit hole').",
        "Working end up through the hole, around behind the standing part, and back down the hole.",
        "Hold the end against the loop's side and pull the standing part to tighten.",
        "! It can shake loose when slack: tie the tail off with a half hitch. Not for climbing without training.") },
    { "Clove hitch (to a pole)", SBG_L(
        "Quick to tie and adjust: a line to a pole, a post or a tent pole.",
        "Wrap the working end around the pole, cross it over its own first turn, wrap around again,",
        "and tuck the end under that second wrap. Pull both sides: an X on the front, two turns behind.",
        "It can roll off under a sideways pull: finish with two half hitches around the standing part.") },
    { "Two half hitches", SBG_L(
        "Tie a line to a tree, a ring or a stake fast; it holds well under a steady pull.",
        "Pass the end around the tree, then around the standing part and through between them (one half hitch).",
        "Do the same again, in the same direction, next to the first. Slide both snug against the tree.") },
    { "Taut-line hitch (guylines)", SBG_L(
        "An adjustable loop that holds under load and slides when you push it: tent and tarp guylines.",
        "Pass the end around the stake. Inside the loop, wrap it twice around the standing part, toward the stake.",
        "Then once around the standing part outside those wraps (away from the stake), and tuck it under itself.",
        "Slide the knot up the line to tighten it; it grips once the line is loaded.") },
    { "Sheet bend (join two ropes)", SBG_L(
        "Joins two ropes, even of different thickness or material.",
        "Make a bight (U) in the thicker rope. Pass the thinner end up through the bight,",
        "around behind both legs of the bight, and tuck it under itself, not back through the bight.",
        "Both short ends should come out on the same side. Slippery rope: go around twice (double sheet bend).") },
    { "Figure-eight loop", SBG_L(
        "The strongest simple fixed loop, easy to check: anchoring, hauling, a loop in a rope's middle or end.",
        "Fold the end back into a bight about 50 cm long. With the doubled rope tie a figure eight:",
        "a loop, then around the standing part, then down through the first loop. Two strands side by side.",
        "It can be hard to untie after a heavy load. Leave a tail of a hand's width.") },
    { "Trucker's hitch (pull tight)", SBG_L(
        "Tightens a ridge line or ties a load down with a pulley's 3 to 1 pull.",
        "In the standing part, about an arm's length from the anchor, make a fixed loop (a slipped knot or a "
        "figure eight on a bight).",
        "Pass the end around the anchor, back up through that loop, and pull down hard.",
        "Hold it with a pinch and finish with two half hitches below the loop.") },
    { "Reef knot (bandages only)", SBG_L(
        "Right over left and under, then left over right and under: the ends come out side by side.",
        "Flat and easy to untie: for slings, bandages and bundles.",
        "! It slips or flips under load: never use it to join ropes you rely on. Use the sheet bend.") },
};

static const struct sbg_chapter sbg_chapters[] = {
    { "Start here", "Priorities, stay or go, the phone", 0xffff453aU, sbg_first, 3 },
    { "First aid", "CPR, bleeding, burns, cold, heat", 0xffff6b6bU, sbg_aid, 12 },
    { "Water", "How much, finding, making it safe", 0xff5ac8faU, sbg_water, 3 },
    { "Shelter and fire", "Warmth, shelter, fire, heat", 0xffff9f0aU, sbg_shelter, 5 },
    { "Signalling", "Be seen, ground signs, sound", 0xffffd60aU, sbg_signal, 4 },
    { "Navigation", "Sun, stars, walking out", 0xff30d158U, sbg_nav, 3 },
    { "Food and weather", "What not to eat, storms", 0xffbf5af2U, sbg_food, 2 },
    { "Knots", "Bowline, hitches, joining ropes", 0xff64d2ffU, sbg_knots, 9 },   /* Track D 2026-11-16 */
};
#define SBG_NCH ((int)(sizeof(sbg_chapters) / sizeof(sbg_chapters[0])))

#endif
