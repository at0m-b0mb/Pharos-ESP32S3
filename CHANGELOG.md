# Changelog

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
