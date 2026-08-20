# Changelog

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
