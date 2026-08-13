<p align="center">
  <img src="assets/branding/banner.png" alt="Pharos — Defensive RF Observatory" width="100%">
</p>

<h1 align="center">Pharos</h1>

<p align="center">
  <b>A defensive, receive-only RF observatory for the Waveshare ESP32-S3-Touch-AMOLED-1.75C.</b><br>
  A lighthouse that listens: it watches the 2.4&nbsp;GHz air, grades what it hears, and is honest about what it cannot.
</p>

<p align="center">
  <a href="#the-transmit-fence"><img alt="posture: receive-only" src="https://img.shields.io/badge/posture-receive--only-3DDC84"></a>
  <img alt="host checks" src="https://img.shields.io/badge/host_checks-4364_passing-1FB6C9">
  <img alt="platform" src="https://img.shields.io/badge/platform-ESP32--S3-2A6C82">
  <img alt="idf" src="https://img.shields.io/badge/ESP--IDF-5.5%20%7C%206.0-444">
  <img alt="license" src="https://img.shields.io/badge/license-MIT-blue">
</p>

---

## What it is

Pharos turns a £40 round-screen dev board into a **touch-first instrument for lawful wireless defence and training**. It is built for the two people who should own a tool like this:

- **the blue-teamer** walking a site, who needs to know if somebody is knocking clients off the Wi-Fi, whether the access points are configured the way policy says, and what the room is leaking; and
- **the learner** — red or blue — who needs to *understand* those attacks on real instrumentation without a lab full of gear, and without ever transmitting a frame.

It is inspired by the usability and extensibility of the Evil-M5Project, and deliberately takes the opposite stance on one thing: **Pharos cannot transmit.** Not "does not by default" — *cannot*. That is enforced by the build, checked in CI, and provable on the device's own screen. See [the transmit fence](#the-transmit-fence).

> [!IMPORTANT]
> **Authorisation first.** Even passive monitoring is regulated in some places, and *acting* on what you find is your responsibility. Use Pharos only on networks and in engagements you are authorised to assess. Read [docs/SAFETY.md](docs/SAFETY.md) before you switch it on.

## Why it is different

Most hobby RF tools answer "is there an attack?" with a confident yes/no. Pharos will not, because the hardware will not let it be honest that way:

> The ESP32-S3 has **one receiver**. To cover 2.4 GHz it hops, so it hears roughly **7% of any one channel** at a time. Multiplying an observed count by fourteen gives an *estimate*, not a *measurement*.

So every verdict Pharos produces carries a **confidence ceiling** derived from how much of the channel it actually heard. A deauth flood reads `FLOOD LIKELY` when you camp on its channel and only `SUSPICIOUS` when you keep hopping — the same traffic, a different honesty. There is no band named "safe". Absence of evidence on a receiver that hears 7% of the air is not evidence of absence, and the firmware never pretends otherwise.

This honesty is not a disclaimer bolted on. It is arithmetic, and it is [tested](test/host): 4,364 host checks assert, among much else, that no single loud reading can raise an alarm on its own and that hopping can never reach the top band.

## What it looks like

Every screenshot below is **generated from the firmware's own code** — the real
`pharos_round`/`pharos_dial` geometry, driven by the real detection engines, fed
by the real training scenarios. Nothing here is a mock-up drawn in a design tool.

<p align="center">
  <img src="assets/screens/watch_camped.png" width="46%" alt="Watch, camped: FLOOD LIKELY">
  <img src="assets/screens/watch_hopping.png" width="46%" alt="Watch, hopping: SUSPICIOUS">
</p>

**This pair is the whole argument.** Identical event stream, both sides. Camped
on the channel, the Watch engine reaches `FLOOD LIKELY` at 88. Hopping across
1–13, the same evidence tops out at `SUSPICIOUS` — the red `CEIL 60` tick is the
hard stop, and the dim arc past it is the score that was **earned and then taken
away** because a receiver hearing 7% of a channel is not entitled to claim it.

<p align="center">
  <img src="assets/screens/lamp_room.png" width="31%" alt="The Lamp Room dial">
  <img src="assets/screens/census.png" width="31%" alt="Census: posture grades">
  <img src="assets/screens/karma.png" width="31%" alt="Karma: impersonation watch">
</p>

Regenerate them yourself — and note that the generator **bounds-checks every
primitive against the panel's safe radius**, so "does the UI fit on a circle" is
a CI result rather than an opinion:

```bash
make -C tools/render        # writes assets/screens/*.png
make -C tools/render check  # asserts nothing escapes the glass
```

## The lenses

A **lens** is one tool. The dial is built from whatever lenses are compiled in; adding one is adding a single `.c` file. Every lens declares its hardware powers at compile time, and the radio refuses any power a lens did not declare.

| Lens | Team | What it does |
|------|------|--------------|
| **Spectrum** | 🔵 recon | 2.4 GHz airtime waterfall — the map you read the others against. States, permanently, that it is deaf above 2.4 GHz. |
| **Watch** | 🔵 detect | Grades deauthentication / disassociation floods across three evidence families, with a confidence ceiling. The headliner. |
| **Census** | 🔵 audit | Grades every nearby network A+…F on what it would take to break in — auth, 802.11w, cipher, WPS. |
| **Twin** | 🔵 detect | Finds the rogue AP wearing a name that belongs to someone else — and scores BSSID multiplicity at *zero*, because roaming is not an attack. |
| **Karma** | 🔵 detect | Catches the rogue AP that answers to *any* name a passing phone asks for — while scoring a legitimate multi-SSID deployment at zero. |
| **Mirage** | 🔵 detect | Detects the beacon/SSID-spam flood that fills every phone's network list — the exact attack the ESP32 world is famous for — while scoring a dense city as *busy, not hostile*. |
| **Probe** | 🟣 recon | Shows a room what its phones broadcast about their owners, and defeats MAC randomisation to prove the point. Awareness-session gold. |
| **Range** | 🔴🔵 train | Plays synthesised attacks through the **real** detection engines so you can learn to read the gauge. Holds no radio at all. |
| **Footprint** | 🔴 train | The red team's mirror: grades how *detectable* an attack is, names the family that gives it away, and tells you whether a hopping defender would even notice. Receive-only OPSEC. |
| **System** | ⚙️ audit | Battery, region, and live proof the transmit fence is clean. |

Red, blue, and purple all run on the **same engines**. The Range doesn't simulate a detector — it drives the actual one, so what you learn on it is exactly what you see in the field. See [docs/REDBLUE.md](docs/REDBLUE.md) for how Pharos serves each side.

### For red teams: know your own signature

Pharos will not transmit — that's the whole point, and it's why it can go into rooms offensive tools can't. What it gives a red-teamer instead is arguably more valuable: **OPSEC insight**. The **Footprint** lens grades any attack from the defender's side of the glass, against a camped defender *and* a hopping one:

<p align="center">
  <img src="assets/screens/footprint.png" width="46%" alt="Footprint: OPSEC detectability">
  <img src="assets/screens/mirage.png" width="46%" alt="Mirage: beacon-flood detection">
</p>

Left: a broadcast deauth flood reads **BLARING** to a camped defender (88) but only 60 to a hopping one — the dominant tell is *frame rate*, and the takeaway is stated plainly: *loud when watched, a hopping defender misses it*. Right: **Mirage** catching a 300-name beacon flood at **FLOOD LIKELY**, while a dense-city rooftop of 60 real networks scores *busy, not hostile*.

## The transmit fence

Receive-only is the product, so it is enforced four ways, and CI checks all four:

1. **Capability tokens.** Every lens declares its powers at compile time. There is no `CAP_WIFI_TX` token to hold — you cannot request what does not exist.
2. **Link-time wrap fence.** Every Espressif/NimBLE transmit primitive (`esp_wifi_80211_tx`, `esp_wifi_deauth_sta`, `esp_now_send`, AP-mode `esp_wifi_set_mode`, …) is redirected by `-Wl,--wrap` to an abort trap. Call one and the firmware halts at the call site rather than emitting a frame.
3. **Build-time role fence.** NimBLE is compiled **observer-only**; the advertising and connection code is not in the image.
4. **Source audit.** [`tools/check_tx_fence.sh`](tools/check_tx_fence.sh) greps the tree for transmit primitives and verifies all four mechanisms, on every commit.

The **System** lens reads the fence's own status and shows it on screen. A device that asks to be trusted in a building it does not own can *prove* it is only listening.

```bash
./tools/check_tx_fence.sh        # → FENCE INTACT - receive-only posture verified.
```

## Evidence you can defend

A report anyone can edit afterwards is a note, not evidence. Every record Pharos
commits is hash-linked to the one before it:

```
H(n) = SHA-256( H(n-1) || seq || t_us || kind || payload )
```

Modification, deletion, insertion and reordering are all caught by
`phc_verify` — each has a test. Addresses are redacted **at write time** (OUI-only
or salted-hash), never at export, so a full MAC that was never written cannot
leak from a partition image or a crash dump.

What it deliberately does *not* claim: this is **integrity, not authorship**. A
device you can disassemble cannot keep a signing key from its owner. Publish the
head digest somewhere you don't control — a ticket, a message to the client at
the end of the walk — and you have a timestamp someone else witnessed. That is
the honest version of the guarantee, and it is the one Pharos makes.

## Build & run

**The engines and UI geometry build and test with no board at all** — do this first, it is fast and catches the most:

```bash
make -C test/host                # 4364 checks, 0 failures
```

**Firmware**, with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) 5.5 or newer:

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

The board driver, LVGL, and the panel init come from Waveshare's managed BSP component (pinned in [`main/idf_component.yml`](main/idf_component.yml)); Pharos does not re-drive the AMOLED.

## Architecture at a glance

```
   radio (RX only) ──▶ lock-free ingest ring ──▶ analytics core
        │                  (drop-count feeds            │
   capability gate          the confidence ceiling)     ▼
   + transmit fence                            pure C engines ── host-tested
                                                  │  watch · census · twin
   round-screen UI ◀── snapshot ◀────────────────┘  probe · range · report
   (dial built from the lens registry)
```

- **`components/pharos_engine/`** — the judgement. Pure C: no ESP-IDF, no allocation, no floating point. All of it host-tested.
- **`components/pharos_radio/`** — the only component that touches the radio, and the transmit fence around it.
- **`components/pharos_ui/`** — round-screen geometry, the Lamp Room dial, the evidence gauge. Tested as maths before it is drawn.
- **`components/pharos_lens_*/`** — one tool each, self-registering.

Full detail in [docs/DESIGN.md](docs/DESIGN.md).

## Documentation

| | |
|--|--|
| [docs/DESIGN.md](docs/DESIGN.md) | Architecture, the honesty model, and how to add a lens. |
| [docs/REDBLUE.md](docs/REDBLUE.md) | How Pharos serves red, blue and purple teams — lawfully. |
| [docs/POLICY.md](docs/POLICY.md) | The safety guardrails, and *why* each one exists. |
| [docs/HARDWARE.md](docs/HARDWARE.md) | The board, the pin map, and every `VERIFY` still open. |
| [docs/SAFETY.md](docs/SAFETY.md) | Legal & ethical use. Read before switching on. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestones M1–M6 and what is proven vs. scaffolded. |
| [CONTRIBUTING.md](CONTRIBUTING.md) | The four rules that are the product, not style. |

## Status

**v1.1.0** — the judgement layer is complete and tested (4,364 host checks); the display layer is scaffolded against tested geometry. **No hardware validation yet:** the pin map, BSP bring-up and LVGL rendering are milestones M1–M2, and every unproven constant is marked `VERIFY`. See [CHANGELOG.md](CHANGELOG.md) and [docs/ROADMAP.md](docs/ROADMAP.md).

## License

[MIT](LICENSE) © at0m-b0mb. Built for people who defend networks and the people learning to.
