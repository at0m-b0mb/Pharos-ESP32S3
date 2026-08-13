# Contributing to Pharos

Pull requests welcome. Four rules, and they are not style preferences — they
are the product.

## 1. No transmit primitives

A change that adds an injection path, removes a `-Wl,--wrap` flag, re-enables
soft-AP support, or turns on a NimBLE advertising role will be declined. If you
want an offensive tool, fork it and rename it; do not make this one lie about
what it is.

`tools/check_tx_fence.sh` runs in CI. A red fence blocks the merge.

## 2. Judgement goes in `pharos_engine`, with a failing test first

Detection logic must be a pure function: no ESP-IDF, no FreeRTOS, no
allocation, no floating point. That is what lets the whole detection model be
developed and regression-tested on a laptop.

A lens is plumbing plus a snapshot. If your lens contains an `if` that decides
whether something is an attack, it is in the wrong file.

```bash
make -C test/host
```

New behaviour lands as a test that fails, then the code that makes it pass.

## 3. Carry a ceiling

If your engine produces a verdict, it must also produce the quality of the
observation behind it, and the verdict must be capped by that quality. This
device has one receiver: an extrapolation is not a measurement, and the screen
must never present one as the other.

Two patterns to reuse rather than reinvent:

- **Duty correction and ceiling are separate.** Correct the estimate for what
  you did not hear; cap the confidence for the same reason. Doing only one of
  them is a bug in either direction.
- **Weak evidence scales with observation quality.** "I never heard it beacon"
  is meaningful when camped and nearly meaningless when hopping. Multiply, do
  not branch.

## 4. No all-clear

Band names may not contain "safe", "clear" or "secure", and advice strings may
not assert the absence of an attack. There is a test that greps for it.

The reasoning is in `docs/POLICY.md` §5. Short version: a device that says
"safe" will eventually be quoted saying it about somewhere that was not, and
the quote will omit that it was hearing 7% of one band at the time.

## Adding a lens

One file. See `docs/DESIGN.md` §6 and copy `components/pharos_lens_watch/`.

Your component's `CMakeLists.txt` needs the `--whole-archive` lines: lenses
register through constructor attributes, so nothing in the project holds a
reference to your translation unit and the linker would otherwise discard it.

Declare every capability you use. The HAL will refuse you otherwise, and the
declaration is what the operator reads on the info card before launching your
tool.

## Hardware claims

If you cannot verify something against a real board, say so in the comment and
mark the constant `VERIFY`. An unverified pin number that looks confident has
cost somebody a bring-up session; one marked `VERIFY` costs nobody anything.

## Commits

Conventional-ish subject lines, imperative mood, and a body that says *why*
when the change is not obvious. Comments in this codebase explain reasoning,
not mechanics — match that.
