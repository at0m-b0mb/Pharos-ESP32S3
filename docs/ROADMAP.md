# Roadmap

Pharos is built judgement-first: the part that must be *correct* — the detection
engines and the honesty model — is done and tested before the part that must be
*pretty*. This page is honest about what is proven versus scaffolded.

## Legend

- ✅ **done & host-tested** — proven on a laptop, no board required
- 🧱 **scaffolded** — code exists, compiles, but is unproven on hardware
- ⏳ **planned**

## v1.0.0 — the judgement layer (shipped)

| Area | State | Notes |
|------|-------|-------|
| Transmit fence (4 mechanisms) | ✅ | source-audited in CI; ELF audit in the IDF job |
| Capability model + lens registry | ✅ | self-registering, host-tested lifecycle |
| Lock-free ingest bus | ✅ | drop-count feeds the ceiling; ordering proven |
| Watch (deauth) engine | ✅ | families, ceiling, all caps tested |
| Census (posture A+…F) engine | ✅ | ceilings for open/WEP/WPA1/TKIP/WPS/no-MFP |
| Twin (evil-twin) engine | ✅ | roaming scored zero; posture-gated alarm |
| Probe (privacy) engine | ✅ | randomisation defeat via fingerprint + seq |
| Range (training) engine | ✅ | drives the real engines; verdicts asserted |
| Report writer | ✅ | bounded, escaped, redacted-on-write |
| Round-screen geometry + gauge + dial | ✅ | tested as maths |
| Power planner, region clamp | ✅ | |
| Radio HAL (RX) | 🧱 | compiles; unproven on silicon (M1) |
| BSP bring-up | 🧱 | wraps Waveshare BSP; pins marked `VERIFY` |
| UI runtime loop | 🧱 | drives lenses over serial; no pixels yet |

**Total: 4,139 host checks, 0 failures.**

## M1 — hardware bring-up

Turn 🧱 into ✅ on real silicon.

- ⏳ Flash on the board; confirm boot banner + clean fence on-device.
- ⏳ I²C scan; resolve every `VERIFY` address in `board_pins.h`.
- ⏳ Wire AXP2101 telemetry into the power planner; measure the baseline current
  and drop the `estimated` flag.
- ⏳ QMI8658 orientation for wrist-raise / still-detect.
- ⏳ Re-audit the linked ELF in CI (`check_tx_fence.sh build/pharos.elf`).

## M2 — the round HUD

The geometry is done; this draws it.

- ⏳ LVGL 9 widgets: the evidence gauge (with denied arcs), the Lamp Room dial,
  the lens cards, the Spectrum waterfall.
- ⏳ CST9217 touch → dial hit-testing and the crown-style rotate gesture.
- ⏳ Boot splash from `assets/branding/boot_splash.svg`.
- ⏳ Per-lens info card showing declared capabilities before launch.

## M3 — evidence & sessions

- ⏳ Write reports to the `evidence` partition; browse/export on device.
- ⏳ Session tally; site-profile save/load for Twin baselines.
- ⏳ Redaction profile chooser in Settings.

## M4 — BLE observer

- ⏳ NimBLE observer-only scan path (the header exists and refuses cleanly).
- ⏳ A BLE-privacy lens (tracker/beacon exposure), same honesty model.

## M5 — deeper Wi-Fi

- ⏳ Capture-ring body walk: full RSN/WPS/SSID information elements into Census
  and the Probe IE-order fingerprint (today's are header-derived).
- ⏳ Reason-code and vendor enrichment in reports.

## M6 — polish

- ⏳ Audio-assisted alerts (ES8311), respecting the outward-capability indicator.
- ⏳ Replay of the operator's *own* captures through the engines (offline range).
- ⏳ Companion export format for desktop review.

## Non-goals — permanently

Not "later". Never. See [REDBLUE.md](REDBLUE.md) and [POLICY.md](POLICY.md):

- ✗ any transmit / injection / soft-AP / deauth capability
- ✗ handshake/PMKID capture-for-cracking
- ✗ credential or captive-portal harvesting
- ✗ cross-session tracking storage
- ✗ detection-evasion features
