# Legal & ethical use — read before switching on

Pharos is a **defensive, receive-only** tool. It never transmits. That removes
a whole category of legal risk, but it does not remove your responsibility, and
this page is the part of the documentation you should not skim.

## The one rule

**Only use Pharos on networks and in engagements you are authorised to assess.**

Authorisation means one of:

- it is **your** network / device / estate; or
- you have **written permission** from the owner (a signed engagement, a scope
  document, an employer's policy that covers it); or
- it is a **deliberately provided training environment** (a CTF, a lab, a range
  you were invited to).

If none of those is true, switch it off.

## Passive is not automatically lawful

Pharos only listens — but the law in your jurisdiction may still regulate:

- **Intercepting communications.** Many countries restrict capturing wireless
  traffic that is not addressed to you, even without decrypting it. Pharos works
  with frame *metadata* and never decrypts, but you must know your local rules.
- **Processing personal data.** MAC addresses, probe SSIDs and device patterns
  can be personal data under regimes like the GDPR. Pharos keeps nothing past a
  session and redacts on write for exactly this reason — but the moment you
  *export* a report, you are the data controller for what is in it.
- **Radio regulations.** Pharos stays within a selected region's channel plan.
  Set your region in Settings on first boot.

This project cannot give you legal advice. When in doubt, ask someone who can,
in your jurisdiction, before you collect anything.

## Acting on findings

Detection is not license to intervene. If Pharos suggests a deauth flood or a
rogue AP:

- **Do** escalate through the proper channel — the site owner, the SOC, the
  engagement lead.
- **Do** preserve the evidence report if it matters.
- **Do not** retaliate, jam, or "hack back". Pharos cannot do any of those and
  neither should you; several are serious offences.

## The honesty you are relying on

Pharos is careful never to over-claim, and you should inherit that care:

- It hears **2.4 GHz only.** A quiet screen is not a quiet building — most modern
  traffic is on 5/6 GHz, where this device is deaf.
- It hears **~7% of a hopped channel.** Absence of a finding is **not** proof of
  absence. Camp to raise confidence; even then, nothing reads as certain.
- A verdict is a **prompt to investigate**, not a conclusion to act on blindly.

## Responsible disclosure

If you find a genuine weakness in someone else's system while authorised, follow
coordinated disclosure: report privately to the owner, give reasonable time to
fix, do not publish specifics that enable abuse before then.

## If you cannot meet these terms

Then Pharos is not the tool for what you are about to do, and the honest thing is
to stop. The transmit fence means it was never going to help you attack anyone —
but the responsibility for lawful, ethical use of what it *shows* you is yours.
