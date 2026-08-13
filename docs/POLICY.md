# Safety & compliance guardrails, and why each exists

Guardrails that are only asserted get removed the first time they are
inconvenient. The ones in Pharos are built so that removing them is hard, loud,
or impossible — and each is paired here with the reasoning, so a future
maintainer who is tempted to relax one has to argue with the reason, not just
delete a line.

---

## 1. Receive-only, enforced four ways

**The guardrail.** Pharos cannot transmit a Wi-Fi or BLE frame.

**Why.** A monitoring tool that *could* be made to transmit with a one-line
change is a tool nobody should carry into a building they do not own. The value
of "it only listens" collapses entirely if it rests on good intentions. So it
rests on the build instead:

1. **No capability token.** `pharos_caps.h` defines the powers a lens may hold.
   There is deliberately no `CAP_WIFI_TX` / `CAP_BLE_ADV`. A lens cannot request
   a power that does not exist.
2. **Link-time `--wrap` fence.** `components/pharos_radio/tx_fence.c` traps every
   transmit primitive; the linker redirects calls to abort traps. A transmit
   call **panics the device at the call site** rather than emitting a frame.
   Silent failure was rejected on purpose: a fence that quietly no-ops rots,
   because nobody notices it working.
3. **NimBLE observer-only.** `sdkconfig.defaults` compiles out the broadcaster,
   peripheral and central roles. The advertising code is not in the image.
4. **Source + ELF audit.** `tools/check_tx_fence.sh` proves all of the above on
   every commit, and can re-audit the linked ELF in CI.

Adding a transmit primitive to the fence requires editing **three** places
(the trap, the linker list, the audit script). That friction is intentional.

**The device proves it about itself.** The System lens reads the fence status
and shows it on screen — wrap linked, BLE observer-only, no transmit symbol,
zero fence trips. Trust, but verify, on the operator's own hardware.

---

## 2. A ceiling on every verdict

**The guardrail.** No verdict may claim more confidence than the observation
behind it supports, and no configuration this firmware allows can reach 100.

**Why.** One receiver hearing ~7% of a hopped channel cannot rule out a
transmitter it never heard. A tool that says `SAFE` will eventually be quoted
saying it about somewhere that was not — and the quote will omit that it was
hearing a fourteenth of one band at the time. So:

- Every engine takes an observation-quality context (channel dwell, ingest
  yield) and computes a **ceiling** the score cannot exceed.
- Weak evidence *scales* with observation quality rather than switching on and
  off: "I never heard it beacon" is meaningful when camped, nearly meaningless
  when hopping.
- The UI shows the ceiling as a hard stop on the gauge and draws capped-away
  points as **denied arcs**, so the honesty is visible, not buried.

Asserted throughout [`test/host`](../test/host): a hopping receiver can never
reach the alarm band; the same stream reads higher camped than hopping.

---

## 3. Two families before an alarm

**The guardrail.** No single evidence family can raise a top-band alarm alone.

**Why.** One loud signal is not a case. A very high frame rate on a normally
shaped, correctly-addressed network is heavy housekeeping, not an attack.
Requiring two independent families (rate, targeting shape, sender identity)
before the alarm band makes a false positive require two things to go wrong at
once. In the Watch engine this is arithmetic — two families cannot exceed 74,
one below the threshold — not a rule that can drift.

---

## 4. Redaction at write time, never at export

**The guardrail.** Personal identifiers are redacted as the evidence is
*written*, not as it is exported.

**Why.** A redaction applied only on the way out is one forgotten flag from
being no redaction at all, and a full MAC that was written to flash can leak
from a partition image, a crash dump, or a support bundle long after. So
`pharos_report.c` redacts in the writer:

- **OUI-only** (`aa:bb:cc:xx:xx:xx`) — vendor kept, device dropped;
- **salted hash** — correlate a device with itself *within* one session, never
  across two, because the salt is fresh each session;
- **full** — only for an authorised assessment of your own estate.

And every string that crosses the writer is escaped, because the most
interesting strings Pharos handles — SSIDs — are 32 bytes of somebody else's
choosing. A network named `", "admin": true, "x": "` must produce a JSON
*string*, not forge a field in an evidence file. There is a test for exactly
that name.

---

## 5. No vocabulary of safety

**The guardrail.** No band, grade or advice string may say "safe", "clear",
"secure", or assert that no attack is present.

**Why.** See §2. The best grade Pharos gives is `A+`, and even its headline says
what the network *survived*, not that it is safe. A greppable test enforces the
whole vocabulary, so the rule cannot erode one cheerful string at a time.

---

## 6. Nothing kept past the session

**The guardrail.** Live analysis state is in RAM and dies with the session.
Persisted evidence is an explicit, redacted, operator-initiated act.

**Why.** Pharos exists to *demonstrate* exposure, not to build a copy of it. The
Probe lens can defeat MAC randomisation to make a point; it must not become a
tracking database. Device tables are bounded, reset on lens mount, and never
written anywhere unless the operator asks for a report.

---

## 7. Region-aware by default

**The guardrail.** The receiver stays within the channel plan of a selected
regulatory region.

**Why.** A monitor that tunes to a channel its region does not use is not
illegal the way transmitting there would be, but it is a tool that does not
know where it is. Pharos would rather know; the default is the safe World plan
until the operator chooses, and the choice is nudged on first boot.

---

## What the guardrails are *not*

They are not a claim that misuse is impossible. A determined person can fork the
repo, tear out the fence, and build something else — and at that point it is
something else, with a different name and their signature on it. The guardrails
make Pharos-the-shipped-thing trustworthy and make crossing the line a
deliberate, visible act rather than an accident or a default. That is the
achievable goal, and it is the one Pharos meets.
