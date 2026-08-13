# Red, blue and purple — how Pharos serves each side, lawfully

Pharos is a defensive instrument. It never transmits, and it keeps nothing past
a session. Within those bounds it is genuinely useful to attackers-in-training,
defenders on the job, and the purple exercises that put them in one room — and
it is useful to all three *because* it is honest and passive, not despite it.

This document is the map. It is not permission: everything here assumes you are
working inside a scope you are authorised for. Read [SAFETY.md](SAFETY.md) first.

---

## The shared foundation

Every team runs the **same engines**. There is no "attacker build" and
"defender build"; there is one firmware, and the difference is which question
you point it at.

- **Watch** grades deauthentication floods.
- **Census** grades network posture, A+ to F.
- **Twin** finds evil twins without crying wolf at roaming.
- **Probe** shows what devices leak.
- **Range** plays synthetic attacks through those exact engines to teach them.

Because the Range drives the real detectors rather than a mock-up, a lesson
learned on it transfers with zero translation loss to the field. That is the
whole reason it exists.

---

## 🔵 Blue team — detection, audit, evidence

This is Pharos' home turf. On a site walk it answers the questions a defender
actually has:

| Question | Lens | What you get |
|----------|------|--------------|
| Is someone deauthing us right now? | **Watch** | A graded verdict with a confidence ceiling. Camp on the suspect channel and watch the ceiling rise. |
| Are our APs configured to policy? | **Census** | A+…F per network, with the one thing to fix first. Missing 802.11w caps a network at C — the same weakness Watch detects being *exploited*. |
| Is there a rogue AP on our SSID? | **Twin** | The impersonator identified by BSSID, with the posture gap that distinguishes it from our own roaming radios. |
| What are our people leaking? | **Probe** | Per-device exposure grades for an awareness session that lands because it is *their* phone on the screen. |
| Can I trust this device in a client's building? | **System** | On-screen proof the transmit fence is clean. |

**Evidence.** Every observing lens can write a session report — bounded JSON,
addresses redacted at write time (OUI-only or salted-hash), a clean artefact to
drop into an engagement record. The report always states the two things a
defender must not forget: this device hears **2.4 GHz only**, and **absence of a
finding is not proof of absence**.

**The two engines that talk to each other.** Census's "no 802.11w → capped at C"
and Watch's deauth grading are two ends of one weakness. Show a client the
Census grade, then show them Watch reacting to a Range flood on that same
posture, and the risk stops being abstract.

---

## 🟣 Purple team — exercises and demonstrations

Purple work is where Pharos is at its most persuasive, because it lets both
sides watch the same instrument at the same time.

- **Run an authorised attack from your own kit; watch it land on Pharos.**
  Your red side runs a deauth or evil-twin from a device *they* own and are
  authorised to operate. Pharos, in a blue hand, grades it live. The gap
  between what red did and what blue's ceiling would actually let them *claim*
  is the most valuable conversation in the room.
- **Calibrate detections.** Because Watch exposes its raw score, its ceiling and
  its per-family breakdown, a purple team can tune what "actionable" means for
  their environment against ground truth they created.
- **Awareness theatre that is true.** Probe in a town-hall, or the Range on a
  projector, teaches the mechanism without a single frame transmitted.

Pharos supplies the **blue instrumentation** for a purple exercise. It is not
the red tooling, by construction — bring your own authorised transmitter for
that half, and keep it inside scope.

---

## 🔴 Red team — understanding, not emulation

Here is the honest boundary. **Pharos does not transmit, so it does not run
attacks.** What it does for a red-teamer is arguably more useful long-term: it
teaches what your attacks look like *from the other side*, and which parts of
them you cannot hide.

- **Recon, passively.** Spectrum, Census and Probe are reconnaissance a red team
  legitimately needs — the lay of the air, the soft targets, the leaked
  histories — all without touching anything.
- **See your own signature.** The **Range** shows exactly how a deauth flood
  earns three evidence families, why a broadcast target and a spoofed identity
  are tells, and why standing still (camping) is what turns a defender's
  "suspicious" into "likely". If you understand the ceiling, you understand what
  a defender can and cannot prove about you.
- **Evil-twin craft.** The Twin lesson makes the point that *posture* is the
  giveaway: an open member of a protected SSID is the shape that gets you
  caught. That is a red-team lesson delivered by a blue-team tool.

### The Footprint lens — know your own signature

The **Footprint** lens is the red team's mirror, and the honest way a
receive-only tool earns its place in an operator's kit. Pick an attack; it
grades that attack with the *real* Watch engine against two defenders — one
camped on the channel, one hopping across the band — and tells you:

- a **detectability grade** (GHOST / FAINT / LOUD / BLARING),
- the **dominant tell**: the single piece of evidence carrying your score
  (broadcast targeting, spoofed identity, frame rate, reason-code monoculture),
- the **stealth gap**: how many points a defender loses by hopping instead of
  camping, and
- the operationally decisive line — **would a hopping defender even notice?**

A red team that internalises "a broadcast deauth lights three families instantly,
but a hopping defender caps at SUSPICIOUS" understands the engagement better than
one that just fires. That is tradecraft, taught defensively, without a frame
transmitted. And it is the same arithmetic the blue side is running — because it
is the same engine.

If you need to actually transmit for an authorised engagement, Pharos is not
that tool and will not become it — use dedicated, clearly-labelled offensive
kit, inside your rules of engagement. Pharos' refusal to transmit is the feature
that lets it be trusted in rooms offensive tools are not allowed into.

---

## The Range curriculum

Five scenarios, each a reproducible fixture that drives the live engines:

| Scenario | Teaches |
|----------|---------|
| **Calm network** | What normal looks like, so a break in it stands out. |
| **Roaming estate** | Why many BSSIDs on one SSID is *not* an attack — the false positive is the lesson. |
| **Deauth flood** | How a flood earns three families, and why hopping caps it below the alarm. |
| **Evil twin** | Why an alarm needs the posture gap, not just an odd radio. |
| **Probe leak** | How MAC randomisation is defeated, and what actually fixes it. |

Each scenario is seed-deterministic and carries on-screen narration timed to the
moment the verdict crosses a band. Pause it and ask "why did it not alarm there?"
— the answer is the same arithmetic that runs in the field, because it is the
same code. This is asserted in [`test/host/test_range.c`](../test/host/test_range.c):
the flood scenario *must* reach `FLOOD LIKELY` camped and stop at `SUSPICIOUS`
hopping, or the build fails and the lesson is not allowed to be a lie.

---

## What Pharos will never be

To keep the tool trustworthy for the people above, it will never gain:

- a frame injector, soft-AP, or deauth transmitter;
- a handshake/PMKID capture-for-cracking pipeline;
- credential capture, a captive-portal harvester, or any collection built to be
  kept and correlated across sessions;
- detection-evasion features.

Those lines are enforced socially in [CONTRIBUTING.md](../CONTRIBUTING.md) and
mechanically by the [transmit fence](POLICY.md). A pull request that crosses them
is declined on sight.
