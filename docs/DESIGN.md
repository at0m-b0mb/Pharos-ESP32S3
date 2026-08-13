# Pharos — architecture & the honesty model

This is the how and, more importantly, the *why*. The mechanics are in the
code; this document is for the decisions a reader cannot recover from a diff.

## 1. The shape of the thing

```
┌──────────────────────────────────────────────────────────────────┐
│ core 0 (protocol/RX)              │ core 1 (analytics)            │
│                                   │                               │
│  esp_wifi promiscuous cb          │   pharos_ui_pump()            │
│    parse fixed header  ───────────┼──▶ drain ingest ring          │
│    timestamp, summarise           │     call active lens on_event │
│    push to ingest ring            │       └─▶ pure engine.observe  │
│  (hard real-time; no malloc,      │                               │
│   no mutex, no LVGL)              │   lens on_tick (~20 Hz)       │
│                                   │     engine.evaluate → verdict │
│  ── transmit fence sits here ──   │     snapshot for the UI       │
└──────────────────────────────────────────────────────────────────┘
        one receiver · one antenna · 2.4 GHz only
```

Two cores, split by role. The receive callback is the only code with a hard
time budget, so it does the minimum and hands off. Everything that reasons runs
on the other core, against copied data, and — the point — none of it depends on
ESP-IDF, so all of it runs on a laptop.

## 2. Why the engines are pure C

Every detection engine (`pharos_watch`, `pharos_census`, `pharos_twin`,
`pharos_probe`, `pharos_range`, plus `pharos_report`, `pharos_dot11`,
`pharos_round`, `pharos_dial`, `pharos_power`, `pharos_region`) is pure C: no
ESP-IDF, no FreeRTOS, no allocation, no floating point in the scored paths.

That constraint buys three things:

1. **Testability.** 4,139 host checks run in under a second with a plain `cc`.
   A scoring bug is caught on a laptop, not with a logic analyser.
2. **Determinism.** No floats in the score means the same input gives the same
   byte on every target. Verdicts are reproducible; the Range is a fixture.
3. **Auditability.** The judgement is separable from the plumbing. You can read
   exactly how a verdict is formed without wading through driver glue.

The rule for contributors follows directly: **if a lens contains an `if` that
decides whether something is an attack, it is in the wrong file.** Lenses are
plumbing; judgement lives in an engine, behind a test.

## 3. The ingest bus

One single-producer / single-consumer lock-free ring per lens
(`pharos_bus.c`). The producer is the Wi-Fi callback; it cannot block or take a
mutex. The consumer is the analytics loop.

Overflow policy is **drop-newest and count**. The count is not a debug
statistic — it is evidence. The moment a detector matters most (a flood) is
exactly when the ring overflows, so the drop count feeds the confidence ceiling.
A detector that quietly under-reported during a flood would be worse than
useless; this one widens its uncertainty instead. The memory-ordering argument
(two release/acquire fences are sufficient) is written out at the top of
`pharos_bus.c`.

## 4. The honesty model

This is the heart of Pharos, and it is three separate ideas that are easy to
conflate:

**Duty correction** recovers a true rate from a thin sample. If you heard 71
deauths while listening to 7% of the channel, the real rate is ~1000/s. The
engine computes this.

**The confidence ceiling** caps how much that recovered estimate is *allowed to
claim*. An extrapolation from 7% is not a measurement, so a hopping receiver's
ceiling sits around 60 — below the alarm band — no matter how alarming the
extrapolated rate. Camp, and the ceiling rises toward 96. Never 100: one
receiver cannot rule out a transmitter it never heard.

Doing only one of these is a bug in either direction — correct the estimate,
*and* cap the confidence. They are separate on purpose.

**Evidence families.** A verdict is built from independent families (for Watch:
rate, targeting shape, sender identity) with individual point ceilings, and the
alarm band requires two of them. This makes a single loud reading — however
extreme — unable to alarm alone. It is arithmetic: two families cannot sum past
74, one point under the threshold.

**Made visible.** The evidence gauge (`pharos_dial.c`) draws the ceiling as a
hard stop and renders capped-away points as *denied arcs* rather than dropping
them. The operator sees what the observation quality cost them, and sees that
camping would buy it back.

## 5. The round screen

466×466, and actually a circle — the corners do not exist. `pharos_round.c` is
pure geometry so the layout is verified before a pixel is lit:

- **Three zones.** Core (the one number), ring (the reading), rim (ticks,
  channels, badges). Every screen is built from these, which is what makes
  twenty lenses feel like one instrument.
- **The chord-width guard.** A line of text `dy` pixels off centre may only be
  as wide as the circle is there. `pd_label_size` returns the largest type size
  that survives the curve, or **zero** — meaning shorten the string, do not draw
  it and hope. A host test asserts every shipped label gets a non-zero answer at
  its radius.
- **The 44px thumb rule.** The Lamp Room dial reports when it has too many items
  to be hittable rather than drawing wedges nobody can press.
- **Burn-in walk.** Static HUD elements drift on two incommensurate periods so
  no AMOLED pixel holds one colour.

The LVGL widgets that consume this geometry are milestone M2; the geometry
itself is done and tested.

## 6. Adding a lens

One file. The registry (`pharos_lens.c`) is populated by constructor
attributes, so:

```c
static bool my_mount(void)  { /* allocate, init engine */ return true; }
static bool my_start(void)  { return pharos_radio_rx_start(&plan, &s_bus); }
static void my_event(const pharos_event_t *ev) { my_engine_observe(...); }
static void my_tick(uint32_t dt) { /* evaluate; snapshot for UI */ }
static struct pharos_bus *my_ingest(void) { return &s_bus; }

static const pharos_lens_t k_mine = {
    .id = "wifi.mine", .name = "Mine", .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN,   // declare everything
    .budget_ma = 130,
    .on_mount = my_mount, .on_start = my_start,
    .on_event = my_event, .on_tick = my_tick, .ingest = my_ingest,
};
PHAROS_LENS_REGISTER(&k_mine);
```

Add the component to `main/CMakeLists.txt`, keep the `--whole-archive` lines in
your component's `CMakeLists.txt` (or the linker discards your self-registration),
and it appears on the dial. `main.c` names no lenses.

The capability declaration is not decoration: the radio HAL refuses any power a
lens did not declare, and the declaration is printed on the lens' info card so
the operator sees what a tool can touch before launching it.

## 7. Where the honesty is tested

- `test_watch_flood_hopping` — the same flood tops out at SUSPICIOUS when hopping.
- `test_watch_single_family_cap` — one family cannot leave ELEVATED.
- `test_twin` — six BSSIDs on one SSID score zero (roaming ≠ attack).
- `test_report` — a hostile SSID cannot forge JSON fields; truncation never
  overruns; redaction is per-write.
- `test_range_flood` — the teaching scenario, through the real engine, must
  reach FLOOD LIKELY camped and stop at SUSPICIOUS hopping.
- `test_dial` — every shipped label fits the curve at its radius.

If you change scoring, a failing test comes first. That is not process for its
own sake — it is how the honesty model stays true across refactors.
