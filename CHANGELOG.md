# Changelog

## v3.2.0 — 2026-08-25

### The gauge has a channel now

A score of 12 used to draw a lone green stub at the bottom-left of the face,
floating in black, and it read as a rendering fault rather than as a small
number. The track behind it was 6 px of near-black under a 14 px bright arc —
so there was nothing for the reading to be a *fraction of*.

The track is the same weight as the score now, and lifted out of the shadows.
`QUIET 12 / 96` finally looks like twelve out of ninety-six.

### Nothing sits on top of the gauge any more

`PR_SAFE_R` is where the *glass* stops. On a face with a score arc, the arc
stops sooner — and everything was being fitted against the glass. The four
evidence chips were laid out to ±190 px while the arc crosses their row at
±178, so the outer two were drawn on top of it.

There is a second working radius now, **`PS_INNER_R`**, derived from the ring's
own geometry rather than typed in again, and every horizontal budget on a gauge
page measures against it.

### The words that mattered were the ones getting cut

Fitting to the gauge made the `why` line narrower, which exposed something
worse: its vocabulary never fitted in the first place. `pw_forgery_name()` was
returning 25–31 characters into a 24-character band, and the cut landed on
exactly the word carrying the meaning —

> `sequence counter went backwards` → **`sequence counter went.`**

Truncation is the right *behaviour* for an unbounded string and the wrong
*outcome* for a fixed vocabulary somebody chose. So the vocabulary was
shortened — `counter went backwards`, `signal level is wrong`,
`unprotected on MFP net`, `source never beaconed` — and a host test now walks
the whole table.

### Polish

- **A smoother aura.** Seven layers instead of five; five still stepped
  visibly across a 300 px disc.
- **The ribbon flows.** Older seconds are dimmer, so the sixteen-second
  timeline has a direction and the bright end is *now*.
- **Lit chips get a hairline edge** as well as a fill — fill alone is
  ambiguous against a coloured aura.
- **The boot screen earns its second and a half:** the ring sweeps closed once,
  green for a clean fence, red if it could not be proven.
- **Each tool gets its own rim** on the browse card, so twenty-one tools have
  an identity you register before reading the name.
- **The receive-only pip breathes.** *"Is it frozen?"* was a real question
  asked of the old face, because a quiet room and a crashed device look
  identical. Ten pixels, four seconds, and it never touches the reading.

7883 checks. Fence, lens, display and sources audits pass. `missed=1` across
500 paints on hardware — a single miss during the splash sweep, not a cost.

## v3.1.1 — 2026-08-25

### A row's value could still become two lines

Reported from a photograph of the PROBE page: the grade `unclassified`
rendered as `unclassifi` with a lone `e` floating outside its card.

Two faults compounding, and both are fixed.

**The label wrapped.** `LV_LABEL_LONG_DOT` is documented as "break the text and
write dots in the last line" — and *break* means **wrap**. With the height left
to grow, an over-long value simply became two lines and dotted the second. Both
row columns are pinned to a single line height now, which leaves `LONG_DOT` no
room to wrap into, so it truncates horizontally as intended. It also restores
the rule that a cell's height can never change, and therefore can never move
the invalidated region.

**Three strings never fitted in the first place.** `struct pharos_lens_row`
carries `right[12]` — eleven printable characters — and `pp_place_name()` was
returning `shop or cafe` (12), `carrier hotspot` (15) and `unclassified` (12),
so `snprintf` silently cut them on the way *into* the row before the UI ever
saw them. They are now `shop/cafe`, `carrier` and `unknown`, and a host test
walks the **whole table** against the contract — because the next place name
somebody adds will be long too, and nothing else would have noticed.

### Which column loses, when one has to

The narrowest card is the bottom one, where the chord has shrunk toward the
rim, and it has about 310 px for both columns. The **value** takes 112 of it
and is never cut: it is the judgement, and a grade that reads `A` when it meant
`A+` is a wrong answer, not a cosmetic loss. The **label** is the column that
degrades — 19 characters, which covers every row the firmware currently
produces, with an honest ellipsis beyond that.

That trade is now an assertion rather than an accident.

7875 checks.

## v3.1.0 — 2026-08-25

### The guide: the device teaches itself

Twenty-one tools, four verdict colours, and a touch surface with no visible
buttons. The controls were explained in the console banner — which is no help
at all to somebody holding the device and looking at the *glass*.

So the glass explains itself now. Nine steps on first boot, remembered in NVS,
replayable forever with the new **`guide`** command, and left in a single press
by anybody who already knows the device.

Each step **shows the gesture rather than describing it**: the outline of a
zone appears and a fingertip pulses inside it, travelling between the two side
zones for the step that has two. You copy what you just watched instead of
skimming a sentence about it.

The order is deliberate — controls first, because you cannot explore without
them; then the colour key, because it is the key to every other screen; then
the two pieces of vocabulary that are genuinely unusual: the ring of dots and
the evidence chips.

### Every detail row was losing its first word

Reported from a photograph of the Rival page, and it affected **every lens**:

| shown on hardware | should have read |
|---|---|
| `p models / advs` | `popup models / advs` |
| `resses used` | `addresses used` |
| `ed senders` | `faked senders` |

The row labels were children of the *page*, positioned with
`lv_obj_align(LV_ALIGN_CENTER, -w/2 + 18, dy)`. That aligns a label's bounding
**box centre** at that x — and an auto-sized label grows symmetrically about
it, so half of any long string extended further left, off its card and off the
glass. `LV_TEXT_ALIGN_LEFT` does not help: it aligns lines *within* the box and
says nothing about where the box sits.

Rows are children of their **card** now, with explicit widths, so text is
clipped by the card and cannot reach the panel edge however long it gets.
`LONG_DOT` ellipsises the few labels that genuinely do not fit — a legible
failure instead of a silent amputation.

The column budget is taken from the **narrowest** card, the bottom one, where
the chord has shrunk toward the rim. A budget measured on the middle rows
leaves the last row two characters short; a host test asserts this and found
exactly that.

### Layout is still arithmetic

The first draft of the guide put its body text at +132/+162 and its hint at
+198 — fine offsets on a rectangle, hopeless on a circle, where the chord at
+198 holds about thirteen characters. `tools/render` caught **ten escaped
primitives** before any of it reached the device. The guide's bands are named
tokens now, with host tests behind them.

7851 checks. Display, sources, fence and lens audits pass.

## v3.0.0 — 2026-08-25

### LUMEN: the face, rewritten from scratch

The old face was an aircraft-instrument pastiche — a 24-tick bezel, hairline
arcs, text down to 12 px. It photographed well and read badly in a hand, which
is the only place it is ever used. It is gone.

**Nothing on the glass is under 14 px now.** At 466 px across a 1.75 inch
circle this panel is 266 ppi, so the old 12 px body text was about 1.1 mm of
cap height — legible in a screenshot, mush at arm's length. Half the supporting
text was set in it. Montserrat 28/32/36 were added to the build because the old
scale jumped 26 → 48 with nothing usable in between, which forced every verdict
*word* down to 26 while numbers sat at 48.

**Every screen now reads in one order:** colour → word → number → evidence →
action. A soft aura behind the centre carries the verdict before you have read
a single glyph, so a glance from across a room gets the answer and can stop.
The number is now set *smaller* than the word it explains — the old face had
that exactly backwards, which is why it read as a number generator rather than
as a judgement.

**The evidence chips are named.** `RATE` `SHAPE` `FORGE` `AFTER`, not four
anonymous dots. "Why does it think so" is answerable from the glass instead of
from the manual.

### The home ring lost its labels, and that is the fix

Fourteen names never fitted a 466 px circle at any radius; twenty-one lenses
made it worse. Names were being drawn through the headline, behind a rule about
which ones were "worth naming". The names were never the point: you answer *is
anything wrong* by counting the dots that are not green. Which watch it is
matters only once you have decided to look, and then it is one name in the
middle — always legible, because there is only ever one of it.

Removing the labels removed the clipping bug, the capacity rule and the
staggered-radius machinery in one go. The dots now sit along the gauge's own
270°, leaving the bottom notch for the clock.

### Measured on the device

| | before | after |
|---|---|---|
| frames | `painted=257 missed=20` (~7% dropped) | `painted=983 missed=0` |
| internal DMA RAM free | 25 KB | **59 KB** |
| `esp_lv_adapter_lock` failures | repeated, every boot | none |

Twenty-four bezel ticks that meant nothing, and sixteen curved `lv_line` bars
whose bounding boxes each spanned most of the panel, were being redrawn
underneath everything that *did* mean something. LUMEN draws nothing that does
not carry information.

### Two real bugs this exposed

**Locate painted the glass red as you walked toward your target.** Its score is
*closeness*, not threat, but it never set `has_alert`, so the threat palette
was applied to it — "found it" rendered as "danger". Harmless behind a thin
arc; not behind an aura.

**Advice was being cut to its first sentence**, throwing away the half that
says what to *do*: "Broad, spoofed deauth." survived, "Preserve the log." did
not. Advice now runs to two fixed lines.

### The parts that did not change

The four verdict colours are frozen and asserted byte-for-byte in
`test_style.c`. A red on Census means what a red on Watch means; that contract
outranks any visual refresh. The transmit fence, the confidence ceilings and
every detection engine are untouched.

Layout is arithmetic, not opinion: `pharos_style.c` is free of LVGL and
ESP-IDF, so the type scale, the chip row and the card stack are all proved on a
laptop — **514 new checks, 7831 total**. `tools/render` now draws the LUMEN
face from the same tokens and bounds-checks every primitive against the safe
radius; it caught five escapes on the first pass, four of them real engine
advice lines that would have run off the edge of the glass.

Fence and lens audits pass. Verified on hardware.

## v2.3.0 — 2026-08-20

### No screen shows invented data any more

Two lenses wore a SIMULATION banner. The banner was honest — they played
synthesised attacks through the real engines — but a device with two screens
showing made-up numbers is a device somebody learns to distrust, and it was
reported exactly that way.

**Footprint now measures the room.** It holds a receiver, grades what is
actually in the air, and reports what a defender would have seen: the grade,
the family that gives it away, and whether a defender who merely *hops* would
notice at all. Run your own tooling and point this at it. Both postures come
from the *same* captured frames — that is what makes the comparison mean
something rather than being two separate guesses.

Quiet air is now reported as quiet air. The old version played a scenario
whenever it had nothing, which is how a lens ends up showing test data forever.
The drill is still there for anyone who wants it — asked for explicitly, and
labelled.

**Range became Ward**, which guards one network instead of replaying invented
ones.

### Ward: watch the network you are responsible for

Every other radio lens grades the whole room, which is right for a sweep and
wrong for the commonest defensive job there is. Watch will happily report a
deauthentication flood — against the café downstairs. Nothing could be told
*"this one is mine."*

Ward camps on your network's channel and feeds the engine **only frames
belonging to it**. Measured on hardware guarding a real network: 43 of its
frames graded, 196 from everything else ignored, and a confidence ceiling of
**96** — against about 59 for a hopping receiver. Camping on one network buys
real certainty, and that is the whole point.

Two things it refuses to say:

- **No target is not a clean bill of health.** Until you choose a network it
  grades nothing rather than quietly watching everything and letting you
  believe the reading is about yours.
- **Out of range is not quiet.** If it cannot hear your network it says so.
  Reporting "nothing is happening" about a network the receiver cannot hear
  would be the most dangerous sentence this lens could produce.

### Motion sensing, and what it is for

The board carries a QMI8658 six-axis IMU that the vendor BSP declares absent.
There is one question it answers that nothing else here can, and it is Vigil's.

Vigil asks whether a tracker is travelling *with you*. Seeing a tracker means
nothing — a café at lunchtime has a dozen. With no GPS it inferred movement
from Wi-Fi locale turnover, which is holed both ways: routers reboot and access
points switch off, so the locale turns over while somebody sat still and a
tracker that was merely nearby starts looking like a stalker; and in a car park
or a rural lay-by there is no Wi-Fi to turn over at all, so it went quiet
exactly where a planted tracker matters most.

An accelerometer is wrong in completely different circumstances. The strongest
thing it does is **refuse**: if you did not move, "it followed you" cannot be
concluded whatever the locales did, so the ceiling drops and the verdict stops
short of FOLLOWING. The sightings are still reported — what changes is what may
be drawn from them. On a board with no IMU, movement is unknown, and unknown is
never a veto.

It reports still / walking / moving and counts steps, and deliberately offers
no distance: double-integrating acceleration is a well-known way to produce
confident nonsense.

Two bugs the hardware found, both caught by self-checks rather than luck:

- the full-scale range was ±8 g while the scale factor assumed ±4 g, so gravity
  measured 473 mg instead of 1000. Nothing crashed — the swing thresholds
  simply became twice as hard to reach, and walking would have gone quietly
  undetected. The probe now requires gravity to read as gravity.
- the part came up in whatever state it was left in, so the configuration
  landed on top of it and every axis railed at full scale. A soft reset first,
  and the control registers are read back afterwards — a write that silently
  did not land looks identical to one that did, until the data is nonsense.

Verified: 955 mg at rest, reports still, refuses to judge.

### The ring stopped crying wolf

A photograph showed "something is up" in orange because Spectrum scored 66 —
which is how busy the channel is. A working router was setting off the alarm.
Five sensors whose scores are not threat scales now say so themselves:
Spectrum and Sentinel cannot raise an alarm at all, Squall alarms only on real
denial rather than a congested building, Vigil only when something is
*following* rather than merely present, and Whisper only on a persistent beacon
— there is always something at 19 kHz.

### Layout is arithmetic, not photography

Thirteen labels round the dial were cramped, and the only check had been
looking at it. The geometry is now computed and the spacing pinned at every
count the operator can configure: eight watches clear each other by 38 px, ten
by 21, twelve by 12, thirteen by 10 — which is where two names start reading as
one word. Ten ship armed for that reason; all of them stay one tap away in the
Ring lens.

Also: the 225 px "tap the middle for the full picture" ran straight through
three labels and now lives inside the core, and the middle no longer shows a
bare "SPECTRUM" that read as a stray fourteenth label.

## v2.2.0 — 2026-08-20

### The Survey: what this place is actually like

The question most people pick this device up to ask was the one nothing
answered. Aegis remembers *attacks*; every other lens reported the present
tense and forgot — and with the watches on a rotation, "the present tense" is a
five-second glance every forty seconds. So Census would grade twenty-three
networks, find the ones that would drop their clients under a flood, and lose
all of it the moment the radio moved on.

The Survey is where that goes. It accumulates over the whole session,
deduplicated by address, and says it in words rather than jargon:

    4 networks seen here
    2 drop clients if hit
    1 runs WPA3 or OWE

Tap the middle of the home ring to open it, or type `survey` on the console.

Its rules are host-tested, because this is the screen most able to frighten
somebody for no reason:

- **It counts what it saw**, never an estimate scaled up for coverage.
- **A weak neighbour is not an attack** and is never worded as one — there is a
  test that asserts the word "attack" appears in none of these lines.
- **The good news is reported too.** A device that only ever lists faults
  teaches its operator that everything is always bad.
- **A full table says so** rather than quietly under-counting a busy place.
- **The words match the numbers.** It said "most networks are floodable" when
  one in four was; "most" now means most.

### Thirteen sensors on the ring, and the fast ones stayed fast

Arming every observer uniformly would have made a sixty-five second lap — so
the deauthentication detector would be deaf for a minute at a stretch in order
to re-count the same access points for the fourth time.

Each watch now says how often it needs the radio. An event lasts seconds and is
missed if you are elsewhere; a standing fact changes over minutes. Watch, Karma,
Mirage, Harvest, Twin and Rival run every lap; Census, Probe, Squall, Vigil and
Whisper every other; Sentinel and Spectrum every third. Adding more surveys now
costs the event detectors nothing, and freshness scales with each watch's own
period so a survey arriving exactly on schedule is not called stale for it.

The ring lays itself out for however many watches actually armed, instead of
for a fixed twelve.

### Fixed: the greys were not grey

The panel is RGB565, and the rounding lands differently on each channel. The
Mono theme — whose entire point is having no hue at all — was rendering
`0xD8D8D8` as `#dedbde` (magenta) and `0x161616` as `#101410` (green). A grey
theme was quietly the only themed thing on the screen.

Every palette value now sits on the 565 lattice, so what is written is
bit-for-bit what the panel emits, and a future colour that does not is a test
failure rather than a faint tint nobody can name.

### Fixed: choosing a lens by hand and being ignored

Only the home ring paused the rotation. Every other way of picking a lens — the
browser, or typing `census` on the console — was overruled within a second as
the rotation took the radio back. It looked like the device ignoring you. The
rule now lives at the single point every path goes through.

### Fixed: "all quiet" while not watching

With the rotation paused, no watch reports, so the summary correctly found
nothing to worry about and said "all quiet". True, and the most dangerous
sentence this screen could show: the room is quiet because nobody is listening
to it. It now says NOT WATCHING.

### Fixed: a sentence cut in half

"2 would drop clients if flooded" reached the glass as "2 would drop clients if
f". The test that should have caught it allowed thirty-three characters against
a column that holds twenty-five, and the slack hid a live truncation. Every
line is now written and checked at the longest count that can occur.

### Smoother, without starving the detector

Five repaints a second is what a stepping gauge looks like. Pushing it to
fifteen was measurably the wrong fix — the loop's own period went from 50 ms to
93 ms, starving the analytics tick that does the actual detecting, to make a
gauge look nicer.

The arcs are animated by LVGL instead, interpolating between readings at the
panel's own refresh rate. Smoother than the fast poll was, and cheaper than the
slow one.

### Also

- A visible way back: every page now carries a line saying how to reach the
  home ring, because there was none and "how do I get to the home screen" is not
  a question anybody should have to ask.
- Survey feeds are throttled to once a second; they were walking whole tables
  ten times a second to push facts that were already deduplicated.
- Only an ALARM holds the radio now. "Above background" let the microphone
  watch — which sits at ELEVATED in any room with something at 19 kHz — hold
  its slice every lap and stretch the whole rotation.

## v2.1.0 — 2026-08-20

### The Watchtower: every watch on one screen

Asked for directly: "I want all the sensors on and everything shown on the home
screen, so the person doesn't have to be in a specific sensor to know if the
attack is happening."

The naive reading of that is impossible. There is one 2.4 GHz receiver and one
BLE controller in this device; six detectors cannot listen at once, and a home
screen with six green dots on it would be claiming they do.

So the watches take turns. Eight of them, five seconds each — a forty-second
lap — and the ring keeps what each one last found. That is strictly better than
switching lenses by hand: it never forgets and it never gets bored.

It is also a duty cycle, and the whole design is about not hiding that:

- **A reading has an age.** A live dot is filled, an ageing one is dimmed, an
  expired one is drawn hollow — an outline with nothing in it, which is exactly
  what the device knows about that watch right now.
- **Freshness is counted in rotations, not seconds.** Arming another watch
  genuinely slows every other watch down, so the window widens to match instead
  of quietly turning honest dots into lying ones.
- **Not having looked is not the same as having found nothing.** They are
  separate states, counted separately, and "all quiet" is only ever said when
  every armed watch has actually reported inside its own window.
- **The confidence ceilings already knew.** Every engine scores against how much
  of the channel it actually heard, so a watch on a duty cycle earns a lower
  ceiling by itself. The rotation did not need a new honesty mechanism; it
  needed the existing one left alone.

Touch a dot to open that watch — one press, and the rotation pauses, because
somebody who chose Watch did not ask to be moved off it five seconds later.
Long-press back to the ring and it resumes.

### Fixed: the home screen and the sensor drew on top of each other

Reported as "when I clicked on the sensor the UI of the home screen and the
sensor were merging". Exactly what it was.

Each page function hid the others by name, so every new page had to be added to
every existing function. The home ring was added and three of those lists were
not updated. There is now one list and it is derived — naming the page you want
hides every other page by construction — plus an audit that fails the build if
anything sets page visibility outside that one function.

### Fixed: one noisy watch could starve the entire ring

Found on the first run on hardware. A watch that is hearing something keeps the
radio, which is right — walking away from an attack in progress to keep a rota
tidy would be the worst possible moment to leave. But the hold was unbounded,
and the microphone watch sits at ELEVATED in any ordinary room because there is
always something at 19 kHz. It took the radio and never gave it back: eleven of
twelve watches had never run, and the ring was eleven hollow dots.

A hold may now extend a slice; it may not abolish one.

### Fixed: the home screen shouted ALERT about the neighbours

Census scores how badly configured the networks *around* you are. A neighbour
with WPS switched on scores 71, and the ring read that as an attack. A lens
whose score is not a threat scale now says so, rather than the ring guessing
from the number.

### Fixed: a mistyped watch name silently shortened the ring

`rf.rival` was written `ble.rival`. It did not fail to build and did not crash —
the ring simply carried seven dots instead of eight, which is indistinguishable
from a design decision. Now it is a build failure and a logged error.

### Fixed: the detail rows were too small to press

Reported as "it is very small so sometimes I am not able to click them
correctly". Six rows at a 36 px pitch, on a 466 px circle across 1.75 inches —
266 ppi, so 36 px is 3.4 mm, against a fingertip of about 9 mm. The rows were
legible and untappable.

Four rows at 58 px (5.5 mm), each with a touch target spanning the full width of
the glass, so only the vertical dimension has to be right. Touching a row now
acts on it directly — one press instead of stepping a cursor with the side zones
and then pressing the centre. The focus pill under the finger is what says the
press registered.

Instrumented before changing anything: every press is logged with its
coordinates, and thirty seconds of an untouched device produced zero events. The
touch controller was fine; the targets were too small.

### Fixed: a build without the vendor BSP could not compile

A bulk edit had matched in both halves of `pharos_hud.c`, pasting the whole LVGL
implementation into the no-panel branch — which then defined one function twice
and referenced widgets that do not exist there. Nobody builds that branch, which
is exactly how it survived. Audited now: every HUD entry point must be defined
once per branch.

## v2.0.1 — 2026-08-19

### Fixed: hardware took half a minute to disappear

Reported as "it took some time to remove the Flipper Zero after I closed it".
It did — thirty seconds, because that was the window every device got.

Thirty seconds is the right patience for the quietest thing this engine can
see: a beacon heard once every ten seconds needs it, or it flickers in and out
of the list forever. It was being spent on a Flipper advertising twice a
second, where five seconds of silence is already eight missed advertisements.

**How long silence has to last before it means something is not a constant.**
It is a property of the device. The window now comes from the cadence actually
observed, and the old constant becomes the *ceiling* — for something heard so
rarely that no cadence can be measured. A Flipper heard twice a second is
dropped about five seconds after it stops; a Pwnagotchi beaconing every ten
seconds still gets the full thirty.

And the wait is legible: a device past a third of its window reports how long
since it was last heard and dims while it says it, on both the row and the live
face. The wait reads as a countdown rather than as the screen being wrong.

### Nicer

A short accent rule under the detail-page title — the header stacks three
things (lens name, column headings, list) and without a line between them the
eye reads the headings as the first row.

---

## v2.0.0 — 2026-08-19

Everything below the v2.0.0-watch heading, plus the work that came out of
using it: a screen you can actually operate with a finger, settings you can
change on the device, and three detectors that were quietly missing things.

### Fixed: the detail rows were too small to press

Reported as "sometimes I am not able to click them correctly". Every press is
now logged with its coordinates, and thirty seconds of an idle device produced
zero phantom touches — so it was not the controller, it was the geometry.

The rows were on a 36 px pitch. The panel is 466 px across a 1.75 inch circle
(266 ppi), so that is **3.4 mm**, against a fingertip contact patch of about
9 mm. Legible, and physically unresolvable.

- **Four rows on a 58 px pitch** (5.5 mm), each target spanning the full width
  of the glass so only the vertical dimension has to be got right.
- **The row is the control.** Touching it edits or opens it. Reaching the
  volume was previously four presses — step a cursor with the side zones, then
  press the centre; it is now one, and it lands under the finger rather than
  wherever a counter had reached.
- **The page controls are the two largest targets on the device** — the whole
  band above and below the list, ~117 px each, each marked with a chevron.
- **The zones partition the glass.** The old layout left a 13 px dead lane
  either side of the centre column, and a press that lands in a gap does
  nothing, which reads from the outside as an unreliable touchscreen.
- Focus is a filled pill rather than a brightened hairline — the same shape as
  the target, so the thing you see and the thing you press are one object.

### New: settings you can change, and a face you can choose

The System page listed the alarm, the region and the rotation and let you
change exactly one of them, by cycling blind through four volumes. Everything
else was a console command over USB, which is not a thing anybody does while
holding a round screen in a corridor.

Theme, brightness, alarm, speaker volume and region are now editable in place,
each remembered across boots. Opening a setting lists every value it can take
and marks the one in force, so a cycling control stops being blind.

Five themes, each with a reason to exist rather than another hue: **Beacon**
(cyan), **Abyss** (blue), **Violet**, **Mono** (no hue at all, so a verdict is
the only colour on the glass) and **Nightwatch** (dim enough to read without
lighting your face). A theme changes the *chrome* and never the verdict
colours — green, amber, orange and red are fixed in every theme, because a
palette that could restyle danger is a way to make the device lie quietly. A
host test fails the build if a new accent lands near a warning hue.

### New: rows that open

A grade nobody can interrogate is a claim, not a finding. Touching a network in
Census opens its own evidence — BSSID, channel, signal, authentication, 802.11w
posture, cipher, WPS, whether the name is hidden, beacons heard, and the single
ceiling that stopped the grade going higher stated as **fix first**.

### Fixed: BLE spam the diversity test could not see

The pairing-popup test measured payload **diversity**, which is right for the
tools that cycle popups and blind to the ones that do not. Several pick the one
dialog that is most annoying on the target and repeat it as fast as the radio
allows: one model code, hundreds of advertisements. Diversity counted one, the
raw-rate note only fires above 60/s, and a steady 20/s single-payload flood fell
between them with **no finding at all**.

The signal that catches it is identity, not payload: real accessories keep a BLE
address for minutes, while these tools draw a fresh random one per advertisement
so a phone cannot dismiss them permanently. Eight distinct addresses carrying
pairing payloads in four seconds is not a room with accessories in it.

**Samsung EasySetup** (company `0x0075`) was not parsed at all — the family the
Android-facing tools reach for. **Microsoft** was the opposite problem: company
`0x0006` alone is every Microsoft-adjacent device announcing something
unrelated, so it is narrowed to the Swift Pair beacon ID. Apple's Nearby Info
and offline-finding types stay out deliberately: every iPhone and every AirTag
broadcasts those continuously, and a detector that cries wolf in a foyer is one
nobody reads in a corridor.

### Fixed: a renamed Pineapple was invisible

Detection was a match on the word "pineapple" in the network name, which
survives about as long as it takes somebody to change it — and changing it is
step one of using one. Hak5's registered OUI (`00:13:37`) is checked before the
name. Presence stays capped: a Pineapple on a shelf is a Pineapple on a shelf.

### Fixed: hardware that was switched off stayed on the screen

Reported as "I turned off my Flipper Zero but it still shows one detected". The
list had been made stale-aware; the count above it had not, so the screen read
`hardware identified: 1` over an empty list. A screen that contradicts itself is
worse than either half alone — the operator has to pick which line to believe
and has nothing to pick with. The test asserts the invariant that matters: not
that the count expires, but that the count and the list agree at every instant.

### Fixed: a rate built out of division

Mirage reported "261.5 new names a minute" on a quiet street. Three names
arriving 800 ms apart narrowed the time base to 800 ms, and the duty correction
multiplied by the reciprocal of a barely-measured dwell. Three floors now guard
the denominator, and each sets `PF_NOTE_SHORT` so a held-back reading says so.

### Fixed: a drill could be mistaken for an incident

Range showed `FLOOD LIKELY 77` and Footprint `BLARING 77` while playing
synthesised scenarios through the real engines — the same words the field uses,
in a quiet building. Training lenses now mark every frame as a simulation, and
both faces label whose number each figure is (`to a defender who camps`,
`if real: deauth flood`) instead of presenting bare numbers.

### Fixed: the microphone was never deaf

`esp_codec_dev_read` returns a status, not a byte count. Treating `0` as "no
samples" discarded every successful read, so Whisper reported `MIC SILENT` on a
working microphone.

### Fixed: the no-panel build had not compiled for a long time

`pharos_hud.c`'s stub branch had acquired a verbatim copy of the LVGL
implementation of `pharos_hud_detail` — defined twice, referencing widgets that
do not exist there. Nobody builds that branch, which is how it survived.
`tools/check_sources.sh` now counts the definitions of every HUD entry point.

### Fixed: Census could attribute one network's failings to another

The expansion was keyed on a list index, and that list re-sorts as the air
changes. A page could begin describing one network and finish describing
another with nothing on the glass to say so. It now latches the record on its
first line.

---

## v2.0.0-watch — 2026-08-18

The deauthentication detector rewritten end to end: the engine, its screen, and
the checks that were supposed to be guarding both.

### Fixed: the detector could not alarm in the posture it ships in

v1's arithmetic was sound and its tests passed, and in the field it was useless.
Hopping 13 channels put the confidence ceiling at ~60; the identity family
needed beacons the receiver was rarely on-channel to hear; so at most two
families could fire, two families capped at 74, and 74 was above the ceiling
anyway. `FLOOD LIKELY` was arithmetically unreachable, and nothing on the glass
let the operator change posture.

The fix is not a lower bar. The ceiling was punishing the wrong thing. Hopping
weakens an **extrapolated rate** — you heard 7% of a second and multiplied by
fourteen. It does not weaken a **contradiction**. An unprotected deauth claiming
to come from a network that *requires* protected management frames is forged,
and hearing it during a 200 ms visit makes it no less forged. Such a verdict now
raises its own ceiling to 88 and may alarm while hopping. Nothing reaches 100.

### New: four evidence families instead of three

| Family | Max | What it reads |
|---|---|---|
| `RATE` | 34 | Duty-corrected rate plus the peak second, anchored on Kismet's DEAUTHFLOOD thresholds (5/min, 2/sec burst) |
| `SHAPE` | 22 | Broadcast share, victim spread, and per-victim burst runs — no AP sends sixteen frames to one client in half a second |
| `FORGERY` | 30 | 802.11w contradiction, sequence-**order** violation, signal level against the beacon's own spread, ghost BSSID |
| `AFTERMATH` | 18 | The reassociation stampede that follows a disconnect burst — the one test that still works on the low-volume targeted attacks volume thresholds miss |

The sequence test checks **order, not gap size**, and that is the point: frames
the receiver never heard widen gaps but can never reverse them, so it does not
inherit the false-positive rate the literature reports for gap thresholds. It is
bounded by the access point's own measured counter rate, so a busy AP is never
accused. Both negatives are asserted in the tests.

Reaching the alarm band needs three families, which — with only two of the four
being volume-shaped — necessarily includes forgery or aftermath. **A busy network
is not an attack**, asserted as an invariant rather than as arithmetic.

### New: the operator can finally stand still

The way to raise the ceiling is to stop hopping, and no control on the glass did
it — the centre tap was inert while a lens was running, and camping needed a
console command over USB. Lenses may now claim the centre tap; Watch camps on
the channel carrying the traffic. It also locks on by itself for twenty seconds
when the rate family fires, then releases. The operator's tap always wins.

### Fixed: the screen flickered and broke

Four distinct causes, all in `pharos_hud.c`:

1. Neither the screen nor the touch zones ever had `LV_OBJ_FLAG_SCROLLABLE`
   removed, and `lv_obj_create()` sets it. Content is laid out past the screen's
   bounds, so a drag — or a smeared tap, which on round glass is most of them —
   **scrolled the whole face away with no way back**. That is the "breaks" half.
2. `pharos_hud_live()` hid the summary label and `pharos_hud_advice()`, called
   immediately after it in the same repaint, showed it again. Every frame.
3. `lv_label_set_text()` marks a label dirty whether or not the string differs,
   so eight labels and three arcs were rewritten 5×/second with identical
   content, under a 210 px opaque disc that then had to be recomposited.
4. The advice label wrapped, so its height changed with its text and the
   invalidated region moved around under the arc.

Now: two page containers switched only on an actual view change, every widget
written through a dirty check, no wrapping text on the live face, and scrolling
removed everywhere. **A steady reading invalidates nothing at all.** The
three-call API is one call, because that is what let the calls disagree.

### New: the face answers four questions instead of one

One ring for how bad, a sixteen-second ribbon for what shape over time (a steady
trickle and one violent burst have the same ten-second mean and are not the same
event), four **labelled** pips — `RATE` `SHAPE` `FORGE` `AFTER` — for what the
evidence is, and one unwrapped line for what to do. The ceiling is a tick across
the arc rather than a second band of colour competing with the score.

### Fixed: three faults in the audits themselves

- **The fence's ELF stage has never run.** `check_tx_fence.sh` read the ELF path
  from `$1`, fifty lines after `set -- $BLE_VALS` reassigned the positional
  parameters. `$1` was the string `y`, `[ -f y ]` was false, and the linked-image
  audit skipped silently. That stage is cited in the README and CI runs it on
  every build.
- **And when it ran, it condemned a clean image**, demanding a `__wrap_` trap for
  every transmit primitive when their *absence* is the stronger result: `--wrap`
  only rewrites references, so an uncalled primitive is never linked at all. It
  now looks for the real breach — primitive present, trap absent — with a
  positive control so it cannot pass by proving nothing.
- **`pipefail` + `strings | grep -q` reported present lenses as discarded.**
  `grep -q` exits on first match, the producer dies of SIGPIPE (141), `pipefail`
  promotes it. Timing-dependent: it passed in CI and failed locally on the same
  commit. Both sites now match from a here-string.

None of this could produce a false *pass* — `pipefail` only makes a status more
non-zero, and the fence was intact throughout. But an audit that skips itself,
and one that fails on correct output, are worth less than no audit, because they
are believed.

### Verification

5,602 host checks · 0 render bounds violations · all four audits green against
the linked ELF · ESP-IDF v5.5 build clean with zero warnings · 49% of the app
partition free.

*Never tested against a live deauthentication attack on real hardware.*

## v1.9.0 — 2026-08-14

### Fixed: changing lens rebooted the device

The touch callback called `pharos_lens_activate()` directly. That callback runs
on **LVGL's task**, holding LVGL's lock and using LVGL's stack — and activating
a lens tears down and restarts the Wi-Fi driver, which needs far more stack than
that task has, then re-enters the display lock. Stack overflow, panic, reboot,
every single time.

The callback now records an *intent* and returns; the UI task, which already
owns the lens lifecycle, applies it on the next tick. The rule is written into
`pharos_hud.h` so it does not get rediscovered the same way.

### New: the device explains itself

*"I don't know what this device is doing"* was fair. It showed a number and a
word, and the only way to learn what a lens did was to read the README on a
laptop. There are now two views:

**BROWSE** — one lens at a time: its name, **what it actually does in plain
words**, which team it serves, and where you are in the list (`3 / 16`). Nothing
runs. The device boots into this, because the first thing an operator should see
is what the tool is *for*, not an unexplained reading.

**LIVE** — that lens running: gauge, headline value, band word, detail line.

| gesture | action |
|---|---|
| tap **left** / **right** | step through the lenses |
| tap **centre** | start the one you are reading about |
| **long-press** | stop, and go back to browsing |

Stepping the list stops whatever was running, so the number on screen always
belongs to the lens named above it. Lenses are colour-coded by kind, and widgets
with nothing honest to say in BROWSE are hidden rather than left showing stale
text.

## v1.8.0 — 2026-08-14

**Drivable from the glass.** Touch navigation (`BSP_CAPS_TOUCH` is 1, so the
CST9217 was already registered with LVGL and simply unused), the BOOT button as
a physical fallback (`BSP_CAPS_BUTTONS` is 0 — the two side buttons are RESET
and BOOT/GPIO0), and `rotate <0|90|180|270>`, persisted in NVS, because which
way is up on a *round* device depends on how you hold it.

## v1.7.1 — 2026-08-14

### Fixed: the black screen. The actual, embarrassing cause.

Every display fix from v1.4.0 to v1.7.0 was written into a branch **that was
never compiled**.

`pharos_bsp.c` chooses between the real panel and a simulated board with:

```c
#elif !defined(CONFIG_PHAROS_HAS_VENDOR_BSP)
```

ESP-IDF does **not** force-include `sdkconfig.h`. A translation unit only sees
`CONFIG_*` macros once something has included it — and in this file every ESP
header was included *inside* the branches, i.e. **after** the test had already
been evaluated. So at that moment the macro was undefined, `!defined(...)` was
true, and the **simulated board path was compiled every single time**.

`sdkconfig.defaults` said `=y`. The build honoured it everywhere except the one
file where it mattered. The firmware compiled cleanly, linked cleanly, passed
the fence and lens audits, booted happily, ran all sixteen lenses — and never
once touched the panel. `pharos_hud.c` had the identical bug, which is why the
HUD was never built either.

The device's own diagnostics are what finally caught it:

```
W (534) bsp: vendor BSP disabled - simulated board, screen will stay dark
     ui: active: wifi.spectrum painted=0 missed=348 hud=0
   diag: display : not attempted (vendor BSP off)
```

The fix is one `#include "sdkconfig.h"` at the top of each file.

### And a guard, because this class of bug has now cost days

`tools/check_display.sh` runs in CI and on every release build. It asserts two
things: that `sdkconfig.h` is included **before** any preprocessor test of a
`CONFIG_` macro, and that the linked ELF contains the real bring-up while
**not** containing the simulated path's fingerprint. A release that would boot
to a black screen now fails the build instead of shipping.

This is the same disease as the lens-linkage bug guarded by `check_lenses.sh`:
code that compiles, links, tests green, and is silently absent from the running
firmware. It gets the same cure — an audit against the actual artefact.

## v1.7.0 — 2026-08-14

**The display fixed at its actual source, a real CLI, and the Bluetooth radio
finally doing something.**

### Fixed: the screen, properly this time

v1.5.0 fixed a genuine bug (the LVGL lock's `esp_err_t` read as a `bool`) but
the panel stayed black, so the diagnosis was incomplete. Reading Waveshare's
BSP source gives the real answer. `bsp_display_start_with_config()` runs five
steps in order:

1. `esp_lv_adapter_init()`
2. `bsp_display_lcd_init()` — **registers the LVGL display**
3. `bsp_display_indev_init()` — touch (CST9217, over I2C)
4. `bsp_display_brightness_init()` — backlight on
5. `esp_lv_adapter_start()` — starts the LVGL flush task

Step 3 aborts the whole function with `return NULL`. But step 2 has **already**
registered the display with LVGL. So `lv_display_get_default()` cheerfully
answers *"yes, there is a display"* while brightness was never initialised (on
an AMOLED, brightness **is** emission) and the flush task never started — every
`lv_*` call still succeeds, because it only mutates objects in RAM.

Pharos called `bsp_display_start()` and **discarded the return value**, then
asked LVGL whether a display existed. That question cannot distinguish "fully
up" from "aborted after step 2" — which is exactly the failure in front of us.

Now the return value is read. If it is NULL but the panel registered, Pharos
finishes the two skipped steps itself: **a working screen with dead touch beats
a black screen**, and this device is driven from the console anyway.

Diagnosis is no longer guesswork:
- the UI heartbeat reports `painted= missed= hud=`, so even a truncated log tail
  shows whether pixels are reaching the panel;
- **`diag`** reports display result, size, touch, lock, HUD, heap, radio, fence;
- **`screen test`** pushes a pattern to prove the pixel path end to end.

### New: a real CLI

The old console polled `getchar()` in a loop — no editing, no history, and
transport behaviour that differs between UART and USB-Serial-JTAG. It is now
built on ESP-IDF's `esp_console` REPL, with every command from the host-tested
table registered automatically, so tab completion and `help` reflect the real
thing rather than a copy that can drift.

The console is also moved to **USB-Serial-JTAG**. This board has one USB-C
socket wired to the ESP32-S3's native USB, not a UART bridge; with IDF's
default (console on UART0) log output still reached the port via the *secondary*
console — which is why logs were visible — but **stdin did not**, so the CLI was
unreachable over the only cable the board has.

### New lens: **Vigil** — is an item tracker travelling with you?

The Bluetooth radio has been idle since v1.0. This is what it is for.

Seeing a tracker means nothing — a café at lunchtime contains a dozen, all in
other people's bags. What matters is whether one is still with you **after you
have moved**. Pharos has no GPS, so movement is inferred from the world
changing: a **locale** is the set of audible access points, and when that set
turns over substantially, you are somewhere else. A tag present across several
locales is travelling with you.

Address rotation does not defeat this, and the reason is worth stating: Find My
devices rotate every ~15 minutes *while their owner is nearby*, but hold an
address far longer once **separated** from the owner — and a tracker planted on
somebody is by definition separated. The case that matters is the one that
stays addressable. Vigil says plainly that it undercounts rotating tags rather
than pretending otherwise.

What it refuses to do:

- **Never says you are safe**, or that no tracker is present. A receiver that
  hears one radio at a time for a few minutes cannot support that sentence, and
  for this subject a false reassurance is worse than no answer.
- **Never claims intent.** A tag travelling with you may be yours, a friend's,
  or in a parcel you are carrying. "Travelling with you" is a fact; "why" is a
  human's job. The advice checks those explanations first, then points at real
  options — making the tag play a sound, the serial number, the police.
- **Your own devices are excluded**, not fudged: `vigil mine` marks one known.
- Without a second locale the score is **capped below any alarm**, and a single
  move is capped below the top band, because you and a stranger can walk the
  same way once.

The BLE scan is **passive**, and that word is load-bearing: an *active* scan
answers every advertisement with a `SCAN_REQ`, which is a transmission. That
would break the one promise the project is built on, so `passive = 1` is now
audited by `check_tx_fence.sh` as a sixth fence mechanism.

**5,164 host checks, 0 failures.**

## v1.6.0 — 2026-08-14

### New lens: **Squall** — busy, broken, or being denied

The question a defender actually asks in a crisis is not "am I under attack?"
It is *"the Wi-Fi is down — is it broken, is it just busy, or is somebody
jamming us?"* Those three have completely different responses — call the ISP,
add capacity, or start a physical search — and to the user they look identical.

The discriminator, and the reason this is worth an engine rather than a
threshold on a noise meter:

| energy | decodable frames | verdict |
|---|---|---|
| high | **high** | `CONGESTED` — a busy building. Loud, and entirely healthy. |
| high | **low** | `DENIAL` — power that will not resolve into frames. |
| low | low | `QUIET` — nothing here… or a deaf receiver, and it says so. |

**A busy office is never called an attack.** That is the false positive every
naive "high noise = jamming" detector ships with, and a tool that cries wolf at
a crowded building gets switched off. It is the first test in the suite.

"Barren" is anchored to something physical rather than picked: a single access
point beacons roughly ten times a second, so hearing *fewer than ten frames a
second* means the receiver cannot decode even one beaconing AP — which, on a
channel the radio reports as busy, is exactly the condition worth naming.

The honesty rules:

- **DENIAL needs two evidence families.** Energy alone is a microwave oven, a
  video sender, a neighbour's outdoor bridge. When only energy is present the
  score is capped *and the word is downgraded too* — the number and the label
  are never allowed to disagree.
- **Nothing is graded from a single dwell.** A jammer is a sustained condition;
  one bad 300 ms visit is a sample.
- **A single loud channel is capped**, because real denial rarely covers just
  one — that is far more likely to be one piece of equipment.
- **The harshest ceiling curve in Pharos.** Every other engine reasons about
  frames that *arrived* and can extrapolate; this one reasons about frames that
  did not — and "no frames on channel 11" is indistinguishable from "we were
  not listening to channel 11".

This also fixed a real gap: `PHAROS_EV_DWELL` had been **defined but never
produced** since v1.0. The radio now emits one summary per channel visit
(frames, retries, airtime, peak RSSI), which is what Squall reasons about. The
noise-floor field is left at zero and disclosed as unknown rather than invented,
because the driver exposes no true noise register.

`squall` on the console, `squall camp 6` to interrogate one channel.

**5,076 host checks, 0 failures.**

## v1.5.0 — 2026-08-13

**The panel actually paints now — and the device has a memory.**

### Fixed: the screen was still black — the real cause

v1.4.0 powered the panel (backlight at 100 %, LVGL task running) but **never
drew a single pixel**, and this is why. Waveshare's v3.0.0 BSP drives LVGL
through Espressif's `esp_lvgl_adapter`, whose lock is an `esp_err_t`:

```c
esp_err_t bsp_display_lock(uint32_t timeout_ms);   // ESP_OK (== 0) means ACQUIRED
```

Pharos' wrapper returned that value straight as a `bool`. `ESP_OK` is `0`, so a
**successful** lock read as `false` — and every drawing site is guarded by
`if (!lock) return;`. The boot splash that *creates* the HUD, and all 5 Hz
repaints, saw a good lock as a failure and bailed out. The panel was up, the
app was running (`ui: active: wifi.spectrum` in the logs), and nothing was ever
painted. The fix is one line — compare against `ESP_OK` — plus a belt-and-braces
`pharos_hud_create()` on the first painted frame so a single missed lock at boot
can no longer leave the screen blank for the session.

*This was found by reading the vendor BSP source, exactly as reported from the
board: everything ran, the screen stayed dark.*

### New lens: **Aegis** — the one screen that tells the story

Every other lens answers one question and then forgets. Two things are true of
real defensive work and false of every single-purpose detector: an attack is a
*sequence*, not a reading; and **you are not looking** — a handheld shows one
lens at a time and the capture you care about lasts seconds.

So Aegis **latches**. Each stage (RECON → IMPERSONATE → DISRUPT → HARVEST →
DRIFT) keeps its own high-water mark with the time it happened, fed by whichever
lens is active through a new `stage_report` entry in the lens vtable. Walk back
to the device ten minutes later and it still tells you what fired, how high, and
how long ago.

The honesty rules are the design:

- **Correlation may not invent evidence.** With one stage raised there is
  nothing to correlate, so Aegis reports that stage's own score *unchanged* —
  a single finding, however loud, can never become an "incident" by fusion.
- **The ceiling is the minimum** across contributing stages. A conclusion drawn
  from a thin sweep and a good one is only as good as the thin one. A stage's
  peak also keeps the ceiling it was *measured* with, so a later high-quality
  sweep cannot retroactively lend confidence to an old, thin observation.
- **The sequence bonus requires actual ordering in time.** Four alarms in a
  jumble is a noisy room; the same four in the attacker's own order is a
  campaign, and only the latter earns it.
- **A latched peak is always reported with its age**, and the headline says so
  rather than implying the present tense.

`aegis` on the console; `aegis ack` clears the latch once you have read it —
the device never forgets a finding on its own.

### New lens: **Harvest** — somebody is collecting your handshakes

The attack that ends in a cracked Wi-Fi password, and the quiet one: nothing
goes down, nobody complains, and the capture lasts seconds. Two routes, told
apart because the response differs:

- **Forced** — deauthenticate a client and record messages 1 and 2 of the 4-way
  handshake as it reconnects. Those two travel *before* the pairwise key exists,
  so a passive listener can read them. The tell is the ordering and the
  tightness, repeated.
- **PMKID (clientless)** — solicit a PMKID from the access point itself. No
  victim, no outage, and **no second message**, because the attacker never
  intended to finish connecting. An unanswered solicitation is deliberate;
  ordinary clients complete their handshakes.

This required real EAPOL visibility, so `pharos_dot11_eapol()` is new: a
paranoid, fully host-tested parser (every truncation length is fuzzed) that
identifies the pairwise message number and the PMKID KDE. It refuses protected
frames — those are ciphertext, and parsing them would be reading noise and
calling it evidence. Pharos stores no nonces, no MICs and no key data, and has
no transmitter with which to complete or replay anything.

Honesty, again, is the point: one disconnect followed by one handshake is a
*rebooting access point* and is explicitly not an evidence family; where 802.11w
is in force forged disconnects are rejected, so that evidence is discounted
hard; and one family alone can never reach HARVEST LIKELY.

### New lens: **Sentinel** — what changed since your site baseline

The other lenses answer *"is anything wrong right now."* Sentinel answers the
question that actually starts incidents: *"what changed since I last swept this
building?"* Adopt a baseline while you believe the estate is clean, then every
later sweep is diffed against it — **NEW / MISSING / MOVED / RENAMED / UPGRADE /
DOWNGRADE**. The severity model is deliberately asymmetric and host-tested:

- a **downgrade** (an AP that dropped from WPA3+MFP to open, or lost 802.11w) is
  scored highest — it is either a misconfiguration or an impersonation, and both
  need a human;
- a **new** AP is ordinary churn and scores low, unless it is *also* open or
  wearing an SSID the estate already owns — the shape of an impersonator;
- **missing** scores lowest of all and is discounted further on a thin sweep,
  because a receiver that hears one channel at a time is far more likely to have
  *missed* an AP than to have watched it leave.

Reachable on the dial and over the console: `sentinel` to watch, `sentinel
adopt` to freeze the baseline when the site is clean, `status` to read the diff.
Pure-C engine with **86 new host checks**.

### Also

- Console gains `sentinel`, `harvest` and `aegis` commands plus `adopt_baseline`
  and `acknowledge` ops. The receive-only table-walk invariant still holds: no
  command can emit a frame, because the ops vtable cannot express one.
- **4,999 host checks, 0 failures** (up from 4,727) across 16 engines.
- `tools/check_sources.sh` and `check_lenses.sh` cover every new engine and
  lens, so none can silently fall out of either build.

Two real defects were caught by the new tests while writing them, and both are
worth recording: the EAPOL parser initially read the ethertype, `key_info` and
`key_data_len` with the little-endian reader (802.11 is little-endian, but
everything riding on it is network byte order), and the Harvest engine used a
timestamp of `0` as its "no disconnect seen" sentinel — silently dropping any
disconnect in the first microsecond of a sweep.

## v1.4.0 — 2026-08-13

**The screen works.** This release is driven by real hardware logs from a board,
and fixes the two defects they exposed.

### Fixed: completely black panel

`CONFIG_PHAROS_HAS_VENDOR_BSP` defaulted to **n**, so `pharos_bsp_init()` ran
the *simulated* path and never brought the display up. It is now **on by
default**, and the real path follows Waveshare's own LVGL example exactly:
`bsp_display_start()`, then `bsp_display_lock()` / `bsp_display_unlock()`
around every `lv_*` call, because their BSP runs LVGL on its own task.

Adopted from their `examples/esp-idf/02_lvgl_demo_v9` sdkconfig, because these
are what make octal PSRAM and the CO5300 panel reliable rather than merely
bootable: `SPIRAM_FETCH_INSTRUCTIONS`, `SPIRAM_RODATA`, `SPIRAM_XIP_FROM_PSRAM`,
a 64 KB data cache with **64-byte lines**, a 32 KB instruction cache, QIO flash
and their LVGL settings. LVGL is pinned to the same **9.5.0** the BSP is built
against and marked `public` — letting the solver pick a different 9.x is
another route to a black screen.

And the panel now shows something: a new LVGL HUD (`pharos_hud.c`) with a boot
splash, a gauge arc, the active lens, a headline value, a band word and the
permanent receive-only pip, repainted at 5 Hz from the UI loop.

### Fixed: `failed to post WiFi event=43 ret=259`, several times a second

`ret=259` is `ESP_ERR_INVALID_STATE`. The Wi-Fi driver posts internal events
(start, channel change) to the default event loop **unconditionally**, even for
a receive-only NULL-mode driver — and that loop was never created, so a
channel-hopping lens produced one failed post per hop. Fixed with
`esp_netif_init()` + `esp_event_loop_create_default()` once at boot.

### Fixed: `BSP_*` namespace collision

`board_pins.h` defined `BSP_I2C_SCL`, `BSP_I2S_*` and friends — names the
vendor header owns for this exact board. Enabling the real BSP produced
`error: 'BSP_I2C_SCL' redefined`. Pharos no longer defines any pin at all: the
vendor header is the single source of truth, and `board_pins.h` keeps only the
facts it does not answer, in a `PHAROS_BOARD_*` namespace.

### Fixed: LVGL vs the IDF 6.0 toolchain

GCC 15 rejects LVGL's own `LV_ATTRIBUTE_FAST_MEM` IRAM placement
(`-Werror=attributes`). Downgraded for the LVGL library only — the same guard
Waveshare's example carries — so Pharos code keeps warnings-as-errors.

### New: the command console

A Cardputer-style REPL over serial (`pharos_console`, pure and host-tested):
`watch camp 6`, `census`, `locate <bssid>`, `karma`, `mirage`, `footprint`,
`report`, `fence`, `region`, `help`. Its command table is data, categorised
scan/analyse/evidence/system — **there is no transmit category and no ops entry
that can emit a frame**, and a host test walks the whole table to prove it.

### Branding

The logo, banner, wordmark and boot splash are rebuilt around the same
lighthouse the web flasher uses — proper lamp room, tapering banded tower, wide
base, and receive arches that converge **on** the lamp. Site, repo and device
are now one mark.

### Verification

- **4,727 host checks**, 0 failures
- Bounds, transmit-fence and lens-linkage audits green
- ESP-IDF **v5.5 and v6.0 both build with the real panel driver enabled**

## v1.3.0 — 2026-08-13

A visual-design pass on the whole HUD, plus a new blue-team hunt tool — and the
README architecture and status rewritten to match.

### Locate — walk to a flagged transmitter (new engine + lens)

Once Watch, Karma or Mirage names a suspect, the next question is physical:
where is it? Locate answers the only honest way a single antenna can — a
hotter/colder game. It camps on the target's channel and turns its smoothed
RSSI into a *closeness* and a *trend* (WARMER / COLDER / STEADY / HERE), then
walks the operator in. Receive-only: it finds a transmitter without becoming
one.

Two honesty constraints, both tested: RSSI is **not distance** (multipath and
bodies move it 20 dB without a step), so it reports relative closeness, never
metres; and the trend only flips after several consistent samples, so the
needle is calm rather than chasing jitter. A simulated walk-in must read WARMER
then HERE, a walk-out COLDER, and a noisy hold must stay STEADY.

### A proper UI/UX pass

The Virtual Pharos renderer — still generating every pixel from the real
geometry and engines — grew a real finish:

- **Bloom.** Bright elements (the score, the band, lit family pips, the status
  rim) are blurred and screened back, the lift an OLED panel actually has.
- **Gradient gauges.** The evidence arcs fill with a colour gradient toward the
  band colour, so a verdict reads as lit energy rather than flat paint.
- **Depth & chrome.** Recessed gauge tracks, a glowing status rim that shows
  the device's state from the bezel alone, sharper 4× anti-aliasing.
- **Two new screens.** A **Home** watch-face (clock, posture, a pip per armed
  watch) and the **Locate** finder.
- **A device gallery.** `assets/branding/gallery.png` composites every screen
  into bezelled device frames for the README — built by `tools/render/gallery.py`.

All of it still passes the bounds check — which caught, and made me fix, a
ceiling-label collision and a home-screen caption overlap along the way.

### README

- **Architecture** is now a rendered mermaid diagram of the two-core,
  receive-only flow.
- **Status** is an honest layer-by-layer table: what is proven (engines, UI
  geometry, fence, firmware build) versus what is not (board bring-up M1, on-
  device LVGL M2).

### Flash from your browser (new)

A one-click **web installer** at
[at0m-b0mb.github.io/Pharos-ESP32S3](https://at0m-b0mb.github.io/Pharos-ESP32S3/):
open it in Chrome or Edge, plug the board in over USB-C, click Install. It flashes
over Web Serial with no toolchain and nothing uploaded anywhere. A GitHub Pages
workflow (`.github/workflows/pages.yml`) serves the *same* fence- and
lens-audited binary the release ships — pulled from the release, never committed
to git — so the flasher is always the latest verified image.

### Verification

- **4,396 host checks**, 0 failures
- Bounds, transmit-fence and lens-linkage audits green
- **11 lenses**, all verified present in the linked ELF; firmware binary attached
- Browser flasher live and serving the audited binary same-origin

## v1.2.0 — 2026-08-13

More red-team value, all of it still receive-only. Two new detection engines,
a red-team OPSEC tool, and two more screens rendered from the real code.

### Mirage — beacon-flood / SSID-spam detection (new engine + lens)

Detects the attack the ESP32 world is most known for: a radio inventing
hundreds of fabricated network names to spam every phone's Wi-Fi list.
Evil-M5Project ships that attack; Pharos is its inverse and now *detects* it,
passively, without transmitting a frame.

Three families — VOLUME (new names per second), EPHEMERAL (names heard once
then gone), SYNTHETIC (software BSSIDs sharing a prefix). The false positive it
exists to defeat is a genuinely dense city: 60 real networks from real vendors
that *persist*. VOLUME alone is capped below the alarm band, so a busy rooftop
reads BUSY, not FLOOD — there is a test that stands in a simulated city and
insists on it. Same confidence ceiling as everything else: hopping cannot alarm.

### Footprint — the red team's OPSEC mirror (new engine + lens)

The honest way a receive-only tool serves a red team. Pick an attack; Footprint
grades it with the *real* Watch engine against two defenders — one camped, one
hopping — and reports how detectable you are:

- a grade: GHOST / FAINT / LOUD / BLARING,
- the **dominant tell** — the single piece of evidence carrying your score,
- the **stealth gap** — what a defender loses by hopping instead of camping, and
- the decisive line: **would a hopping defender even notice?**

Understanding your own signature is tradecraft, and this teaches it defensively,
from the watchtower's side, without a transmitted frame. Its guidance explains
*detectability* and never coaches evasion — there is a test that greps for it.

### Two more screens, from the real engines

`tools/render` now draws Mirage and Footprint too, driven by the actual
detection code. The Footprint screen shows the camped-vs-hopping arcs side by
side — the OPSEC insight made visual. All ten lenses now appear on the rendered
Lamp Room dial, and the bounds check still passes (it caught, and I fixed, the
side-label collisions on the new screen).

### Verification

- **4364 host checks**, no board required: `make -C test/host`
- Round-screen bounds check, transmit-fence audit, lens-linkage audit
- CI runs all four plus ESP-IDF builds against v5.5 and v6.0
- **10 lenses**, all verified present in the linked ELF

## v1.1.0 — 2026-08-13

Three genuinely new capabilities, one important bug caught, and the UI made
visible — all still without a single transmitted frame.

### KARMA / MANA rogue-AP detection (new engine + lens)

Catches the attack that pairs with Probe: a rogue radio that listens for the
network names phones shout, and answers "yes, that's me" to every one.

The signal is that an honest access point *announces* what it carries, in
beacons, continuously — a KARMA responder cannot, because it does not know
which names to advertise until somebody asks. So the engine scores the gap
between "names this radio answered for" and "names it ever announced on its
own", across three families (breadth, absence, echo).

The false positive it exists to defeat: a corporate AP carrying guest, staff,
IoT and voice networks answers for four SSIDs too — and beacons all four. It
scores **zero**, by construction, and there is a test for exactly that estate.
An alarm requires the ABSENCE family specifically.

### Tamper-evident evidence chain (new)

Chain of custody for an engagement, on a £40 board. Every committed record is
hash-linked to the one before it with a vendored, NIST-vector-tested SHA-256:

    H(n) = SHA-256( H(n-1) || seq || t_us || kind || payload )

Modification, deletion, insertion and reordering are each caught, and each has
a test. Honest about its limit: this is **integrity, not authorship** — a device
you can disassemble cannot keep a signing key from its owner. Publish the head
digest somewhere you do not control and you have a witnessed timestamp, which
is the guarantee that actually holds.

### Virtual Pharos — the round HUD, rendered from the real code

`tools/render` links the actual `pharos_round`/`pharos_dial` geometry and the
actual detection engines, plays a `pharos_range` scenario through them, and
emits a display list that `rasterize.py` turns into PNGs. These are not
mock-ups; the numbers on screen are engine output.

The headline image is the camped/hopping pair generated from **one identical
event stream**: `FLOOD LIKELY 88` on the left, `SUSPICIOUS 60` on the right,
with the ceiling tick and the dimmed "denied" arc showing precisely what the
observation quality cost. The project's thesis, drawn by the project's code.

`make -C tools/render check` bounds-checks every primitive against the panel's
safe radius and now runs in CI, so "does the UI fit on a circle" is a test
result. It immediately earned its keep, catching three real layout bugs:
off-axis dial labels measured with the on-axis fitter, spectrum bars that could
exceed the glass, and census cards sized from their midline rather than their
worst corner.

### Fixed: lenses were being discarded at link time

The firmware built clean, produced a valid image, and would have booted with
**zero lenses**. `INTERFACE --whole-archive` on the component library is not
enough in ESP-IDF's link model; because a lens registers only from a
constructor, nothing referenced its translation unit and the linker dropped it.

Fixed with the `WHOLE_ARCHIVE` argument to `idf_component_register`, and made
un-regressable by `tools/check_lenses.sh`, which verifies the declaration in
every lens component *and* greps the linked ELF for every lens id. Also added
three engine sources (`power`, `probe`, `range`) that were missing from the
component's build list — the same class of bug, spotted while fixing this one.

### Verification

- **4298 host checks**, no board required: `make -C test/host`
- Round-screen bounds check: `make -C tools/render check`
- Transmit fence audit: `tools/check_tx_fence.sh`
- Lens linkage audit: `tools/check_lenses.sh`
- CI runs all four, plus ESP-IDF builds against v5.5 and v6.0

## v1.0.0 — 2026-08-12

First release. The judgement layer is complete and tested; the display layer
is scaffolded against tested geometry. Nothing here has met real hardware yet
— see *Known limits* below, and treat every hardware claim as unverified until
milestone M1 closes.

### The architecture

- **Transmit fence.** Raw 802.11 injection, soft-AP mode, `esp_wifi_deauth_sta`
  and ESP-NOW are severed at link time with `-Wl,--wrap`; NimBLE is built with
  the broadcaster and peripheral roles disabled so the advertising code is not
  in the image. `tools/check_tx_fence.sh` audits sources, link flags, Kconfig
  and the linked ELF, and runs on every commit in CI.
- **Capability tokens.** Every lens declares its powers at compile time; the
  radio HAL refuses a lens that asks for something it did not declare. There is
  no `CAP_WIFI_TX` and no `CAP_BLE_ADV` to grant.
- **Lock-free ingest bus.** Single-producer ring between the Wi-Fi driver
  callback on core 0 and the analytics task on core 1. Drop-newest overflow,
  with the drop count feeding the confidence ceiling rather than silently
  under-reporting.
- **Self-registering lenses.** Adding a tool is adding one `.c` file — no menu
  table, no central switch. Registration is through constructor attributes, so
  the same code registers lenses in the host build.

### Detection engines — all pure C, all host-tested

- **Watch** — deauthentication and disassociation flood grading across three
  evidence families, with a confidence ceiling derived from measured channel
  dwell and ingest yield. A hopping receiver tops out at SUSPICIOUS; the alarm
  band requires camping *and* all three families agreeing.
- **Census** — access point posture grading, A+ to F, over authentication (45),
  management frame protection (25), cipher (15) and exposure (15), with hard
  ceilings for open, WEP, WPA1, TKIP, WPS PIN and missing 802.11w. Declines to
  grade below three beacons.
- **Twin** — rogue and evil-twin detection that treats BSSID multiplicity as
  worth exactly zero, because a roaming deployment is not an attack. An alarm
  requires the posture family specifically.
- **Probe** — probe-request privacy grading that classifies leaked network names
  into kinds of place, weights by how much each narrows a person down, and
  defeats MAC randomisation via IE fingerprint + 802.11 sequence continuity to
  prove the exposure. Silence is the only route to A+.
- **Range** — a red-and-blue training simulator that plays five synthesised
  scenarios (calm, roaming, deauth flood, evil twin, probe leak) through the
  *real* engines. Seed-deterministic, narrated, and holds no radio capability.
  A test asserts the flood scenario reaches FLOOD LIKELY camped and only
  SUSPICIOUS hopping — the lesson is not allowed to be a lie.
- **802.11 dissector** — fixed header, reason codes, information elements, and
  an RSN parse covering SAE, PSK, OWE and the MFP capability bits. Bounds-checked
  against beacons that lie about their own element lengths.
- **Power planner & region clamp** — pre-launch runtime prediction from AXP2101
  state of charge and screen mode (pessimistic by design), and a regulatory
  channel-plan clamp (World/FCC/ETSI/Japan).

### Evidence

- **Report writer** — bounded streaming JSON with redaction applied *at write
  time*, in three profiles (full, OUI-only, salted hash). Every string is
  escaped, because SSIDs are 32 bytes of somebody else's choosing.

### Round-screen interface

- **Geometry kit** — three-zone layout, chord-width clipping guard, wedge hit
  testing, inscribed cards, and an AMOLED burn-in walk on two incommensurate
  periods.
- **Lamp Room dial** — radial launcher with a 44 px thumb rule that reports
  when a layout has too many items rather than drawing wedges nobody can hit.
- **Evidence gauge** — draws the points that were *capped away* as denied arcs,
  so the honesty model is visible rather than described.
- **Type fitting** — picks the largest size from the scale that survives the
  curve at a given radius, and returns zero rather than letting a label clip.

### Lenses (self-registering)

Spectrum, Watch, Census, Twin, Probe, Range and System — each one `.c` file that
ends in `PHAROS_LENS_REGISTER`. `main.c` names none of them.

### Branding

- Lighthouse-that-listens identity (receive arcs converge *inward*): round app
  icon, wordmark, 466×466 boot splash, and a social banner. SVG sources in
  `assets/branding/`.

### Verification

- 4139 host checks, no board required: `make -C test/host`
- Transmit fence audit: `tools/check_tx_fence.sh`
- CI runs both, plus an ESP-IDF build against v5.5 and v6.0

### Known limits

- **No hardware validation yet.** The radio HAL, the BSP bring-up and every pin
  constant marked `VERIFY` in `board_pins.h` are unproven against a real board.
- **2.4 GHz only.** The ESP32-S3 radio cannot hear 5 or 6 GHz, where most modern
  office traffic lives. Every report says so.
- **LVGL rendering is not wired.** The geometry is tested; the widgets that will
  use it are milestone M2.
- **Census reads the fixed header only.** The information-element walk that
  fills in RSN, WPS and the SSID from the capture ring lands in M5; until then
  `pc_grade` declines to grade what it cannot see.
