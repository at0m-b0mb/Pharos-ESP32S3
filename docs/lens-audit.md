# Pharos lens audit - findings raised against prior art
Seven lenses read against published implementations and papers, then each
finding put to an independent agent prompted to REFUTE it. 73 raised, 44
survived. This file is the raised set with full detail; severity from the
verification pass.


## aegis

**What it does.** Aegis is a radio-less correlator: the UI loop asks whichever lens currently holds the radio, once a second, for a (stage, score, ceiling) triple, latches a per-stage high-water mark with its timestamp and the ceiling it was measured under, and turns the five latched stages into one 0-100 score, a CLEAR/NOTED/ELEVATED/INCIDENT band and a headline, with the rules that a single raised stage may never be amplified, that the verdict is capped by the minimum contributing ceiling, and that an old peak is reported with its age rather than in the present tense.

**Prior art.** Fetched sources, and how each differs from ours:

1. SigmaHQ correlation spec (github.com/SigmaHQ/sigma-specification/blob/main/specification/sigma-correlation-rules-specification.md). Types: event_count, value_count, temporal, temporal_ordered. Two mechanisms we lack: (a) `timespan` is MANDATORY on every correlation (format Xs/Xm/Xh/Xd), so "these three fired together" is always bounded; (b) `group-by` fields define "separate event occurrence scope", so correlation is keyed to an entity, and `aliases` map differently-named fields onto one key. temporal_ordered "behaves like temporal and requires in addition that the events appear in the order provided in the rule attribute", and the spec warns about backends that cannot see order: "This could cause false positives by differently ordered events." Aegis's PA_NOTE_SEQUENCE has neither a timespan nor a group-by key: three stages 5 seconds apart and three stages 5 hours apart score identically, and stages about different BSSIDs/channels fuse as if they were about one target.

2. Wazuh composite rules (documentation.wazuh.com ruleset XML syntax). `if_matched_sid` / `if_matched_group` always appear with `frequency` AND `timeframe`, plus a sameness key such as `same_source_ip`; the shipped example is rule 31151, frequency 12, timeframe 90, same_source_ip. Same two mechanisms, same absence in ours.

3. Suricata xbits (docs.suricata.io/en/latest/rules/xbits.html). Cross-packet latched state, but keyed with `track ip_src|ip_dst|ip_pair` (never global) and with an optional `expire <seconds>`. Our latch is global and never expires — only the console `aegis ack` clears it, and there is no on-device control at all (finding 7).

4. Kismet alerts (kismetwireless.net/docs/readme/alerts/alerts/). Dedup by rate limiting, not fusion: `alert=NAME,throttle/rate,burst/rate`, e.g. `alert=NETSTUMBLER,5/min,1/sec`, "Excess alerts will not be reported". Directly relevant to our house rules: Kismet documents that DISASSOCTRAFFIC "may trigger a false positive when combined with channel hopping where the re-association is not seen" — a mainstream WIDS explicitly naming absence-under-hopping as a false-positive source. Aegis's n_live and PA_NOTE_STALE are exactly that shape of inference and are never disclosed as such (findings 3 and 4).

5. Ning, Cui & Reeves, "Constructing Attack Scenarios through Correlation of Intrusion Alerts", ACM CCS 2002 (engineering.iastate.edu/~daji/seminar/papers/NCR02.ACMCCS.pdf). A hyper-alert type is a triple (fact, prerequisite, consequence); two alerts are linked only when the earlier alert's consequence implies the later alert's prerequisite. Staging is earned by causal content. Aegis stages by ordinal position in an enum with no prerequisite test, so any three raised stages whose first_us happen to ascend in enum order are declared "an operation".

6. Valeur, Vigna, Kruegel & Kemmerer, "A Comprehensive Approach to Intrusion Detection Alert Correlation", IEEE TDSC 1(3):146-169, 2004. Ten-component pipeline: normalization, preprocessing, fusion, verification, thread reconstruction, attack session reconstruction, focus recognition, multistep correlation, impact analysis, prioritization. Note: the UCSB PDF returned HTTP 403 and the eScholarship page rendered empty, so I am citing this from the abstract and secondary descriptions rather than quoting the text — the component list is the useful part, and it puts "fusion" (same event, several sensors) and "multistep correlation" (different events, one campaign) in separate stages. Aegis conflates them: three lenses (wifi.watch, wifi.mirage, wifi.squall) collapse into the single DISRUPT slot where only the maximum survives, so corroboration by an independent sensor is worth exactly zero while an unrelated stage is worth +9.

7. "Alert Correlation Algorithms: A Survey and Taxonomy", arXiv 1811.00921 — PDF would not render through the fetch tool and the abs page carries only the abstract, so I got nothing citable beyond the framing that correlation exists to "reduce false alerts, detect high level patterns of attacks... and detect root cause".

The common thread: every working implementation binds correlation to (entity key, bounded window) and none of them infers "still happening" from a sensor that was not scheduled to be listening. Pharos already has the correct local precedent for the second half — pharos_tower.c's rotation_us()/ptw_freshness() measure freshness in laps scaled by each watch's period, and pharos_ui.c's home screen prints "NOT WATCHING" when the rotation is paused — and Aegis uses neither.

### [high] dishonesty — Stop asserting "several findings" when only one stage is raised
`components/pharos_engine/pharos_aegis.c:211`

**Wrong.** A single stage can reach the INCIDENT band on its own: the n_raised==1 branch sets `score = raised_peak;` unchanged (correctly, per the anti-invention rule), and band_of() promotes anything >= 70 to PA_BAND_INCIDENT. But the INCIDENT headline and advice are written as if plurality were guaranteed. With one raised stage at 85 the device prints "Several findings at once - this is an incident" and the advice "More than one independent stage has fired." PA_NOTE_SINGLE is already set in the verdict at line 161, and neither string consults it. Verified by compiling the engine and observing a single HARVEST 85/96: score=85 band=INCIDENT notes=0x04 (SINGLE) headline="Several findings at once - this is an incident". This is the most likely real-world reading on the device — Watch alone at FLOOD LIKELY is >= 75.

**Evidence.**
```c
out->headline = "Several findings at once - this is an incident";   /* line 211 */
...
return "More than one independent stage has fired. Capture evidence "   /* line 285 */
```

**Why it matters.** The operator reads a claim about the quantity of independent evidence that is simply false, and the advice sends them to "escalate to whoever owns this network" on the strength of a plurality that does not exist. It is the exact failure the file's own header sets out to prevent - "Correlation may not invent evidence" - committed in the presentation layer after the arithmetic got it right.

**Fix.** In pa_evaluate's PA_BAND_INCIDENT switch arm, branch on PA_NOTE_SINGLE before the existing SEQUENCE/LATCHED tests: when SINGLE is set use a headline like "One finding, and it scored this high on its own" (plus the LATCHED variant). Because pa_band_advice() takes only a band it cannot know this, so either add `const char *pa_verdict_advice(const pa_verdict_t *v)` that returns a single-stage INCIDENT text ("One detector reached an alarm on its own. Open that lens and read its verdict; nothing here is corroborating it.") and have lens_aegis.c:97 and console_glue.c call that instead, or keep pa_band_advice for the band word and append the single-stage caveat in the lens. Add a host test in test/host/test_aegis.c next to the existing line-44 check: pa_observe(PA_STAGE_HARVEST, 85, 96) alone must not produce a headline containing "Several" nor advice containing "More than one".

### [high] bug — Only let stages that actually contributed to the score set the ceiling
`components/pharos_engine/pharos_aegis.c:111`

**Wrong.** The minimum-ceiling loop treats any stage with a non-zero peak as a contributor, but when n_raised >= 1 the score comes solely from `raised_peak` (line 160/166) — background stages below PA_STAGE_ALARM contribute nothing to it. So a stage that played no part in the conclusion still caps the conclusion. Measured against the real engine: HARVEST 85/96 alone gives score 85 INCIDENT; adding a single quiet observation of DRIFT peak=1 with ceiling=45 (a lens that reported once with a thin dwell) drops the same verdict to score=45 ELEVATED, raw_score=85. The code's own comment says the ceiling is "pulled down by whatever actually contributed", and the header says "the MINIMUM of the contributing stages' ceilings" — `st->peak == 0` is not that test. Worse, this is attacker-reachable in the useful direction for the attacker: anything that makes an unrelated lens emit a low-confidence, non-alarm reading (a BLE beacon in the bag so ble.vigil reports a short-span RECON at ceiling 50, per pharos_vigil.c:447) silently suppresses the score of a genuine deauth flood.

**Evidence.**
```c
        if (st->peak == 0) {
            continue;
        }
        any_contrib = true;
        if (st->ceiling && st->ceiling < ceiling) {
            ceiling = st->ceiling;
        }
```

**Why it matters.** A real INCIDENT is displayed as ELEVATED or NOTED because an unrelated quiet lens had a thin sweep - the device downgrades itself using evidence that is not part of the claim, and an attacker who can raise any background reading anywhere gets a free mute on every other detector.

**Fix.** Compute the ceiling in a second pass over only the stages that fed the score. When out->n_raised > 0, take the minimum over stages with `st->raised` (optionally only those whose peak actually entered the sum - i.e. all raised stages, since each adds +9); when out->n_raised == 0, take the ceiling of the single stage that produced best_weighted (track its index alongside best_weighted in the existing loop). Keep any_contrib as is. Then extend test_aegis.c's ceiling block (line 118) with the negative: a raised HARVEST 85/96 plus a never-raised DRIFT 1/45 must still report ceiling 96 and score 85.

### [high] dishonesty — Measure liveness in rotation laps, not in a fixed 60 seconds
`components/pharos_engine/include/pharos_aegis.h:82`

**Wrong.** PA_FRESH_US is a hard-coded 60 s, but a stage's last_us only advances while ITS lens holds the radio, and the shipped ring hands the radio round on a schedule. With the default k_ring in pharos_ui.c:335 (7 armed period-1 watches, 4 armed period-2 watches, dwell 5000 ms, PTW_MAX_PERIOD 4) the tower's own arithmetic gives slices = 7*4 + 4*2 = 36, cycle = 180 s, average lap = 45 s — so a period-2 lens (wifi.squall -> DISRUPT, ble.vigil -> RECON) gets its turn roughly every 90 s. 90 > 60, so those stages can never be counted live and permanently contribute to PA_NOTE_STALE. Verified: pa_observe(DISRUPT, 70, 80) at t=90 s, evaluated at t=179 s (one second before its next turn is due) gives n_live=0 with PA_NOTE_STALE set. The sister engine one directory over already solves exactly this and documents why; Aegis ignores it.

**Evidence.**
```c
#define PA_FRESH_US 60000000ull /* 60 s */

/* and in the same engine directory, pharos_tower.c:163 */
    /* Scaled by THIS watch's period: a survey that is only due every second
     * lap must not be called stale for arriving exactly on schedule. */
    const uint64_t rot =
        rotation_us(s) * (uint64_t)(w->period ? w->period : 1u);
```

**Why it matters.** "still current: 0" and the stale note are claims about the air that are actually facts about the scheduler. Under the shipped ring a live jamming condition found by Squall reads as history within 60 s of its turn ending, every time - and it gets worse exactly when it matters, because ptw_turn holds the radio on an alarming watch and stretches every other stage's revisit interval past the 60 s bound.

**Fix.** Give pa_observe a per-observation freshness budget instead of using a global constant: add a `uint32_t revisit_ms` argument (or a `pa_set_budget(pa_state_t*, pa_stage_t, uint32_t)` call) that aegis_pump fills from the tower - ptw_lap_ms(&s_tower) multiplied by that watch's period, which pharos_tower.c already computes in rotation_us()/ptw_freshness(). Store it in pa_stage_state_t, and replace the two PA_FRESH_US tests at lines 135 and 184 with `(now_us - st->last_us) <= st->budget_us * 2` (two missed turns, mirroring PTW_AGEING_ROTATIONS/PTW_EXPIRED_ROTATIONS). Keep PA_FRESH_US as the fallback when the budget is 0, so the pure host tests still pass. Add a host test: a stage observed once with a 90 s budget must still read live at +89 s and not be stale.

### [high] dishonesty — Say NOT WATCHING on the Aegis screen while the rotation is paused
`components/pharos_lens_aegis/lens_aegis.c:114`

**Wrong.** Aegis's verdict is only reachable through pharos_ui_aegis_snapshot(), whose only callers are this lens' display/rows/report. Opening the lens by any route goes through lens_switch(), which sets `s_tower_on = false` ("watchtower: paused - %s was chosen by hand", pharos_ui.c:1282). sys.aegis has no stage_report, so aegis_pump() returns at pharos_ui.c:105 and nothing is folded in for as long as the operator is reading. Within 60 s (or one revisit interval, per the previous finding) n_live decays to 0, PA_NOTE_STALE sets, and the rows print "still current 0" as a statement about the room. The tower's home screen guards against precisely this and says so in capitals; the Aegis screen has no equivalent and no access to s_tower_on.

**Evidence.**
```c
        snprintf(o->detail, sizeof(o->detail), "%u raised  %u live",
                 v.n_raised, v.n_live);

/* and the guard that exists for the ring but not for this screen, pharos_ui.c:601 */
    /* PAUSED IS NOT QUIET.
     *
     * With the rotation stopped the watches stop reporting, every dot goes
     * hollow, and the summary - which only counts watches that have reported
     * recently - correctly finds nothing to worry about and says "all quiet".
     * Which is true, and is the most dangerous sentence this screen could
     * show: the room is quiet because nobody is listening to it.
```

**Why it matters.** The only way to read the whole picture is to stop collecting it, and the screen does not admit that. An operator who opens Aegis during an incident, reads the headline, and looks up thirty seconds later sees "still current 0" and concludes the attack stopped. The house rule is that a detector must never read its own missing instrumentation as a finding; this reads a paused scheduler as quiet air.

**Fix.** Export the device's own state to the lens: add `bool pharos_ui_watching(void)` in pharos_ui (return `s_tower_on || (pharos_lens_active() && pharos_lens_active()->stage_report != NULL)`) and a `uint32_t pharos_ui_aegis_since_feed_ms(void)` derived from the last successful pa_observe. In k_aegis_display, when nothing is feeding, overwrite o->advice with "NOT WATCHING - no radio lens is running while you read this" and in k_aegis_row case 1 render the right column as "paused" rather than a live count. Put the same flag in the JSON report as `"watching": false`. This is the same fix the ring already has at pharos_ui.c:607-612, applied one screen over.

### [high] dishonesty — Do not call the rotation's sampling order the attacker's order
`components/pharos_engine/pharos_aegis.c:131`

**Wrong.** st->first_us is set in pa_observe, which only ever runs for the lens currently holding the radio (pharos_ui.c:105-116). So first_us is not when a stage started happening, it is when Aegis was first told - which the tower's rotation, or the operator's browsing, decides. The ordering test then rewards whatever order the scheduler happened to sample in. Measured: three unrelated standing facts, each at exactly 50, observed 5 s apart in ring order, give score=76 PA_BAND_INCIDENT with PA_NOTE_SEQUENCE and the headline "Several stages, in attack order: treat as an operation"; the identical three facts sampled in the opposite order give 68 ELEVATED. There is also no minimum spacing, so three stages sampled inside one 15-second stretch of a single lap read as a campaign. The shipped ring makes it worse in both directions: it visits wifi.watch (DISRUPT) first and ble.vigil (RECON, period 2) last, so a genuine recon-then-twin-then-deauth campaign fails the test while three simultaneous standing facts pass it.

**Evidence.**
```c
            /* Did the stages arrive in the attacker's own order? The loop runs
             * in stage order, so first-seen times must be non-decreasing. */
            if (st->first_us < prev_first) {
                ordered = false;
            }

/* the header's promise, pharos_aegis.h:35 */
 *   - The ordering bonus requires the stages to have actually happened in
 *     order, in time.
```

**Why it matters.** +8 points and the strongest sentence the device can say - "treat as an operation" - are awarded for an artifact of the radio schedule. Sigma's temporal_ordered and Ning's prerequisite/consequence correlation both require the ordering to come from event content or a bounded timespan, never from the order the sensor happened to look.

**Fix.** Require the ordering to be bigger than the sampling grain, and say what it is measured on. (a) Require each consecutive pair of first_us values to be separated by more than one full rotation (reuse the revisit budget from the liveness fix, or a conservative PA_ORDER_MIN_GAP_US of 2 minutes) - three stages that all first appeared inside one lap prove nothing about order. (b) Since a stage cannot be observed before its lens has had a turn, only compare stages whose lens has been visited at least twice, so "A before B" is not just "A's lens came first in the ring". (c) Rename the note in the UI from "in attack order" to "first seen in this order" and keep the +8 only when (a) and (b) hold. Add the negative test to test_aegis.c: three stages 5 s apart must NOT earn PA_NOTE_SEQUENCE.

### [high] dishonesty — Derive the present tense from what is live, not from the worst peak's age
`components/pharos_engine/pharos_aegis.c:178`

**Wrong.** PA_NOTE_LATCHED is computed from one stage only - the highest-scoring one - and every headline's tense follows it, while out->n_live (which is the actual answer to "is anything happening now") is ignored by the headline logic. Both directions are wrong and both are reachable. Measured: DISRUPT peaked 80 at t=10 s and HARVEST is reporting 60 right now at t=700 s gives n_live=1 and the headline "Something serious happened here while you were not watching" - the device denies a live collection attack because an older, louder stage is the worst. And the reverse: DISRUPT 70 whose flood ended 45 s ago (current back to 0, n_live=0) is inside PA_FRESH_US, so LATCHED never sets and the headline is "Several findings at once - this is an incident" in the present tense about something that has stopped.

**Evidence.**
```c
    const pa_stage_state_t *w = &s->stages[out->worst];
    if ((now_us - w->peak_us) > PA_FRESH_US) {
        out->notes |= PA_NOTE_LATCHED;
    }
```

**Why it matters.** The header says the difference between "twenty minutes ago" and "right now" is "a different operational fact" and that the device says which - it says which of the WRONG thing. An operator told "while you were not watching" stops looking for the live harvest; an operator told "this is an incident" goes hunting for a flood that ended.

**Fix.** Keep PA_NOTE_LATCHED meaning what it says (the worst peak is history) but gate the headline on n_live: if out->n_live > 0 use present tense regardless of the worst stage's age; if n_live == 0 use the history wording regardless of whether the peak is inside 60 s. When both are true - something live AND an older louder peak - say both, e.g. "%s is live now; %s peaked %u minutes ago" using out->worst and the live stage. That needs the live stage's identity, so also record `pa_stage_t live_worst` in pa_verdict_t while you are in the loop at line 135. Add two host tests mirroring the two measured cases above.

### [high] ux — Wire the acknowledge control to the centre press
`components/pharos_lens_aegis/lens_aegis.c:166`

**Wrong.** The file's header says this is "the lens that displays it and the operator's acknowledge button", and pharos_lens_aegis_acknowledge() exists — but the k_aegis descriptor leaves .on_select NULL, and pharos_lens.h documents that as "NULL means the centre stays inert for this lens". The only caller of pharos_lens_aegis_acknowledge is glue_acknowledge in main/console_glue.c:132, reached solely by typing `aegis ack` over USB. The centre press on the ring's home view acknowledges a different latch entirely (ptw_acknowledge, pharos_ui.c:1022). Four other lenses (wifi.watch, wifi.ward, train.footprint, sys.audit) already use .on_select, and the contract comment for it makes exactly this argument about camping.

**Evidence.**
```c
static const pharos_lens_t k_aegis = {
    .id = "sys.aegis",
...
    .display = k_aegis_display,
    .row = k_aegis_row,

/* the mechanism it does not use, pharos_lens.h:227 */
     * The only way to camp was a console command over USB, which
     * is not a thing you do while holding the device up in a corridor.
```

**Why it matters.** On a handheld with no serial attached the Aegis latch can never be cleared, so after the first busy afternoon the summary screen is permanently stuck on an old peak and every later engagement reads through it. The design deliberately requires a person to clear the latch, and then gives that person no way to be one.

**Fix.** Add `.on_select = aegis_select` to k_aegis, where aegis_select() calls pharos_lens_aegis_acknowledge() and raises pharos_hud_toast("acknowledged") under pharos_bsp_display_lock, exactly as the ring path does at pharos_ui.c:1022-1026. Copy the ring's guard so a live alarm cannot be cleared by a stray press: take the snapshot first and only acknowledge when v.n_live == 0 (the tower uses `ack.worst < PTW_ELEVATED`); otherwise toast "still live" and do nothing. on_select runs on the UI task per the header contract, so calling pharos_ui_aegis_ack() from there is safe.

### [high] ux — Stop saying "One real finding" when several stages are raised
`components/pharos_engine/pharos_aegis.c:216`

**Wrong.** The ELEVATED headline is a fixed pair of strings that never looks at out->n_raised. Three raised stages that each score in the 45-50 range land at 68 - below the INCIDENT threshold - and the device reports them as "One real finding is live right now". Measured: DISRUPT 50, IMPERSONATE 46, RECON 45 gives n_raised=3, n_live=3, band ELEVATED, headline "One real finding is live right now".

**Evidence.**
```c
        out->headline = (out->notes & PA_NOTE_LATCHED)
                            ? "Something happened here while you were not watching"
                            : "One real finding is live right now";
```

**Why it matters.** It understates in the one band where the operator is deciding whether to keep walking, and it contradicts the row list on the next page, which says "stages raised 3". Two screens of the same verdict disagreeing is how an operator learns not to trust either.

**Fix.** Make the ELEVATED arm branch on n_raised, e.g. keep the current text when out->n_raised <= 1 and use "Several findings, none conclusive on its own" (and its LATCHED variant) when n_raised >= 2. Since the headline is a `const char *` into static storage, add the extra literals rather than formatting - no snprintf, no buffer, consistent with the rest of the engine.

### [high] ux — Record which lens reported a stage, since the advice tells the operator to open it
`components/pharos_engine/pharos_aegis.c:281`

**Wrong.** The ELEVATED advice sends the operator to "the lens named below", but no lens is ever named: pa_stage_state_t carries no identity, pa_observe takes only a stage index, and the most the screen shows is the stage word from k_aegis_display ("DISRUPT 80, 691s ago"). Three different lenses report DISRUPT - wifi.watch (lens_watch.c:373), wifi.mirage (lens_mirage.c:156) and wifi.squall (lens_squall.c:167) - and two report IMPERSONATE - wifi.karma (lens_karma.c:172) and wifi.twin (lens_census.c:840). The detail rows (k_aegis_row cases 0-4) show five numbers and name neither the stage nor the lens.

**Evidence.**
```c
        return "One finding is real enough to look at. Open the lens named "
               "below and read its own verdict, which carries the detail and "
               "the reasoning this summary does not.";
```

**Why it matters.** The one instruction Aegis gives cannot be followed. "DISRUPT" leaves the operator guessing between a deauth flood, a beacon flood and a jam - three different lenses, three different responses - and the lens comment calls this screen "the closest this device has to a case summary" while it omits the one fact a summary needs.

**Fix.** Add `const char *lens;` to pa_stage_state_t and a `const char *lens_id` argument to pa_observe; aegis_pump already holds `active->id`, which is a pointer into the lens's static descriptor, so storing the pointer needs no copy and no allocation (store it alongside the peak, at line 40, so it is the lens that set the high-water mark). Surface it as `const char *worst_lens` in pa_verdict_t, print it in k_aegis_display's detail line ("wifi.watch 80, 11m ago"), add it as a row, and add `prt_str(&w, "worst_lens", ...)` to the report. The host tests can pass string literals.

### [high] bug — Set PA_NOTE_THIN, and let the disclosure notes reach the screen and the report
`components/pharos_engine/include/pharos_aegis.h:78`

**Wrong.** PA_NOTE_THIN is defined and documented but has zero writers anywhere in the tree - the only other mentions are the definition itself. It is the flag that would disclose the single most important caveat in this engine (a contributing sweep was thin), and it can never fire. PA_NOTE_STALE is set at line 190 but is read by nothing: lens_aegis.c's report emits only `latched` and `sequence`, console_glue.c:201 prints only latched, and no row or display string mentions it. PA_NOTE_SINGLE likewise never leaves the engine. And `raw_score` reaches the glass on row 4 ("earned / allowed") but is missing from the JSON report, so an exported report cannot show that an 85 was cut to 45 by the ceiling.

**Evidence.**
```c
#define PA_NOTE_THIN      (1u << 3) /* a contributing sweep was thin        */

/* what the report actually exports, lens_aegis.c:95 */
    prt_bool(&w, "latched", (v.notes & PA_NOTE_LATCHED) != 0);
    prt_bool(&w, "sequence", (v.notes & PA_NOTE_SEQUENCE) != 0);
```

**Why it matters.** The two caveats that most change how much weight a verdict deserves - it was measured through a thin sweep, and nothing has updated recently - are either unreachable or invisible. A report exported for an incident write-up shows a capped score with no record of what was cut or why.

**Fix.** Set PA_NOTE_THIN in pa_evaluate when the binding ceiling is low (after the ceiling pass, `if (ceiling < 70) out->notes |= PA_NOTE_THIN;` - 70 is where pharos_vigil.c's own un-discounted ceiling sits, and pw_ceiling's floor is 45, so it separates a thin sweep from a camped one). Then export the notes: add `prt_bool` entries for `thin`, `stale` and `single` plus `prt_u32(&w, "raw_score", v.raw_score)` to pharos_lens_aegis_report, append " (thin)" or " (stale)" to k_aegis_display's advice line, and add the corresponding CHECK to test_aegis.c so a thin contributor sets the flag.

### [medium] blind-spot — No Wi-Fi reconnaissance lens feeds the RECON stage
`components/pharos_lens_rival/lens_rival.c:575`

**Wrong.** PA_STAGE_RECON is fed by exactly one lens - ble.vigil, a BLE tracker-following detector (lens_vigil.c:286). rf.rival, whose whole job is "Finds the other operator's hardware announcing itself" and which is armed every lap in the default ring, has no stage_report at all. So the first stage of the kill chain the engine's header narrates - "Somebody scans, then stands up a lookalike access point, then knocks clients off it" - has no Wi-Fi-side sensor behind it. PA_STAGE_DRIFT is similarly fed only by wifi.sentinel, which ships disarmed (`{ "wifi.sentinel", 3, false }`, pharos_ui.c:354), so in the default configuration DRIFT is unreachable without the operator opening Sentinel by hand.

**Evidence.**
```c
static const pharos_lens_t k_rival = {
    .id = "rf.rival",
    .purpose = "hacking hardware",
    .name = "Rival",
    .summary = "Finds the other operator's hardware announcing itself",
```

**Why it matters.** The worked example in the engine's own header cannot be assembled on a shipped device: two of the five stages have no armed sensor, so the four-stage sequence that justifies the INCIDENT band is unreachable while a two- or three-stage jumble is easy. Pentest hardware sitting in the room - the single clearest "somebody is looking" signal this firmware has - never reaches the correlator.

**Fix.** Add a k_rival_stage() reporting stage 0 (PA_STAGE_RECON) from s_verdict, as the other seven lenses do. prv_verdict_t has `score` and `raw_score` but no `ceiling` field, and rival caps internally instead (see the presence-cap comments at pharos_rival.c:696 and 722 and the patient-ceiling note at 780) - so either surface those caps as `uint8_t ceiling` on prv_verdict_t, or, if that is too invasive, pass a deliberately conservative fixed ceiling (70, matching pharos_vigil.c:419's un-discounted value for a rotation-limited BLE observer) and say so in the comment. Rival's evidence is forgeable (advertised names and address behaviour), which is fine here: Aegis only ever raises. Separately, decide whether wifi.sentinel being disarmed by default should be disclosed on the Aegis screen, since it makes one of the five stages structurally silent. Do NOT wire wifi.probe - it grades what the room's own phones leak, not an attacker, and feeding it as RECON would be the noise the census comment at lens_census.c:835 warns about.

### [medium] bug — A stage whose ceiling is zero is treated as having no ceiling at all
`components/pharos_engine/pharos_aegis.c:111`

**Wrong.** The `st->ceiling &&` guard makes a zero ceiling mean "do not constrain" rather than "no confidence". Verified: pa_observe(PA_STAGE_HARVEST, 90, 0) followed by pa_evaluate gives out->ceiling = 100 and score 90. Every one of the eight stage_report hooks passes its engine's ceiling through unconditionally and returns true even before its engine has evaluated once (each reads a file-static `s_verdict` that starts zeroed), so a zero here means "unknown", which is the opposite of what the guard does with it. Today the same zeroed struct also carries score 0, so peak stays 0 and the path is not reached - it is a latent hole rather than a live misreading, but it is one line from becoming live if any engine ever emits a score before a ceiling.

**Evidence.**
```c
        if (st->ceiling && st->ceiling < ceiling) {
            ceiling = st->ceiling;
        }
```

**Why it matters.** The engine's central promise is that no conclusion outruns its weakest input's confidence; this is the one input value for which it does the reverse, and it fails open rather than closed.

**Fix.** Refuse the observation instead of silently un-capping it: in pa_observe, `if (score > 0 && ceiling == 0) return;` with a comment saying an unknown ceiling is not an unlimited one. Then the guard at line 111 can drop the `st->ceiling &&` test entirely. Tighten the contract at pharos_lens.h:206 to say a lens must return false until its engine has produced a verdict, and add the host test: pa_observe(HARVEST, 90, 0) must leave the verdict CLEAR.

### [medium] ux — The per-stage hit count is maintained and never read
`components/pharos_engine/pharos_aegis.c:35`

**Wrong.** st->hits is incremented on every alarm-level observation and is the only writer or reader anywhere in the tree - nothing in pa_evaluate, pa_verdict_t, the rows, the display or the report ever looks at it. It is the field that would distinguish one burst from twelve bursts over an hour, which is exactly the distinction the INCIDENT advice implies the operator can make when it says "Note the times". pa_stage_meaning() is in the same position: a full set of plain-English stage explanations with no production caller, exercised only by test_aegis.c:179.

**Evidence.**
```c
        st->hits++;
```

**Why it matters.** A single 80 and a stage that has hit 80 on forty separate occasions render identically on every screen and in the report, so the operator cannot tell a one-off from a campaign - and the one screen that exists to tell a story keeps the story's repetition count in memory and never says it.

**Fix.** Add `uint32_t worst_hits;` to pa_verdict_t, fill it from the worst stage alongside worst_peak at line 114-118, render it as a row ("times raised" / "%u") and add `prt_u32(&w, "worst_hits", ...)` to the report. Use pa_stage_meaning(v.worst) as the detail text on the Aegis detail page, which is where an operator who does not know what IMPERSONATE means will be standing. If neither is wanted, delete hits rather than leave a maintained field with no reader.


## rival

**What it does.** Rival is a passive presence/capability lens: it identifies attack-adjacent hardware that announces itself (Flipper Zero by its 16-bit service UUID 0x3081-0x3083, deauther/Marauder/Pineapple/Pwnagotchi by name, OUI or Pwnagotchi whisper IEs, Espressif dev boards running open APs, O.MG-style implants by DE:4F:22) over BLE advertisements and Wi-Fi beacons, aggregates it by KIND rather than by address, and scores it CLEAR/NOTED/CAPABLE/IN USE - capping mere presence at 55 and reserving the ACTIVE band for four flood tests (raw advertisement rate, pairing-payload model diversity, pairing-payload address diversity, per-address pairing rate) plus a payload-independent RSSI-coherence test that says "many addresses, one radio".

**Prior art.** I pulled the actual payload builders rather than descriptions, because every finding below turns on byte offsets.

(1) Willy-JL / Spooks4576 / ECTO-1A "FlipperZero-BLE-Spam" (read via the dwgx/ble-spam-esp32-boost mirror, protocols/continuity.c, easysetup.c, fastpair.c, swiftpair.c). These are the exact packets every Flipper, Marauder, Bruce and Android clone sends:
  - Apple Nearby Action: [len][FF][4C 00][0F][05][flags][action][auth x3]. `flags` is hardcoded 0xC0, with only two exceptions (0xBF for action 0x20 half the time, 0x40 for action 0x09 half the time). The ACTION - the byte that picks which dialog appears, drawn from a ~20-entry table - is the NEXT byte.
  - Apple Proximity Pairing: [len][FF][4C 00][07][19][prefix][model_hi][model_lo][0x55][battery][charge][lid][colour]. `prefix` is 0x01 (or 0x05 for AirTag-ish models, 0x07 documented for "new device"); the 20+ cycled AirPods models live in the two bytes after it.
  - Samsung EasySetup Buds: [27][FF][75 00][42 09 81 02 14 15 03 21 01 09][model_hi][model_mid][01][model_lo]... - the first ten bytes after the company ID are protocol constants.
  - Samsung EasySetup Watch: [14][FF][75 00][01 00 02 00 01 01 FF 00 00 43][model] - model is the LAST byte, tagged by 0x43.
  - Microsoft Swift Pair: [len][FF][06 00][03][00][80][random display name].
  - Google Fast Pair: [6][16][2C FE][model_id x3] - service data length exactly 6.
(2) Google's Fast Pair spec (developers.google.com/nearby/fast-pair/specifications/service/provider) - the NON-discoverable frame is also service data 0xFE2C, but carries a version/flags byte 0x00, an account-key Bloom filter of (1.2n+3) bytes and a 2-byte salt, and the spec requires the RPA to rotate together with that payload. Every account-paired earbud in the room emits these continuously.
(3) ESP32Marauder (justcallmekoko) "BT Spam All" / Samsung BLE Spam wiki, and Bruce (pr3y) - same four families over NimBLE, MAC redrawn per advertisement.
(4) Samsung crowd-sourced location (USENIX Sec '23, arXiv 2210.14702): SmartTags/Galaxy offline finding use service UUID FD5A/FD59, NOT 0x0075 manufacturer data - so I did NOT report the "any 0x0075 is a pairing popup" acceptance as an airport false positive; I could not evidence that ordinary Samsung traffic looks like that, and I say so rather than guess.
(5) Detector-side prior art is thin and Pharos is ahead of it: the Flipper/Android "spam detector" tools key on a payload signature (Apple company ID + continuity type) plus an RSSI proximity threshold (-90 dBm default) and "following" over time. None of the surveyed detectors does a per-address rate test or an RSSI-coherence test. The coherence idea in this lens is genuinely novel relative to published work - which is exactly why the three bugs in it below matter.

Method note: I did not touch the connected board. Every finding below is reproduced on the host by compiling components/pharos_engine/pharos_rival.c against a probe harness (scratchpad), using the byte layouts above; each "failure_scenario" is a printed result, not a prediction.

### [high] bug — Reclaim stale slots in admit() instead of going permanently blind
`components/pharos_engine/pharos_rival.c:340`

**Wrong.** The device table is a session-lifetime budget with no reclamation. Entries are never freed: `in_use` is never cleared, `s->n` never decreases, and staleness is only applied as a display-time filter in prv_evaluate() and prv_device_at_now(). So the 24th distinct Wi-Fi tool address ever seen permanently closes the table, and every later device - including a Flipper two feet away - is silently dropped. Espressif smart plugs and ESPHome sensors classify as PRV_KIND_DEV_BOARD via prv_is_devboard_oui(), so ordinary furniture exhausts the table.

**Evidence.**
```c
    if (s->n >= PR_MAX_SIGHTINGS) {
        s->full = true;
        return NULL;
    }
```

**Why it matters.** Reproduced on the host: after 30 ordinary Espressif APs seen a minute apart (all long stale), a Flipper advertising the real 0x3082 UUID at -40 dBm every 500 ms for 20 s is never admitted. The verdict is n_devices=0, band=CLEAR, headline "Nothing announced itself to this receiver.", zero rows on the glass - the strongest possible false reassurance, produced by the detector's own bookkeeping rather than by the room. This is the house rule 'a detector must never read its own missing instrumentation as a finding' failing in its worst direction.

**Fix.** In admit(), when `s->n >= PR_MAX_SIGHTINGS`, scan for an entry where `stale_on_listen(d, s->listen_us)` is true and reuse the stalest one (memset + re-init), keeping `s->full = true` as the note. This is exactly the policy the pairing tables already use two hundred lines below ('Full is itself the finding ... so evict the stalest and keep counting rather than stop'). Only refuse admission when nothing in the table is stale, and in that case prefer evicting the lowest-kind_weight entry over dropping a FLIPPER/PINEAPPLE sighting. Add a host test: fill the table with expired DEV_BOARD beacons, then assert a fresh Flipper still reaches the verdict.

### [high] bug — pairing_code() reads a constant byte, not the model, for Apple and Samsung
`components/pharos_engine/pharos_rival.c:205`

**Wrong.** `*code = p[4]` is only the model/action byte for Google Fast Pair. Checked against the actual spam-tool packet builders: for Apple Nearby Action (0x0F) p[3] is the continuity length and p[4] is the ACTION FLAGS byte (hardcoded 0xC0 in every tool), with the action itself at p[5]; for Apple Proximity Pairing (0x07) p[4] is the PREFIX (0x01/0x05/0x07) and the cycled device model is at p[5..6]; for Samsung EasySetup Buds p[4] is the protocol constant 0x81 (model at p[12], p[13], p[15]); for Samsung EasySetup Watch p[4] is the protocol constant 0x02 (model at p[12], tagged by p[11]==0x43). The header states the opposite as fact: 'The byte after the length is the action or model, and that is the one that varies wildly under spam'.

**Evidence.**
```c
            if (company == 0x004C && (p[2] == 0x0F || p[2] == 0x07)) {
                *code = p[4];
                return true;
            }
...
            /* Samsung EasySetup. */
            if (company == 0x0075 && plen >= 5) {
                *code = p[4];
                return true;
            }
```

**Why it matters.** pair_models - the discriminator the header calls the right test for payload-cycling spam, and the one whose threshold is justified with 'which is also why it does not fire in an airport' - cannot fire on the real attack. Measured on the host with the exact tool layouts: 20 Nearby Action codes cycled gives pair_models=1; 12 AirPods models cycled gives pair_models=1; 20 Samsung Buds models cycled gives pair_models=1. Threshold is 6. Two consequences. (a) A tool that does NOT rotate its address is scored CLEAR: 40 Samsung Watch models from one address at 10/s reads models=1 addrs=1 worst=40 band=CLEAR. (b) When address rotation does save the detection, the face reports the wrong thing - lens_rival.c:322 falls through to 'one popup, %u faked senders' during a twenty-popup flood, because the models>=6 branch is unreachable. It also strongly suggests the field measurement recorded in the header ('three action codes from a single address') was never three actions: it is p[4] taking its three possible FLAG values 0xC0/0xBF/0x40.

**Fix.** Extract the real model per family: Apple 0x0F -> require plen>=6, code = p[5] (action type); Apple 0x07 -> require plen>=7, code = p[6] (low byte of the 16-bit device model); Samsung Buds -> require plen>=16 && p[2]==0x42 && p[3]==0x09 && p[4]==0x81, code = p[15]; Samsung Watch -> require plen>=13 && p[2]==0x01 && p[11]==0x43, code = p[12]; leave Microsoft (p[5] is the first byte of the randomised display name, which does vary) and Fast Pair alone. Tightening the Samsung branch to its two known prefixes also removes the current 'any 0x0075 of length >=5 is a pairing popup' acceptance, which is the same over-broad shape the Microsoft branch is deliberately narrowed against. Fix the host fixtures at test/host/test_rival.c:709 and :1216 at the same time - both currently encode the wrong offset, which is why this shipped.

### [high] bug — pair_addr_cnt is a lifetime total but is thresholded as a 4-second rate
`components/pharos_engine/pharos_rival.c:569`

**Wrong.** pair_addr_cnt[k] increments on every hit and is only ever reset when the slot is newly allocated or evicted. Nothing decays it, and the slot's timestamp is refreshed on every hit so it never expires while the device is present. prv_evaluate then compares that lifetime total against PRV_SPAM_PAIR_RATE, a threshold whose entire justification in the header is a four-second measurement ('thirty in the cafe, eighty-eight in the measured attack').

**Evidence.**
```c
                        s->pair_addr_us[k] = t_us;
                        if (s->pair_addr_cnt[k] < 0xFFFFu) {
                            s->pair_addr_cnt[k]++;
                        }
...
                           (out->pair_models >= PRV_SPAM_MODELS ||
                            out->pair_addrs >= PRV_SPAM_ADDRS ||
                            out->pair_worst_addr >= PRV_SPAM_PAIR_RATE);
```

**Why it matters.** The engine fires on the exact negative it was built to refuse, given a few more seconds of standing still. Reproduced on the host with the shipped cafe fixture (three accessories, stable addresses, one Apple model each, 25 advs/s total) run for 30 s instead of 3.6 s: pair_advs=75, models=3, addrs=3, pair_worst_addr=250, PRV_NOTE_PAIR_SPAM set, band IN USE, score 70. Any single accessory emitting a 0x07/0x0F payload at >=5/s trips this after about twelve seconds. The shipped test at test_rival.c:358 passes only because it stops the clock at 90 advertisements.

**Fix.** Make the count windowed like everything else in that struct: give each pair_addr slot the same per-second bucket treatment the global counter already has (uint16_t cnt[4] plus a uint32_t sec per slot, index sec%4), and in prv_evaluate sum only the buckets whose second is within PRV_SPAM_WINDOW_US. Cheapest correct alternative if RAM is tight: store (first_us, cnt) per slot and reset both when now-first_us exceeds the window, so the reading is always 'advertisements from this address in the current window'. Then extend the cafe fixture to 30 s and assert it still does NOT alarm - that assertion is the real regression guard.

### [high] dishonesty — peak_adv_per_s has no timestamps, so a finished flood is reported forever
`components/pharos_engine/pharos_rival.c:1054`

**Wrong.** adv_distinct[8] carries counts with no time attached. A bucket is zeroed only when its slot is recycled, which requires a further advertisement in a later wall second. When advertising stops - which is what an attacker leaving looks like - the buckets freeze at their peak and prv_evaluate keeps reading them.

**Evidence.**
```c
    for (unsigned i = 0; i < 8; i++) {
        if (s->adv_distinct[i] > out->peak_adv_per_s) {
            out->peak_adv_per_s = s->adv_distinct[i];
        }
    }
```

**Why it matters.** Reproduced on the host: 120 advertisements in one second, then ten minutes of listening with nothing heard, still evaluates as peak=120/s, PRV_NOTE_SPAM set, band IN USE, score 66, headline 'Advertisement flooding in progress. Something is being run.' with n_devices=0. The lens is asserting a present-tense attack from evidence ten minutes old, and row 0 of the glass ('advertisers/sec 120', tone BAD) corroborates it. Even with intermittent traffic the peak survives eight distinct wall seconds of it, so a burst can be re-reported a minute later. This is the oscillation lesson from PRV_FLOOD_HOLD_US applied in the wrong direction: the reading is stable because it is stuck.

**Fix.** Store the second each bucket represents (uint32_t adv_sec_of[8], written when the slot is claimed) and in prv_evaluate skip any bucket where `now_sec - adv_sec_of[i] >= 8`. That is four bytes per slot and makes 'advertisers/sec' mean what the row label says. Also note the field name and both comments say DISTINCT advertisers while line 539 increments once per advertisement regardless of address - either count distinct addresses per bucket or rename the field and the row, because 60 raw advertisements a second is reachable in a crowded foyer and the 'a busy cafe sits far below this' defence is written for the distinct-address reading.

### [high] dishonesty — The coherence cluster is only expired on BLE ingest, so prv_evaluate can read arbitrarily old evidence
`components/pharos_engine/pharos_rival.c:957`

**Wrong.** cohere_expire() is called only from cohere_note(), i.e. only when a BLE advertisement arrives. cohere_peak() takes no time argument and filters nothing, so prv_evaluate scores whatever is still sitting in coh_rssi[] no matter how old it is. PRV_COHERE_WINDOW_US is enforced only on the ingest path.

**Evidence.**
```c
        coh_tight = cohere_peak(s, &coh_at);
        flood_in_view = s->coh_flood_us &&
                        (now_us >= s->coh_flood_us) &&
                        (now_us - s->coh_flood_us) < PRV_FLOOD_HOLD_US;
```

**Why it matters.** The eight-second PRV_FLOOD_HOLD_US bound governs flood_in_view but not the block at line 1187 that actually sets NOTE_COHERENT, PRV_FAM_ACTIVE, PRV_NOTE_NAMES_FORGED and the 66/74 score floors. Reproduced on the host: a 30-address coherent flood, then the BLE attacker leaves while a Pwnagotchi keeps beaconing (so the early exit at line 1125 cannot fire and no BLE advertisement arrives to trigger expiry). Five minutes later the verdict is still coherent=1, cohere_addrs=30, names=4, NAMES_FORGED set, band IN USE, score 74, 'Advertisement flooding in progress.' - and because NAMES_FORGED is set, row 9 tells the operator that the Pwnagotchi's name on screen is the attacker's. A six-second window reported five minutes later is precisely the claim the house rules forbid.

**Fix.** Give cohere_peak a `uint64_t now_us` parameter and skip entries where `now_us - s->coh_us[i] >= PRV_COHERE_WINDOW_US`; do the same for the name count (see the separate finding). The ingest-side call passes t_us and behaves identically, so the latch is unchanged. prv_evaluate is const-correct already - this needs no state mutation. Host test: build the cluster, advance now_us by 60 s with no ingest, assert NOTE_COHERENT is gone.

### [high] blind-spot — The coherence table has no eviction, so a crowded room starves the only payload-independent test
`components/pharos_engine/pharos_rival.c:461`

**Wrong.** New addresses are added only while `s->coh_n < PRV_COHERE_SLOTS`; when the 64 slots are full, fresh addresses are dropped rather than replacing the stalest. An address that keeps being heard refreshes its own timestamp (`s->coh_us[i] = t_us`) and therefore never expires, so a population of steady advertisers holds every slot indefinitely.

**Evidence.**
```c
    if (fresh_addr && s->coh_n < PRV_COHERE_SLOTS) {
        s->coh_addr_h[s->coh_n] = h;
        s->coh_rssi[s->coh_n] = rssi;
        s->coh_us[s->coh_n] = t_us;
        s->coh_n++;
    }
```

**Why it matters.** Reproduced on the host: 64 steady advertisers spread across -30..-93 dBm, each heard every ~2 s, then a 40-address flood arrives all at -50 dBm. Result: coherent=0, cohere_addrs=6, band CAPABLE - the flood is invisible. Sixty-four distinct BLE advertisers inside a six-second window is an ordinary train carriage or airport gate, and this is the one test the header describes as 'the only one of these that can see a flood nobody has catalogued yet'. The pairing-address table two hundred lines away already gets this right and says why: 'Full is itself the finding - a spammer produces far more addresses than this holds - so evict the stalest and keep counting rather than stop.'

**Fix.** Apply the same eviction to coh_*: when full and the address is fresh, replace the entry with the oldest coh_us (a 64-entry linear scan, the same cost as the existing cohere_peak double loop). Because a genuine flood draws fresh addresses far faster than bystanders repeat, eviction preferentially clears the steady bystanders. Host test: the scenario above must reach NOTE_COHERENT.

### [high] bug — Wi-Fi tools of the same kind collapse to one in the verdict while the list row counts them
`components/pharos_engine/pharos_rival.c:1013`

**Wrong.** n_devices, n_addresses, n_flipper, n_pwnagotchi and n_wifi_tools are all incremented inside the `!kind_seen[d->kind]` guard, so every counter is per-KIND. That is right for BLE, where admit() deliberately folds rotating addresses into one entry, but wrong for Wi-Fi, where admit() keys on BSSID precisely because (its own comment) 'BSSIDs do not rotate, two deauther boards really are two, and keying on the address there is both safe and more precise'. The extra precision is then discarded, and n_addresses takes only the FIRST entry's address count.

**Evidence.**
```c
        if (d->kind < PRV_KIND_COUNT && !kind_seen[d->kind]) {
            kind_seen[d->kind] = true;
            out->n_devices++;
            out->n_addresses = (uint16_t)(out->n_addresses + d->addresses);
```

**Why it matters.** Reproduced on the host with five distinct Pwnagotchi BSSIDs: the verdict says n_devices=1 and n_addresses=1, while prv_device_at_now aggregates correctly and the list row renders '5 addr'. On the glass that is row 3 'hardware identified 1' and row 5 'addresses used 1' sitting directly above a row that says 5 - the self-contradiction this file has already been fixed for twice (see the comments at lines 1004 and 1283). It also understates a real finding: five handshake collectors in range is a different situation from one.

**Fix.** Count Wi-Fi entries individually and BLE entries per kind: keep kind_seen only for `d->ble` entries, and for `!d->ble` increment n_devices / n_wifi_tools / n_pwnagotchi per entry. Move the n_addresses accumulation out of the guard entirely so it sums every surviving entry (that is what prv_device_at_now already does with `agg.addresses = agg.addresses + d->addresses`). Then assert count==rows for the five-Pwnagotchi case in the host tests, which is the invariant test_rival_switched_off_everywhere already asserts for staleness.

### [high] ux — cohere_addrs/names/rssi are computed and then thrown away by the early exit
`components/pharos_engine/pharos_rival.c:1125`

**Wrong.** coh_tight and coh_at are computed at the top of prv_evaluate, but out->cohere_addrs / cohere_names / cohere_rssi are only assigned in the block at line 1183, below this early return. Whenever nothing else is interesting, the verdict carries zeros for a number the engine already measured.

**Evidence.**
```c
    if (out->n_devices == 0 && out->peak_adv_per_s < 60u && !pair_spam &&
        !flood_in_view) {
        return;
    }
```

**Why it matters.** Reproduced on the host: ten addresses clustered inside 1 dB reports cohere_addrs=0. Rows 7 and 8 of the glass ('one radio, addresses' and 'names it is wearing') therefore read 0 at all times until the alarm fires, then jump to 14+. The operator cannot watch the cluster build, cannot sanity-check the reasoning the header says these rows exist for ('reported so the operator can check the reasoning rather than take "one radio" on trust'), and cannot distinguish 'nothing is clustering' from 'the test did not run'. It also makes the two rows useless as a field diagnostic for exactly the measured failure that motivated the coherence test.

**Fix.** Assign out->cohere_addrs / cohere_names / cohere_rssi (and, while you are there, out->pair_worst_addr) immediately after the counts are computed, before the early return. The band and score logic is unaffected; only the instrumentation reaches the screen earlier.

### [medium] bug — Fast Pair non-discoverable frames are counted as pairing popups
`components/pharos_engine/pharos_rival.c:223`

**Wrong.** The 0xFE2C branch accepts any service-data block of plen>=5 and takes p[4] as the model. A discoverable Fast Pair frame is exactly `06 16 2C FE <model x3>` (plen==5). A NON-discoverable frame is the same UUID carrying a 0x00 version/flags byte, an account-key Bloom filter and a 2-byte salt - so p[4] is a filter byte, which changes whenever the salt is regenerated, and Google's spec requires the RPA to rotate with that payload.

**Evidence.**
```c
        if (type == 0x16 && plen >= 5) {
            const uint16_t uuid = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
            if (uuid == 0xFE2C) { /* Google Fast Pair */
                *code = p[4];
                return true;
            }
        }
```

**Why it matters.** Every account-paired Fast Pair earbud in range broadcasts these continuously. Eight of them in a gate lounge supplies pair_addrs>=PRV_SPAM_ADDRS with pair_advs well over 20, and their filter bytes supply spurious model diversity - which is the airport false positive the header explicitly claims this design avoids ('which is also why it does not fire in an airport'). Because the two Apple families and both Samsung families currently contribute almost no diversity (see the pairing_code finding), Fast Pair frames are disproportionately what pair_models actually counts today.

**Fix.** Require `plen == 5` for the 0xFE2C branch - that is the discoverable model-ID frame and nothing else - or equivalently reject when `p[2] == 0x00` (version 0 / non-discoverable). Both forms are one line and cost nothing. Add a host fixture built from a non-discoverable frame (2C FE 00 <akf bytes> <salt>) that asserts pair_advs stays 0.

### [high] dishonesty — The names credited to "one radio" are collected room-wide, not from the level cluster
`components/pharos_engine/pharos_rival.c:481`

**Wrong.** cohere_note() records every advertised name it sees in the window into a flat list, with no reference to the address's signal level or to the cluster. prv_evaluate then reports that flat count as the number of names the coherent radio is wearing. The struct comment in the header describes something else: 'addresses bucketed by the signal level they arrived at, and the names seen inside the strongest bucket'.

**Evidence.**
```c
        if (fresh_name && s->coh_n_names < PRV_NAME_SLOTS) {
            s->coh_name_h[s->coh_n_names] = nh;
            s->coh_name_us[s->coh_n_names] = t_us;
            s->coh_n_names++;
        }
...
        out->cohere_names = s->coh_n_names;
```

**Why it matters.** Reproduced on the host: a nameless 20-address flood at -50 dBm plus three innocent named devices at -20, -50 and -80 dBm yields cohere_names=3, PRV_NOTE_MANY_NAMES, score 74. The glass then says 'names it is wearing: 3' in BAD tone, and the 66->74 bump is carried by bystanders' device names. The flood detection itself is sound here, but the specific claim - one radio is wearing several identities - is assembled from evidence about other radios, which is the forgeable-evidence rule inverted: names the attacker never sent are raising the score.

**Fix.** Scope names to the cluster. Cheapest version that preserves the privacy design: store the arriving RSSI alongside each name hash (int8_t coh_name_rssi[PRV_NAME_SLOTS]), and in prv_evaluate count only names whose level is within PRV_COHERE_DB of the coh_at the peak was found at. Apply the same window filter as the addresses (see the expiry finding). If that is judged too loose, keep the flat list for diagnostics but gate PRV_NOTE_MANY_NAMES and the 74 floor on the cluster-scoped count only.

### [medium] ux — pair_worst_addr never reaches the glass, so the rate finding is described as its opposite
`components/pharos_lens_rival/lens_rival.c:322`

**Wrong.** pair_worst_addr is one of the three triggers for PRV_NOTE_PAIR_SPAM and is the only one that fires for a single-address flood, but it is never rendered anywhere. The display falls back to the address-diversity wording, and row 6 shows the address count in DIM tone because it is below its own threshold.

**Evidence.**
```c
            snprintf(o->why, sizeof(o->why), "one popup, %u faked senders",
                     (unsigned)v.pair_addrs);
```

**Why it matters.** In the measured iOS case the engine's own tests encode (88 popups from one address in four seconds), the operator is told 'one popup, 1 faked senders' while row 6 reads 'faked senders 1' in DIM and row 4 reads '1/88'. The evidence that actually raised the alarm - one radio hammering a dialog 22 times a second - is nowhere on the screen, so the face looks like it is alarming on nothing, which is the same credibility cost the file has already paid twice for contradictory rows.

**Fix.** Add a third branch in the NOTE_PAIR_SPAM block: when `v.pair_worst_addr >= PRV_SPAM_PAIR_RATE` and pair_addrs is small, say '%u popups from one radio'. Optionally make row 6 fall back to showing pair_worst_addr with BAD tone in that case. No engine change is needed once pair_worst_addr is assigned before the early return.


## spectrum

**What it does.** Spectrum is a lens-only tool (no engine): it hops 1..13 at 120 ms with mgmt+data delivery, tallies decoded 802.11 frames/beacons/peak-RSSI per channel into a "current visit" table, promotes that table to a "last completed visit" table when the radio publishes PHAROS_EV_DWELL, and presents the busiest channel as a headline plus a 14-row channel map with an 8-step ASCII bar — explicitly refusing to raise an alarm on the grounds that "a crowded band is not an attack".

**Prior art.** Four comparables, all of which measure airtime rather than count frames.

1. **horst** (github.com/br101/horst, `ieee80211_duration.c`) — the closest open-source analogue: a hopping passive 802.11 monitor that draws a per-channel "used %" bar. It does NOT count frames; it computes each frame's true on-air microseconds and sums them. OFDM: `dur = 20 + 4 * DIV_ROUND_UP((16 + 8*(len+4) + 6) * 10, 4 * rate)`; CCK/DSSS: `dur = short_preamble ? (72+24) : (144+48)` plus `DIV_ROUND_UP(8*(len+4)*10, rate)`; then it adds the access overhead (SIFS 16/10 us, slot 9/20 us, DIFS = sifs + 2*slot for data, AIFS per access category for QoS, plus `cw = 2^(cw_min+retries) - 1` halved). This matters because a 1 Mbps CCK beacon occupies ~2 ms and a 65 Mbps HT data frame ~100 us — a frame count is wrong by a factor of 20 between them, always in the direction that makes legacy/management-heavy channels look idle. Pharos's radio already admits this: "Without per-frame duration from the driver, count each decoded frame as a nominal slot" (`s.dwell_busy_us += 400`), but the driver DOES expose it — `wifi_pkt_rx_ctrl_t` on ESP32-S3 carries `rate` (5 bits), `sig_mode`, `mcs`, `cwb` and `sgi`, which is everything horst's formula needs.

2. **802.11e/WMM BSS Load ("QBSS Load") element, IE 11** — the single biggest gap. Element ID 11, length 5: station count (2 octets LE), **channel utilization (1 octet, 0-255 linearly scaled to 0-100% of time the AP's own carrier sense saw the medium busy)**, available admission capacity (2 octets, units of 32 us/s). It rides in EVERY beacon and probe response of a WMM AP, and is averaged over ~50 beacon intervals (~5 s) by a radio that sits on that channel at 100% duty. This is exactly the house rule "positive evidence about a frame that ARRIVED beats an inference from frames that did not": instead of extrapolating occupancy from a 120 ms sample at ~1/13 duty, Pharos can read a 5-second 100%-duty measurement out of a beacon it already parsed. It is attacker-forgeable, so per house rules it may only be shown attributed ("ch6: AP reports 71% busy") and may never lower Pharos's own reading. Pharos already walks the IE chain (`pharos_dot11_ie_count`, `rsn_flags`), so this is one more element case and one spare byte on `pharos_ev_dot11_t`.

3. **`iw dev … survey dump` / mac80211 survey** — the canonical utilisation measurement: per-frequency "channel active time", "channel busy time", "channel receive time" in ms, straight from MAC counters. The ESP32 exposes no equivalent registers, which is why `ev.u.dwell.noise_floor = 0` with the comment "No true noise-floor register is exposed". That comment is wrong for this part: ESP-IDF's `esp_wifi_types_native.h` gives ESP32-S3 `signed noise_floor: 8; /* noise floor of Radio Frequency Module(RF). unit: dBm */` in `wifi_pkt_rx_ctrl_t` (the non-HE branch, which S3 uses). It is a per-received-frame estimate, not an idle-channel floor, so it must be labelled as such — but it is real instrumentation, and it is currently thrown away while the spectrum lens holds a hard-coded -95.

4. **Non-Wi-Fi interference identification** (Cisco CleanAir / Spectrum Expert, MetaGeek Chanalyzer, MERL TR2014-018 "Classification of Wireless Interference on 2.4GHz Spectrum"). Every one of them keys on the same three-way combination: high measured medium utilisation, high CRC/FCS error rate, and low successful-decode rate — because a receiver cannot demodulate a non-802.11 emitter, so the only visible signature is "busy but nothing decodes". Household microwave ovens run ~50% duty over a 16.67 ms (60 Hz) or 20 ms (50 Hz) period; Bluetooth/BLE, analogue video senders and cordless phones straddle multiple channels. Pharos's sibling lens Squall implements this correctly (`broken_permil >= 250 && fcs_fail_total >= 40`), and Pharos's radio already tallies FCS failures in the promiscuous callback and drops the bytes — the count costs the bus nothing. Spectrum, the lens that tells the operator which channel to camp on, does not request it and does not read it.

### [high] dishonesty — Stop the airtime-share score from firing the audible alarm
`components/pharos_lens_spectrum/lens_spectrum.c:222`

**Wrong.** The lens publishes `has_score = true` with a score that is one channel's SHARE of all frames heard, and correctly declares `has_alert` so the ring and the face colour do not read that share as a threat. But `alarm_pump()` in pharos_ui.c never looks at `has_alert` — it bands purely on `d->score` and plays a sound on every rising edge. Spectrum's share score is 100 whenever one AP dominates, which is the normal case in a flat, so the lens that the CHANGELOG says "cannot raise an alarm at all" plays the top alarm tone at a working router. The has_alert fix reached tower_state_of() (ring severity) and pharos_hud.c:1361 (face colour) and stopped at the speaker.

**Evidence.**
```c
lens_spectrum.c:222-234 — `o->score = (uint8_t)((total_frames ? (best * 100u) / total_frames : 0u));` … `/* Spectrum is a picture to go and look at, not a watch that raises anything. */ o->has_alert = true; o->alert = (o->score >= 80u) ? 1u : 0u;`
pharos_ui.c:1351-1369 — `if (!active || !d || !d->has_score) { return; } … if (d->score >= 75) band = 4; … if (band > s_alarm_band) { pharos_audio_alert(pharos_audio_alert_for_band(band)); }`
pharos_audio.c:285 — `case 4:  return PHAROS_ALERT_ALARM;   /* LIKELY */`
CHANGELOG.md:459 — "Spectrum and Sentinel cannot raise an alarm at all"
```

**Why it matters.** Flat with one router on ch 6. Every frame heard is on ch 6, so best == total_frames and score == 100 → band 4, adopted silently on entry. A neighbour's AP on ch 1 is caught during one visit, share dips to ~60 (band 3); next sweep it is back to 100 → rising edge into band 4 → PHAROS_ALERT_ALARM sounds. In a block of flats the share oscillates across the 75 boundary continuously, so the device shrieks at nothing — the exact failure the alarm_pump comment warns about ("a device that shrieks continuously gets muted, and a muted alarm is worse than no alarm because it is still trusted"). Secondary: `ps_alert_colour(1)` is PS_WARN 0xFFC34A, so the reported "photograph showed something is up in orange" regression still reproduces above 80, just at a higher threshold.

**Fix.** Make alarm_pump honour the same contract the ring does. In pharos_ui.c:1346, before computing the band: `if (d->has_alert) { band = (d->alert >= 3) ? 4 : (d->alert == 2) ? 3 : (d->alert == 1) ? 1 : 0; } else { …existing score bands… }` — alert 1 ("worth knowing") must map below the SUSPECT/ALARM tones. Independently, Spectrum should stop exporting a non-threat number in the field every UI consumer treats as 0..100 threat: set `o->has_score = false`, put the share in `o->detail` ("ch6 holds 78% of what I heard"), and let `big` carry the channel number it already carries. Add a host check in test/host asserting that for every lens with `has_alert && alert <= 1`, alarm_pump's band is <= 1.

### [high] bug — Read the DWELL event's own frame count instead of the post-drop tally
`components/pharos_lens_spectrum/lens_spectrum.c:104`

**Wrong.** The DWELL handler reads exactly one field — `.channel` — and throws away `frames`, `dwell_ms`, `busy_permil`, `peak_rssi`, `retries` and `fcs_fail`, substituting its own tally counted from events that survived the bus. The radio increments `s.dwell_frames++` inside the promiscuous callback, BEFORE `pharos_bus_push`, so `ev.u.dwell.frames` is the loss-free count and `s_cur[ch].frames` is the after-drop count. Worse, the DWELL push is best-effort on a drop-newest ring and is issued at peak ring fill (immediately after the burst it summarises). If it is dropped, spectrum_event never runs the copy OR the memset, so `s_cur[dc]` keeps accumulating and the next visit reports two visits' frames as one.

**Evidence.**
```c
lens_spectrum.c:104-111 — `if (ev->type == PHAROS_EV_DWELL) { const uint8_t dc = ev->u.dwell.channel; if (dc >= 1 && dc <= PHAROS_CHAN_MAX) { s_now[dc] = s_cur[dc]; memset(&s_cur[dc], 0, sizeof(s_cur[dc])); } return; }`
pharos_radio.c:439 — `s.dwell_frames++;` (in promisc_cb, before `pharos_bus_push`)
pharos_radio.c:149 — `ev.u.dwell.frames = (uint16_t)(s.dwell_frames > 0xFFFFu ? 0xFFFFu : s.dwell_frames);`
pharos_radio.c:170 — `pharos_bus_push(s.bus, &ev);` (return value ignored)
pharos_bus.h — "Overflow policy is drop-newest *and count*. That count is not a debug statistic - it is evidence."
```

**Why it matters.** Spectrum runs the highest-volume plan in the project — `want_mgmt` AND `want_data` at 120 ms, against a 512-slot ring (Census is 350 ms mgmt-only, Squall 500 ms). On a saturated channel the ring overflows, so the lens under-counts precisely the channel it is about to name BUSIEST, and the share score it feeds the UI is biased downward there. Then the DWELL for that channel is dropped, `s_cur[6]` is never cleared, and the following sweep reports channel 6 at roughly double — a channel map that reads 2x high on the one channel an operator is deciding whether to camp on. The lens also never reads `pharos_bus_dropped(&s_bus)`, which bus.h says "every engine is expected to widen its uncertainty" from.

**Fix.** Take the numbers from the event, as lens_squall.c:88-97 already does: in the DWELL branch set `s_now[dc].frames = ev->u.dwell.frames; s_now[dc].peak_rssi = ev->u.dwell.peak_rssi;` carry `ev->u.dwell.busy_permil` and `ev->u.dwell.dwell_ms` into new fields, keep `beacons` from the per-frame path (the event has no beacon count) but mark it delivered-only, and `memset(&s_cur[dc], 0, …)` unconditionally so a dropped DWELL costs one stale refresh instead of a doubled count. Then drive the bar and the `busy` byte from `busy_permil` — a value normalised by the visit's real length — rather than from a raw count compared across visits of different duration.

### [high] dishonesty — Never print "quiet" for a channel the receiver has not visited
`components/pharos_lens_spectrum/lens_spectrum.c:272`

**Wrong.** The channel map always emits PHAROS_CHAN_MAX (14) rows and labels any row with zero frames "quiet", but the scan plan is built from `pharos_region_max_channel()`, so channels above the region's limit are never tuned at all. `pharos_region.h` is #included by this lens and then never called. The lens is reporting a positive fact about air it has not sampled once — the exact house-rule violation ("a detector must never read its own missing instrumentation as a finding").

**Evidence.**
```c
lens_spectrum.c:24 — `#include "pharos_region.h"` (no pharos_region_* symbol appears anywhere in the file)
lens_spectrum.c:242 — `if (index >= PHAROS_CHAN_MAX) { return false; }` with pharos_radio.h:48 `#define PHAROS_CHAN_MAX 14`
lens_spectrum.c:272 — `snprintf(out->right, sizeof(out->right), "quiet");`
pharos_radio.c:88-91 — `const uint8_t hi = pharos_region_max_channel(); for (uint8_t c = PHAROS_CHAN_MIN; c <= hi && p.n_channels < PHAROS_CHAN_MAX; c++) { p.channels[p.n_channels++] = c; }`
pharos_region.c:8-12 — FCC returns 11, WORLD (the default) returns 13
```

**Why it matters.** Shipped default is PHAROS_REGION_WORLD, so channel 14 is never tuned and its row permanently reads `ch 14 ........   0 ap` / `quiet`. Set region FCC and channels 12, 13 and 14 all read "quiet" while the receiver has never been near them — and an operator in Europe reading an FCC-configured unit would conclude 12/13 are free and site an AP there. The same wording also fires for the first ~1.5 s after start, before any channel has completed a visit: at that moment the device asserts all thirteen channels are quiet having heard none of them.

**Fix.** Use the include that is already there. Cap the row count at `pharos_region_max_channel()` so out-of-plan channels are not listed at all, and add a `bool visited` to spec_cell_t set in the DWELL branch; for a channel in the plan that has not completed a first visit print `--` or `unseen` with PHAROS_TONE_DIM instead of `quiet`, and leave its bar blank. A host test should assert that for each region, no row index maps to a channel outside `pharos_scan_plan_survey()`'s channel list, and that an unvisited cell never renders the string "quiet".

### [high] blind-spot — Request and display FCS failures so a non-Wi-Fi emitter is not read as a free channel
`components/pharos_lens_spectrum/lens_spectrum.c:80`

**Wrong.** Every quantity Spectrum measures comes from successfully decoded 802.11 frames. A microwave oven, analogue video sender, BLE piconet or CW jammer occupies the medium and produces no decodable frames, so the affected channel shows FEWER frames, a shorter bar and PHAROS_TONE_DIM — and the lens then advises camping on the loudest DECODABLE channel. The interference reads as emptiness, and emptiness reads as a recommendation. The signal exists and is free: `want_fcsfail` opens WIFI_PROMIS_FILTER_MASK_FCSFAIL, the callback tallies broken frames and drops the bytes (so the bus cost is zero), and the count already rides the DWELL event this lens receives.

**Evidence.**
```c
lens_spectrum.c:80-87 — `pharos_scan_plan_t plan = pharos_scan_plan_survey(); plan.dwell_ms = 120; plan.want_mgmt = true; plan.want_data = true;` (no `plan.want_fcsfail`)
lens_spectrum.c:112-126 — every cell update is guarded by `if (ev->type != PHAROS_EV_DOT11) { return; }`
lens_spectrum.c:218 — `snprintf(o->advice, sizeof(o->advice), "Camp here to hear it properly.");`
pharos_radio.c:248-251 — `if (pkt->rx_ctrl.rx_state != 0) { s_fcs_fail++; return; }`
pharos_event.h:177-186 — "It is the difference between 'frames are not arriving' and 'frames are arriving corrupted', and only the second is direct evidence of interference"
```

**Why it matters.** Stand next to a running microwave oven (~50% duty over a 16.67/20 ms period, centred near ch 8-11). Decodes on those channels collapse. Spectrum's bar for ch 9 shrinks to nothing, the row goes DIM and — because `best` moves to ch 1 — the headline sends the operator to camp on a channel that is not the problem, while the channel that IS jammed is presented as the quietest in the band. This is not duplicating Squall's verdict: Squall grades denial; Spectrum's job is the map every other lens is read against, and a map that marks a jammed channel as empty is worse than no map.

**Fix.** Set `plan.want_fcsfail = true;` in spectrum_start(). Add `uint16_t fcs_fail;` to spec_cell_t and take it from `ev->u.dwell.fcs_fail` in the DWELL branch (alongside the frame count from finding 2). Show it in the row: `ch 9 ........   0 ap` with right column `48 bad` in PHAROS_TONE_WARN whenever `fcs_fail * 4 > fcs_fail + frames` (the same 250-per-mille ratio Squall uses at pharos_squall.c:222). And suppress the camp advice — or qualify it — for any channel whose broken share is high: "ch 9: heard 48 broken, 0 decoded - something non-Wi-Fi may be here." That is a positive claim about frames that arrived, not an inference from silence.

### [high] dishonesty — Stop counting the scanner's own hop events in the activity ribbon
`components/pharos_lens_spectrum/lens_spectrum.c:101`

**Wrong.** `pharos_pulse_note()` is called before the PHAROS_EV_DWELL early return, so every channel-change summary the radio emits is counted as "activity" in the 16-second ribbon. Every other frame-counting lens filters first and notes second. The ribbon normalises against its own peak, so on a genuinely dead band the only events in the window are the scanner's own ~8.3 DWELLs per second — every slot holds the same value, the peak equals that value, and all sixteen slots normalise to 255: a full-height ribbon claiming maximum activity in an empty room.

**Evidence.**
```c
lens_spectrum.c:101-111 — `pharos_pulse_note(&s_pulse, ev->t_us);` placed above `if (ev->type == PHAROS_EV_DWELL) { … return; }`
lens_census.c:114-120 — `if (!ev || ev->type != PHAROS_EV_DOT11) { return; } pharos_pulse_note(&s_pulse, ev->t_us);` (same in lens_mirage.c:63-69 and lens_probe.c:61-67)
pharos_pulse.c:119 — `out[i] = (uint8_t)(((uint32_t)buf[i] * 255u) / peak);`
pharos_pulse.h — "a flat ribbon is a claim that the room was quiet and an empty one is an admission that nothing was measured"
```

**Why it matters.** 13 channels at `dwell_ms = 120` is a 1.56 s sweep, so the radio emits ~8.3 DWELL events per second forever, independent of traffic. Switch to Spectrum somewhere with no 2.4 GHz activity: the face says `--` / `listening` / `no traffic heard yet`, and directly above it the ribbon draws sixteen full-height bars. Even with light traffic — five frames a second — more than half of every bar is the device counting its own hops, so the shape the ribbon exists to show (burst vs trickle) is buried under a constant self-generated floor.

**Fix.** Move the `pharos_pulse_note(&s_pulse, ev->t_us);` call below the DWELL branch, immediately after the `if (ev->type != PHAROS_EV_DOT11) { return; }` guard, matching lens_census.c. A dead band then leaves the pulse unprimed, `pharos_pulse_fill` returns false, `has_history` is false and the HUD draws no ribbon — which is the documented and correct statement. Add a host check that feeds a pulse only DWELL-shaped events and asserts `has_history == false`.

### [high] dishonesty — Put the "deaf above 2.4 GHz" disclaimer somewhere permanent
`components/pharos_lens_spectrum/lens_spectrum.c:210`

**Wrong.** The file header and the README both state that this lens carries the 2.4-GHz-only disclaimer permanently on screen. It does not. The only string on the face that mentions the band lives in the `if (!total_frames)` branch and is replaced the instant one frame arrives; from then on the face reads BUSIEST CH / n frames n beacons n dBm / "Camp here to hear it properly" and says nothing about the band at all. The quiet-branch string does not make the point either — "Sweeping 2.4 GHz." is a statement about what it is doing, not an admission of what it cannot hear.

**Evidence.**
```c
lens_spectrum.c:7-10 — "It also carries the single most important honest disclaimer in the product, on screen, permanently: this radio hears 2.4 GHz only. Most modern office and home traffic has moved to 5 and 6 GHz, where this device is deaf. A quiet waterfall is not a quiet building."
lens_spectrum.c:206-212 — `if (!total_frames) { … snprintf(o->advice, sizeof(o->advice), "Sweeping 2.4 GHz."); … }`
lens_spectrum.c:214-218 — the populated branch: `"BUSIEST CH"` / `"%u frames  %u beacons  %d dBm"` / `"Camp here to hear it properly."`
README.md:155 — "States, permanently, that it is deaf above 2.4 GHz."
Grep for "5 GHz", "6 GHz" or "deaf" across components/pharos_ui/ and components/pharos_lens_spectrum/ returns only comments, never a display string.
```

**Why it matters.** The case the disclaimer exists for is an operator looking at a sparse map in a modern office and concluding the building is quiet. That is precisely the case where `total_frames` is non-zero — a handful of legacy beacons on ch 1 and 11 while every real client is on 5 GHz — so the disclaimer has already been replaced by advice to go camp on the emptiest band in the building. The one moment it is shown (`total_frames == 0`) is the one moment the operator can already see there is nothing there.

**Fix.** Put it in a row, not in the advice line — rows are permanent and the advice line is not. Emit index 0 from k_spectrum_row as a fixed header row: left `2.4 GHz only`, right `5/6 deaf`, `tone = PHAROS_TONE_DIM`, and shift the channel rows to index 1..n (adjusting the `index >= …` bound). Additionally set `o->band` to `"BUSIEST CH (2.4)"` so the live face carries the band at all times, and change the quiet-branch advice to say the thing that matters: `"2.4 GHz only - a quiet band is not a quiet building."`

### [high] ux — Give Spectrum the camp control its own advice tells the operator to use
`components/pharos_lens_spectrum/lens_spectrum.c:218`

**Wrong.** The face's advice is "Camp here to hear it properly." — and there is no control anywhere that camps this lens. The centre tap in VIEW_LIVE dispatches to `live->on_select()`, and the lens table has no `.on_select` member. The console `scan` command is declared with min 0 / max 0 arguments and calls run_scan with `argc = 0, argv = NULL`, so `scan camp 6` is rejected by the arg check and would be discarded even if it were not. Watch, Ward, Footprint and System all wire the tap; Spectrum is the lens that asks for it and does not have it.

**Evidence.**
```c
lens_spectrum.c:218 — `snprintf(o->advice, sizeof(o->advice), "Camp here to hear it properly.");`
lens_spectrum.c:280-299 — the `k_spectrum` initialiser: no `.on_select` member (cf. lens_watch.c:659 `.on_select = watch_select,`)
pharos_ui.c:1126-1136 — `if (s_view == VIEW_LIVE) { … const pharos_lens_t *live = pharos_lens_active(); if (live && live->on_select) { live->on_select(); } return; }`
pharos_console.c:403 — `{ "scan", "scan", "2.4 GHz airtime waterfall (Spectrum)", PC_CAT_SCAN, 0, 0, cmd_scan },` and pharos_console.c:151-155 — `run_scan(ops, "wifi.spectrum", 0, NULL, out);`
```

**Why it matters.** The lens names a channel and instructs the operator to camp on it; pressing the middle of the glass — the gesture the nine-step first-boot guide teaches for exactly this — does nothing, with no toast and no visible reason. Typing `scan camp 6` on the console prints a usage error. The one action the tool recommends is unreachable from both of its interfaces, which is the same dead end the on_select comment says the centre tap was added to close.

**Fix.** Add `static void spectrum_select(void)` modelled on watch_select: snapshot the busiest channel under s_lock, then toggle — if `pharos_radio_is_camped()` restart with `pharos_scan_plan_survey()` (dwell_ms 120, want_mgmt/want_data), otherwise restart with `pharos_scan_plan_camp(busiest)` (which already carries want_data across, per pharos_radio.c:113-126). Wire `.on_select = spectrum_select,` into k_spectrum. Widen the console entry to `{ "scan", "scan [camp <ch>|survey]", …, 0, 2, cmd_scan }` and have cmd_scan pass `argc - 1, argv + 1` like cmd_watch does. While camped, change the advice to say so and note that the other channels' rows are now frozen at their last visit.

### [medium] dishonesty — Lower the ceiling: a hopping receiver is not entitled to 100
`components/pharos_lens_spectrum/lens_spectrum.c:223`

**Wrong.** The lens hard-codes `o->ceiling = 100` — "the most this observation could have earned" set to certainty — for a number assembled from thirteen non-simultaneous 120 ms samples at roughly 1/13 duty, after an unbounded number of bus drops it never reads. Because the HUD only draws the ceiling tick when `ceiling < 100`, Spectrum is also the one lens on the device that shows no ceiling mark at all: the arc reads as a complete measurement.

**Evidence.**
```c
lens_spectrum.c:223 — `o->ceiling = 100;`
pharos_hud.c:1367 — `if (d->has_score && d->ceiling > 0 && d->ceiling < 100) {` (the ceiling tick)
README.md — "every verdict Pharos produces carries a confidence ceiling derived from how much of the channel it actually heard" … "Nothing ever reaches 100."
pharos_radio.h:102 — `uint16_t pharos_radio_dwell_permil(uint8_t channel);` — "This is the number the engines multiply their confidence ceiling by"
pharos_bus.h — `uint16_t pharos_bus_yield_permil(const pharos_bus_t *bus);` — "lowers its confidence ceiling as this falls"
```

**Why it matters.** The share is partly an absence claim — "ch 6 holds 78% of the air" rests on not having heard much on the other twelve, each sampled for 120 ms at a different moment. If a neighbour's ch 11 traffic happens to fall in the 1.4 s gap between visits, ch 11 reads near zero and ch 6's share is inflated, and the face asserts that with a full-confidence arc and no tick. Both instruments that would qualify it already exist and are used by Squall (`pq_context_t.dwell_permil = pharos_radio_dwell_permil(...)`); Spectrum reads neither.

**Fix.** Either (a) keep the score and cap it honestly: `const uint16_t dw = pharos_radio_dwell_permil(busiest); const uint16_t y = pharos_bus_yield_permil(&s_bus); o->ceiling = (uint8_t)(40u + (50u * dw * y) / (1000u * 1000u));` — ~44 hopping, ~88 camped, never 100 — and clamp `o->score` to it so the tick and the arc agree; or (b) accept that a share of airtime is not a confidence-bearing verdict, set `has_score = false`, and move the share into `o->detail`. Option (b) also fixes finding 1. Do not leave it at 100.

### [medium] ux — Either draw the waterfall or stop naming it
`components/pharos_lens_spectrum/lens_spectrum.c:61`

**Wrong.** `s_waterfall` is written once a second by spectrum_tick and is not read anywhere in components/, main/, tools/ or test/. The 1 Hz mutex acquisition in spectrum_tick exists solely to write it. Meanwhile three shipped user-facing strings promise it: the lens summary on the info card, the README feature table, and the console help line for `scan`. docs/ROADMAP.md lists "the Spectrum waterfall" under pending M2 work, so the gap is known — what is wrong is that the copy the operator reads presents it as a feature of the build they are holding.

**Evidence.**
```c
lens_spectrum.c:61 — `EXT_RAM_BSS_ATTR static uint8_t s_waterfall[SPEC_HISTORY][PHAROS_CHAN_MAX + 1]; /* busy 0..255 */`
lens_spectrum.c:170 — `s_waterfall[s_row][c] = busy;` (the only write; grep for `s_waterfall` across the tree returns only lines 61, 68 and 170)
lens_spectrum.c:284 — `.summary = "2.4 GHz airtime waterfall - the map you read the others against",`
pharos_console.c:403 — `"2.4 GHz airtime waterfall (Spectrum)"`
docs/ROADMAP.md:48 — "⏳ LVGL 9 widgets: the evidence gauge (with denied arcs), the Lamp Room dial, the lens cards, the Spectrum waterfall."
```

**Why it matters.** An operator picks Spectrum off the dial expecting the one view that shows how the band has behaved over the last minute, and gets a 16-second single-value activity ribbon and a 14-row text table. The history that would answer "was ch 6 always this busy or did it just start?" is computed, stored in PSRAM and discarded — 960 bytes and a mutex round-trip per second spent producing something nothing can read.

**Fix.** Draw it: the detail page already has the geometry, and a 64-row x 13-column heat strip (oldest at the top, `busy` mapped through ps_score_colour or a dim-to-warn ramp) is one pharos_round primitive. Add a lens hook — `bool (*heat)(uint8_t rows, uint8_t cols, const uint8_t **out);` or simply a `pharos_lens_spectrum_waterfall()` accessor the HUD calls — since the lens vtable currently has no way to hand the UI a 2-D buffer. Until that lands, change `.summary` to "2.4 GHz channel map" and the console help to match, and gate the buffer and the tick behind the same #ifdef so the build does not carry dead state. Note that the rows should be fed from `busy_permil` per finding 2, not from the frame-count ladder, or the waterfall will inherit the same unit error.

### [medium] bug — Delete the invented -95 dBm noise floor, or measure a real one
`components/pharos_lens_spectrum/lens_spectrum.c:36`

**Wrong.** `spec_cell_t` carries a `noise_floor` that is never measured, never displayed, and initialised to a fabricated constant. `spectrum_mount` sets every `s_now[c].noise_floor = -95`, and the first DWELL overwrites it with 0 because `s_now[dc] = s_cur[dc]` copies a struct that was memset — so the field silently transitions from an invented -95 dBm to an equally meaningless 0 dBm. It is a fake instrument reading sitting in a struct waiting for the next person to render it.

**Evidence.**
```c
lens_spectrum.c:36 — `int8_t noise_floor;` in spec_cell_t (no read of it appears anywhere in the file)
lens_spectrum.c:70-73 — `for (unsigned c = 0; c <= PHAROS_CHAN_MAX; c++) { s_now[c].peak_rssi = -128; s_now[c].noise_floor = -95; }`
lens_spectrum.c:107 — `s_now[dc] = s_cur[dc];` with s_cur zeroed at line 66/108
pharos_radio.c:158-160 — `/* No true noise-floor register is exposed, so leave it at 0 - which the engines read as "unknown" and disclose - rather than inventing one. */ ev.u.dwell.noise_floor = 0;`
ESP-IDF components/esp_wifi/include/local/esp_wifi_types_native.h:69 — for CONFIG_IDF_TARGET_ESP32S3: `signed noise_floor: 8;  /**< noise floor of Radio Frequency Module(RF). unit: dBm*/` inside wifi_pkt_rx_ctrl_t
```

**Why it matters.** Today it is dead weight, which is why this is low severity — but it is dead weight shaped exactly like the thing the house rules forbid. The radio's comment justifying `noise_floor = 0` is also factually wrong for this part: the ESP32-S3's promiscuous rx_ctrl does expose a per-frame noise floor in dBm (the non-HE struct branch, which S3 uses — CONFIG_SOC_WIFI_HE_SUPPORT is a C5/C6 feature). So Pharos is disclosing "unknown" for a number the chip hands it on every frame, while a lens holds a hard-coded -95 for the same quantity. Squall already guards `if (d->noise_floor != 0)` and would light up the moment it were populated.

**Fix.** Two independent changes. (1) In this lens: delete the field and the -95 initialiser, since nothing reads them. (2) Separately, in pharos_radio.c's promisc_cb, track `if (pkt->rx_ctrl.noise_floor < s.dwell_noise_min || s.dwell_noise_min == 0) s.dwell_noise_min = pkt->rx_ctrl.noise_floor;` and emit it as `ev.u.dwell.noise_floor`, replacing the comment with an accurate one: this is a floor sampled at frame arrivals, not an idle-channel measurement, so it stays 0 for a visit with no frames and must be labelled "floor when frames arrived" wherever it is shown. Spectrum can then display a real SNR per channel (`peak_rssi - noise_floor`), which is the number that actually distinguishes a loud close AP from a busy distant one — a distinction the current `%d dBm` peak cannot make.


## squall

**What it does.** Squall consumes one PHAROS_EV_DWELL summary per channel visit (channel, dwell_ms, decoded frames, retries, retry count, busy airtime per mille, FCS failures) and, per channel, classifies the accumulated visits into QUIET / HEALTHY / CONGESTED / DEGRADED / DENIAL using the "loud AND barren" discriminator (busy airtime high while decoded frames-per-second is low), then scores the band 0..100 from four evidence families (ENERGY, RETRIES, SPREAD, BROKEN) under a set of honesty caps and the harshest confidence ceiling in Pharos, so that a busy building reads CONGESTED rather than jammed.

**Prior art.** The canonical algorithm is Xu, Trappe, Zhang & Wood, "The Feasibility of Launching and Detecting Jamming Attacks in Wireless Networks" (MobiHoc 2005, http://eceweb1.rutgers.edu/~yyzhang/research/papers/mobihoc05-xu.pdf). Its central result is exactly Squall's thesis and its central mechanism is what Squall is missing: no single measurement identifies a jammer, so they pair packet delivery ratio with a *signal-strength consistency check* — "if signal strength is higher, then PDR must be high, while the converse is not true". Low PDR + high RSS = jammed; low PDR + low RSS = out of range or dead. Squall has both halves available in pq_dwell_t (peak_rssi, frames) and uses neither for that test; it substitutes busy_permil, which in this firmware is not an energy measurement at all.

Jardosh, Ramachandran, Almeroth & Belding, "Understanding Congestion in IEEE 802.11b Wireless Networks" (IMC 2005, https://www.usenix.org/legacy/event/imc05/tech/full_papers/jardosh/jardosh.pdf) motivates channel busy-time as the direct measure of channel utilisation and calibrates it: >84% utilisation = "highly congested". Squall's loud=40% / very_loud=65% sit sensibly below that line — but Jardosh's busy-time comes from the PHY's CCA, not from counting decoded frames. The same distinction is the whole point of Linux nl80211/mac80211 `survey dump` `channel_time_busy`, which ath9k/ath10k fill from the hardware CCA register precisely because it counts energy the receiver could not decode.

The commercial metric sets (e.g. US 9,167,457 / 8,953,573, "Measuring and displaying wireless network quality") use a weighted sum of "normalized noise floor offset, channel busy time from interference or duty cycle of interferers, frame error rate, PHY error rate and CRC error rate", and add deferral rate (time CCA reports busy while the AP has a packet queued). Squall's four-family structure matches this and is arguably better disciplined; the difference is that three of those five inputs are CCA/PHY-register-derived and Squall has none of them.

Platform-specific prior art that *vindicates* two of Squall's choices: espressif/esp-idf#1751 confirms `wifi_pkt_rx_ctrl_t.noise_floor` is stuck at 0 in promiscuous mode, so pharos_radio.c's refusal to invent a floor ("No true noise-floor register is exposed, so leave it at 0") is correct and PQ_NOTE_NOFLOOR is honest. Conversely espressif/esp-idf#6473 (IDFGH-4661) and #10777 (IDFGH-9408) report that WIFI_PROMIS_FILTER_MASK_FCSFAIL is not honoured on ESP32 and ESP32-C2 — an unverified risk to PQ_FAM_BROKEN on the S3, though Squall already refuses to read fcs_fail==0 as health, which is the right defence.

The ESP32 field is weak and Squall is well ahead of it: ronaldstoner/ESP32-WifiJammerDectector is an AP-count-disappearance heuristic — exactly the absence-based detector Squall's own header rejects — and cifertech/ESP32-DIV's "Jamming Detector" does adaptive noise-floor thresholding, but only because it has a *separate* CC1101/nRF24 energy receiver. That is the hardware answer to the problem Squall is trying to solve in software on a Wi-Fi-only part, and it is why the busy_permil bug below matters so much.

### [high] bug — busy_permil is a frame counter, not airtime — DENIAL is arithmetically unreachable on hardware
`components/pharos_radio/pharos_radio.c:451`

**Wrong.** The only producer of busy_permil in the tree accumulates a flat nominal slot per *decoded* frame, after the FCS-fail early return: promisc_cb does `if (pkt->rx_ctrl.rx_state != 0) { s_fcs_fail++; return; }` and only later `s.dwell_busy_us += 400;`. emit_dwell then publishes `busy = dwell_busy_us*1000/us`. So busy_permil = 400*F/dwell_ms, while Squall's fps = 1000*F/dwell_ms — i.e. busy_permil is exactly 0.4*fps, the same number in different units. Substituting into classify(): `loud` (busy>=400) means fps>=1000, `very_loud` (busy>=650) means fps>=1625, and `barren` (fps<10) means busy<4. `loud && barren` and `very_loud && barren` are therefore mutually exclusive by construction. PQ_STATE_DENIAL can never be assigned, n_denial is always 0, PQ_FAM_ENERGY and PQ_FAM_SPREAD are never set, PQ_NOTE_NARROW is never set, and the lens's alert level 3 never fires. CONGESTED needs 1000+ decoded frames/second through the promiscuous callback and is unreachable in practice too. On a real board Squall can only ever say QUIET, HEALTHY or DEGRADED. The engine's own comment asserts the opposite of what the value is.

**Evidence.**
```c
pharos_radio.c:451  `s.dwell_busy_us += 400;`  (reached only after `if (pkt->rx_ctrl.rx_state != 0) { s_fcs_fail++; return; }` at :249-251 and a successful `pharos_dot11_parse_header`)
pharos_squall.c:76-78  `/* "Loud" is judged from the airtime the radio reported busy, which is the\n     * measure that does not depend on anything decoding. */\n    const bool loud = busy >= 400;`
pharos_squall.c:87  `const bool barren = fps < 10;`
```

**Why it matters.** The lens exists to answer "busy, broken, or jammed" and on hardware it structurally cannot say "jammed". An operator standing inside an actual jam sees QUIET or HEALTHY with alert 0. Every host test passes because test_squall.c feeds hand-written pairs such as `dw(ch, 400, 2, 1, 820)` — 5 fps at 820 permil busy — which this radio can never produce: 5 fps yields busy_permil 2. The test fixture is physically impossible given the firmware's own busy estimator, which is why the dead branch has gone unnoticed.

**Fix.** Make busy_permil a real occupancy estimate, in the radio, receive-only and integer-only. (1) Move airtime accumulation ahead of the FCS-fail early return: rx_ctrl is valid for rx_state!=0 frames, so add their airtime too — a frame that shattered still occupied the medium, and on a jammed channel it is the only airtime there is. (2) Replace the flat 400 us with duration from the metadata already in rx_ctrl: `dur_us = preamble_us + (sig_len*8*1000)/rate_kbps`, where rate_kbps comes from a static table indexed by sig_mode/rate/mcs (non-HT {1000,2000,5500,11000,6000,9000,12000,18000,24000,36000,48000,54000}; HT20 LGI MCS0-7 {6500,13000,19500,26000,39000,52000,58500,65000}; *2 when cwb==1; *10/9 when sgi==1) and preamble_us is 192 for long DSSS, 20 for OFDM, 36 for HT-mixed. All uint32 arithmetic, no float. (3) Add a host test that asserts the invariant this bug violates: for any dwell the engine may see, `loud && barren` must be *reachable* — e.g. a regression test feeding a dwell built by the same duration function the radio uses, so the fixture can never again describe air the radio cannot report.

### [high] bug — The channel table never decays, so a jam that starts mid-session can never be seen
`components/pharos_engine/include/pharos_squall.h:63`

**Wrong.** PQ_HISTORY is declared as "dwells remembered per channel" but is never referenced anywhere in the tree (verified by grep across components/, main/ and test/). There is no ring, no window and no ageing: pq_observe only ever does `c->frames_total += d->frames; ... c->dwell_ms_total += d->dwell_ms; c->busy_sum += d->busy_permil;`, and pq_reset is called exactly once, from squall_mount. Every figure the engine judges is a lifetime-of-session mean. classify() computes `busy = c->busy_sum / c->visits` and `fps = (c->frames_total * 1000u) / secs` over the whole session.

**Evidence.**
```c
pharos_squall.h:63  `#define PQ_HISTORY 8 /* dwells remembered per channel */`  (defined, never used)
pharos_squall.c:69-72  `const uint32_t busy = c->busy_sum / c->visits;`  /  `const uint32_t fps = (c->frames_total * 1000u) / secs;`
lens_squall.c:41  `pq_reset(&s_table);`  — inside squall_mount, the only call site
```

**Why it matters.** Detection latency grows without bound with uptime. After T seconds of healthy air at 100 fps, a *total* jam (0 fps) only drags the mean below the barren threshold of 10 fps after a further 9T seconds — ten minutes of normal observation buys the jammer ninety minutes of invisibility. The same arithmetic runs the other way: once a channel has been graded DENIAL the verdict is sticky long after the source is switched off, so the operator keeps walking toward a transmitter that has stopped, which directly contradicts the DENIAL advice "this is a condition that stops the moment the source is switched off". It also removes the only bound on `c->frames_total * 1000u`, which overflows uint32 past ~4.29M frames (hours on a camped channel). And because camping does not reset the table, a `squall camp 6` posture keeps grading twelve other channels from minutes-old survey data as though it were current.

**Fix.** Implement the window the header already promises. Add `uint16_t f_frames[PQ_HISTORY], f_retries[PQ_HISTORY], f_busy[PQ_HISTORY], f_dwell_ms[PQ_HISTORY], f_fcs[PQ_HISTORY]; uint8_t head, n;` to pq_channel_t, have pq_observe write at head and advance modulo PQ_HISTORY, and have classify() sum the ring instead of the running totals (visits stays a lifetime count for PQ_MIN_SAMPLES, or becomes min(n, PQ_HISTORY)). At 500 ms dwells and a 13-channel sweep, 8 slots is roughly the last minute per channel, which is the right horizon for "a condition, not a sample". Add a host test that feeds 40 healthy dwells to one channel, then 8 jammed ones, and asserts the channel is graded DENIAL — today it reads HEALTHY — plus the mirror test that 8 healthy dwells after a jam clear it.

### [high] dishonesty — PQ_FAM_SPREAD counts overlapping channel indices, so one 20 MHz device spans the band
`components/pharos_engine/pharos_squall.c:227`

**Wrong.** SPREAD is set when three or more *channel numbers* are in the denial state, on the reasoning that one bad channel is one noisy device. But 2.4 GHz channel centres are 5 MHz apart while the channels themselves are 20 MHz wide, and pharos_scan_plan_survey() visits every index from PHAROS_CHAN_MIN to the region maximum consecutively (`for (uint8_t c = PHAROS_CHAN_MIN; c <= hi ...) p.channels[p.n_channels++] = c;`). A single 20 MHz emitter on channel 6 puts substantial in-band energy into the receiver when it is tuned to channels 4, 5, 6, 7 and 8. n_denial therefore reaches 5 from one device, SPREAD fires, and PQ_NOTE_NARROW (`else if (out->n_denial == 1)`) — and with it the 74-point cap — becomes effectively unreachable for any real interferer. SPREAD is also not an independent family: it is derived from the same n_denial counter as PQ_FAM_ENERGY, so `families == PQ_FAM_ENERGY|PQ_FAM_SPREAD` is one measurement counted twice.

**Evidence.**
```c
pharos_squall.c:227-234  `if (out->n_denial >= 3) {`  /  `/* A jammer usually covers a band, not one channel. A single bad\n         * channel is far more likely to be one noisy device. */`  /  `out->families |= PQ_FAM_SPREAD;`  /  `} else if (out->n_denial == 1) { out->notes |= PQ_NOTE_NARROW; }`
pharos_squall.c:246-249  `const bool energy_only = (out->families == PQ_FAM_ENERGY);`  /  `if (energy_only && score > 62) { score = 62; }`
pharos_squall.h:37-39  `*   - DENIAL needs BOTH families. Energy alone is a microwave oven, a video\n *     sender, a neighbour's outdoor bridge, or a badly-sited camera. None of\n *     those are attacks`
```

**Why it matters.** SPREAD is one of only two ways to escape the 62-point energy_only cap, and on this silicon it may be the only one (the other, PQ_FAM_BROKEN, depends on WIFI_PROMIS_FILTER_MASK_FCSFAIL, which espressif/esp-idf#6473 and #10777 report as not honoured on some parts). Once finding 1 is fixed and DENIAL becomes reachable, the very devices the header names as the innocent explanation — a video sender, a neighbour's outdoor bridge, a microwave oven, all of them wideband — satisfy the family whose job is to rule them out. Severity 70+ plus the +10 SPREAD bonus clears the score>=70 threshold, so worst stays DENIAL, alert goes to 3, and the glass reads "this is what a jam looks like" for a baby monitor. That is the false positive the whole lens was written to avoid, arriving through the mechanism meant to prevent it.

**Fix.** Judge spread in megahertz, not in array indices. In pq_evaluate, while walking the channels, record the lowest and highest denial channel and count denial channels that are at least 5 apart from the previously counted one (a greedy non-overlapping set). Then: set PQ_NOTE_NARROW when `hi - lo <= 4` regardless of how many indices are involved — that is one emitter's 20 MHz footprint, not a band — and set PQ_FAM_SPREAD only when the non-overlapping count is >= 3 and `hi - lo >= 8`, which on a 13-channel plan means energy at both ends of the band. Add the negative as a host test: feed DENIAL-shaped dwells to channels 4,5,6,7,8 only and assert PQ_NOTE_NARROW is set, PQ_FAM_SPREAD is not, and the score stays at or below 62.

### [high] bug — broken_permil is summed over ungraded channels, so one dwell can light a family and lift a cap
`components/pharos_engine/pharos_squall.c:195`

**Wrong.** The corruption block iterates every channel with `t->ch[i].in_use` and never checks whether that channel cleared PQ_MIN_SAMPLES. classify() is only meaningful for channels with visits >= 3, and the rest of pq_evaluate is careful about this — `frames` and `retries`, from which retry_permil is built, are accumulated only after `if (c->state == PQ_STATE_UNKNOWN) { continue; }`. The broken block skips that guard, and it reads the caller's table `t->ch[i]` rather than the classified `local[i]`, so the state is not even available at that point. The header's own contract for the field says otherwise.

**Evidence.**
```c
pharos_squall.c:195-201  `for (unsigned i = 1; i <= PQ_MAX_CHANNELS; i++) {`  /  `if (!t->ch[i].in_use) { continue; }`  /  `bad += t->ch[i].fcs_fail_total;`  /  `heard += t->ch[i].fcs_fail_total + t->ch[i].frames_total;`
pharos_squall.h:157  `uint32_t fcs_fail_total; /* across everything graded              */`
pharos_squall.h:66-67  `/* A channel must be visited at least this many times before it may be graded.\n * One bad visit is a sample, not a finding. */`
```

**Why it matters.** Reachable whenever some channels are graded and others are not — for example releasing a camp back to survey, when the camped channel is graded and the other twelve have one visit each for the next couple of sweeps. In that window a single 500 ms dwell on one un-graded channel that lands 40+ FCS failures at over 25% of what was heard sets PQ_FAM_BROKEN, adds up to +18 to the score, and — because `energy_only` is an exact equality test on the family bitmask — silently removes the 62-point energy-only cap, letting the verdict reach DENIAL. A finding built from one un-graded visit is precisely what PQ_MIN_SAMPLES exists to forbid. It also distorts broken_permil permanently, since un-graded channels' frames inflate the `heard` denominator.

**Fix.** Move the block inside the main classification loop, or gate it on the classified copy: `if (!local[i].in_use || local[i].state == PQ_STATE_UNKNOWN) continue;` and accumulate from `local[i]` rather than `t->ch[i]`. Host test: grade one channel, then give a second channel a single dwell with `frames=10, fcs_fail=200`, and assert PQ_FAM_BROKEN is clear and broken_permil reflects only the graded channel.

### [high] blind-spot — peak_rssi is accepted by the engine and thrown away — the Xu consistency check is one field from working
`components/pharos_engine/include/pharos_squall.h:115`

**Wrong.** pq_dwell_t declares `int8_t peak_rssi;`, the lens dutifully fills it (`.peak_rssi = d->peak_rssi,`), the radio computes it per visit (`if (ev.u.dot11.rssi > s.dwell_peak_rssi || s.dwell_peak_rssi == 0) s.dwell_peak_rssi = ev.u.dot11.rssi;`) — and pq_observe has no line that touches it and pq_channel_t has no field to hold it. The measurement is carried the whole way to the engine's front door and dropped. Separately, floor_sum/floor_n are accumulated and only ever tested for non-zero (`any_floor`); the mean noise floor is never computed. (The floor itself is genuinely unavailable — espressif/esp-idf#1751 confirms rx_ctrl.noise_floor is stuck at 0 in promiscuous mode, and pharos_radio.c is right to leave it at 0 rather than invent one. peak_rssi is not unavailable; it is measured and discarded.)

**Evidence.**
```c
pharos_squall.h:115  `int8_t peak_rssi;`
lens_squall.c:94  `.peak_rssi = d->peak_rssi,`
pharos_squall.c:17-38 — pq_observe accumulates frames, retries, fcs_fail, dwell_ms, busy_permil and noise_floor; there is no statement mentioning peak_rssi, and pq_channel_t (pharos_squall.h:123-136) has no field for it.
```

**Why it matters.** Peak RSSI is the missing half of the canonical jamming test. Xu et al. (MobiHoc 2005) establish that PDR alone cannot identify a jammer and pair it with a signal-strength consistency check: high signal strength implies delivery must be high, and the converse does not hold. Squall already has the delivery half (fps, retry_permil, broken_permil) and the strength half arrives on every dwell. Without it, Squall cannot separate its two most consequential cases: a channel with almost no frames and a strong peak (something loud is close and nothing is getting through) versus a channel with almost no frames and a peak near the sensitivity limit (there is nothing here, or the receiver is deaf). Today both read QUIET, severity 0. It is also the one honest fallback if the busy-time fix in finding 1 cannot be made to work on this part.

**Fix.** Add `int8_t peak_best;` to pq_channel_t and set it in pq_observe (`if (d->peak_rssi != 0 && (c->peak_best == 0 || d->peak_rssi > c->peak_best)) c->peak_best = d->peak_rssi;`). Then in classify(), use it as a consistency check rather than a threshold in its own right: when `barren` and peak_best is strong (say >= -60 dBm), the silence is not emptiness — that is the low-PDR/high-RSS quadrant, and it should upgrade QUIET to DEGRADED and be eligible as a second, genuinely independent family (a new PQ_FAM_STRONG). When `barren` and peak_best is weak or absent, keep QUIET and keep the existing "or a receiver that is not hearing" disclosure. Because peak_rssi is a field an attacker cannot usefully forge downward, the rule must only ever raise suspicion: a strong peak may promote a state, a weak one may never demote one.

### [high] ux — raw_score is computed, has a display slot, and never reaches the glass or the report
`components/pharos_lens_squall/lens_squall.c:183`

**Wrong.** pq_evaluate sets `out->raw_score = (uint8_t)score;` before the ceiling is applied, and struct pharos_lens_display carries a documented field for it ("raw_score; /* what the evidence earned BEFORE the caps */"). k_squall_display sets score, ceiling and has_score but never raw_score, and pharos_lens_squall_report does not emit it either. Watch, Rival and Ward all do both.

**Evidence.**
```c
pharos_squall.c:266  `out->raw_score = (uint8_t)score;`
lens_squall.c:183  `o->score = v.score; o->ceiling = v.ceiling; o->has_score = true;`  — raw_score absent
lens_squall.c:148-149  `prt_u32(&w, "score", s_verdict.score);` / `prt_u32(&w, "ceiling", s_verdict.ceiling);` — raw_score absent
lens_watch.c:392  `o->raw_score = v.raw_score;`  and lens_watch.c:546-547  `snprintf(out->right, ..., "%u/%u", v.raw_score, v.ceiling); out->tone = (v.raw_score > v.score) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;`
```

**Why it matters.** Squall applies more caps than any other lens — energy_only 62, families-none 45, NARROW 74, THIN 66, then the ceiling — and it is the lens where the gap between evidence and claim carries the most meaning. An operator seeing 62 cannot tell whether the evidence earned 62 or earned 95 and was held back because only one family fired. That is the difference between "nothing much here" and "something is very wrong and I cannot prove it from a hopping receiver", and it is exactly what the raw_score field was added to show. Because the caps are silent, the operator also gets no signal that camping (which lifts the ceiling from ~36 to 90) would change the answer.

**Fix.** Add `o->raw_score = v.raw_score;` to k_squall_display, `prt_u32(&w, "raw_score", s_verdict.raw_score);` to the report next to score/ceiling, and a detail row in k_squall_row on the Watch pattern — left "evidence/ceiling", right `"%u/%u"` of raw_score and ceiling, toned WARN when raw_score > score so a capped verdict is visible as a capped verdict.

### [high] ux — The lens whose advice says "camp on the channel" has no camp control
`components/pharos_lens_squall/lens_squall.c:270`

**Wrong.** k_squall registers no on_select, so the centre tap does nothing while Squall is live: pharos_ui.c dispatches `if (live && live->on_select) { live->on_select(); }` and falls through to nothing otherwise. Squall's own advice strings instruct the operator to camp, in two states, and Squall's ceiling depends on camping more than any other lens's — pq_ceiling subtracts 12 and the dwell share collapses to roughly 77 permil when hopping, giving ~36, against up to 90 when camped, and PQ_NOTE_THIN adds a hard 66-point cap on top while hopping. Watch, Ward, Footprint and System all implement on_select; the UI comment says plainly why it exists.

**Evidence.**
```c
lens_squall.c:270-290 — the k_squall initialiser lists on_mount, on_start, on_stop, on_tick, on_event, ingest, stage_report, display, row, row_head_left, row_head_right; there is no `.on_select`.
pharos_squall.c:330-334  `"... Camp on the channel to raise confidence, and use Locate to walk toward whatever is loudest."`  and :317-318  `"Not enough visits to this channel to say anything. Camp on it if you want an answer about it specifically."`
pharos_ui.c:1137-1145  `/* ... the way to raise the confidence ceiling is to stop hopping, and no control on the glass did it. A running lens may now claim the tap. */` / `if (live && live->on_select) { live->on_select(); }`
```

**Why it matters.** The device tells the operator to do the one thing it gives them no way to do. The only camp path is `squall camp 6` over USB console, which, as pharos_radio.c itself notes, "is not a thing you do while holding the device up in a corridor". The cost is concrete: a hopping Squall is capped at 66 and ceilinged near 36, so a genuine band-wide denial cannot be reported above DEGRADED, and the action that would lift it is unreachable from the glass.

**Fix.** Add a squall_select() on the watch_select pattern: on first tap, camp on v.worst_channel (falling back to pharos_radio_channel() when worst_channel is 0) via pharos_scan_plan_camp — which already carries want_data and want_fcsfail across, per the comment at pharos_radio.c:108-126 — and on second tap release back to pharos_scan_plan_survey(). Call pq_reset(&s_table) on each posture change, since per finding 2 the stale survey data would otherwise keep grading twelve channels nobody is listening to. Show the posture in o->detail ("HOPPING" / "CAMPED ch6") the way Watch does, so the tap has visible feedback.

### [medium] blind-spot — A jammer with no 802.11 preamble produces the lowest possible alarm
`components/pharos_engine/pharos_squall.c:103`

**Wrong.** Every input the engine receives originates in the promiscuous RX callback: frames, retries and busy airtime come from decoded frames, fcs_fail from frames whose preamble was detected but whose checksum failed, and noise_floor is hardcoded to 0 by the radio. A continuous-wave, swept or noise jammer emits nothing the receiver will interpret as a preamble, so it produces no callbacks at all — busy 0, frames 0, fcs_fail 0. classify() then takes `barren && !loud` and returns QUIET with severity 0, the display maps that to alert 0, and the headline is "Very little here - quiet air, or a radio hearing nothing". The most complete denial there is produces the calmest possible reading. Note this is worse in combination with finding 2: because the table never decays, a jam starting mid-session does not even reach QUIET — the accumulated healthy frames keep fps high and the channel stays HEALTHY.

**Evidence.**
```c
pharos_squall.c:103-105  `} else if (barren && !loud) {`  /  `c->state = PQ_STATE_QUIET;`  /  `c->severity = 0;`
pharos_radio.c:158-160  `/* No true noise-floor register is exposed, so leave it at 0 - which the\n     * engines read as "unknown" and disclose - rather than inventing one. */`  /  `ev.u.dwell.noise_floor = 0;`
pharos_squall.h:21-24  `*     energy is high AND decodable frames are LOW   -> DENIAL. The band is\n *         full of power that will not resolve into frames.`
```

**Why it matters.** The header's central claim — that the engine can see "power that will not resolve into frames" — holds only for power that still carries a detectable preamble. For everything else there is no energy input at all. This is a real gap rather than a fixable bug on this hardware (it is why cifertech/ESP32-DIV bolts a CC1101 onto the board to do the same job), but the current wording does not disclose it, and QUIET's advice offers "an empty channel - or a receiver that is not hearing" without naming the third possibility. Squall never says "safe", which is right, but a 0 next to "quiet air" reads as reassurance during the worst case it is meant to catch.

**Fix.** No new hardware and no transmission: make the limit explicit and make the state carry information. (a) Extend pq_state_advice(PQ_STATE_QUIET) to name the third cause — an emitter with no 802.11 preamble is indistinguishable from empty air to this receiver, and the way to tell them apart is to check whether other channels also went silent at the same moment. (b) Add a note bit, PQ_NOTE_DEAF, set when a channel that previously carried traffic drops to zero frames within the window from finding 2 — a transition to silence is positive evidence in a way that steady silence is not, and it is available once the ring exists. (c) Add one line to the lens header comment stating that the loud-and-barren discriminator only sees interference that produces preamble detections, so the claim in pharos_squall.h:21-24 is scoped to what the radio can actually hear.

### [medium] ux — The detail line pins a band-wide retry percentage to one channel number
`components/pharos_lens_squall/lens_squall.c:180`

**Wrong.** The on-glass detail line prints the worst channel and the retry percentage side by side, but retry_permil is the aggregate across every graded channel (`out->retry_permil = (uint16_t)(frames ? (retries * 1000u) / frames : 0);` over the summed totals), while worst_channel is one specific channel. The same mismatch runs through the family logic: PQ_STATE_DEGRADED is assigned from the *per-channel* `retry_permil >= 400`, but PQ_FAM_RETRIES is set from the *aggregate*, so the channel that produced the headline can be retrying at 90% while no retry family lights and the `families == 0` rule caps the score at 45.

**Evidence.**
```c
lens_squall.c:180-181  `snprintf(o->detail, sizeof(o->detail), "ch%u  %u graded  retry %u%%", v.worst_channel, v.n_graded, v.retry_permil / 10u);`
pharos_squall.c:172  `out->retry_permil = (uint16_t)(frames ? (retries * 1000u) / frames : 0);`
pharos_squall.c:99-102  `} else if (retry_permil >= 400 && !barren) {`  /  `c->state = PQ_STATE_DEGRADED;`  — a different, per-channel retry_permil
```

**Why it matters.** One busy, clean channel dominates the aggregate denominator and washes out a badly retrying one, so the glass can read "DEGRADED  ch6  8 graded  retry 12%" while channel 6 is at 90%. The operator reads the 12% as a fact about channel 6, which it is not, and the evidence that actually produced the word DEGRADED is nowhere on the screen. The score suffers the same way: the per-channel evidence exists, lights no family, and is then capped at 45 by the families==0 rule.

**Fix.** Add `uint16_t worst_retry_permil;` to pq_verdict_t, set it from the channel that wins the worst_sev comparison, and print that in the detail line and in row 5 next to the channel number (keep the aggregate as a separate row labelled "band retry" if it is still wanted). Then set PQ_FAM_RETRIES on `out->retry_permil >= 350 || out->worst_retry_permil >= 500` so the family reflects the evidence that produced the headline, and export worst_retry_permil in the report.

### [medium] ux — The BROKEN evidence never reaches the report the DENIAL advice tells you to capture
`components/pharos_lens_squall/lens_squall.c:146`

**Wrong.** pharos_lens_squall_report writes state, worst_channel, score, ceiling, families, notes, channels_graded, channels_denial, channels_congested, retry_permil and advice. It does not write broken_permil or fcs_fail_total — the two fields the header calls the direct physical signature and "the one measurement that shows it happening". The console summary in main/console_glue.c omits them too. `notes` is exported as a raw integer bitfield and is never decoded anywhere, on the glass or in the report, so PQ_NOTE_THIN, PQ_NOTE_NARROW, PQ_NOTE_FEW and PQ_NOTE_NOFLOOR are unreadable by the person holding the device.

**Evidence.**
```c
lens_squall.c:146-156 — the full field list; no broken_permil, no fcs_fail_total, no raw_score.
lens_squall.c:151  `prt_u32(&w, "notes", s_verdict.notes);`  — an undecoded bitmask
pharos_squall.c:338-340  `"... Capture a report now - this is a condition that stops the moment the source is switched off."`
pharos_squall.h:94-95  `* That is the distinction this engine exists to make, and until now it was\n * being made without the one measurement that shows it happening. */`
```

**Why it matters.** The DENIAL advice is explicit that the report is the only durable artefact of a condition that disappears when someone flips a switch, and the report omits the evidence that distinguishes the condition from a busy Tuesday. A captured Squall report cannot be used afterwards to defend the claim that the channel was jammed rather than crowded: the reader gets a score, a word and a retry percentage that a congested office also produces. The notes bitfield has the same problem in the other direction — the disclosures that qualify the score ("this was measured while hopping", "this is one channel") are present in the JSON as the integer 5 and absent from the screen entirely.

**Fix.** Add `prt_u32(&w, "broken_permil", s_verdict.broken_permil);` and `prt_u32(&w, "fcs_fail_total", s_verdict.fcs_fail_total);` alongside retry_permil, and add the same two figures to the console line in main/console_glue.c:169. For the notes, either emit them as a string array of decoded names ("thin", "narrow", "few", "nofloor") rather than an integer, or add a helper `const char *pq_note_short(uint8_t notes)` in the engine returning the single most limiting disclosure and render it in the display's `why[48]` field, which Squall currently leaves empty.

### [medium] bug — k_squall_display and k_squall_stage read the verdict without the mutex
`components/pharos_lens_squall/lens_squall.c:177`

**Wrong.** Both take a copy of the shared s_verdict on the UI task with no lock, while squall_tick concurrently rewrites it through pq_evaluate — which begins with `memset(out, 0, sizeof(*out))`. The lens already has the correct accessor: pharos_lens_squall_snapshot takes s_lock with a 5 ms timeout, and k_squall_row uses it. Watch's display uses its own snapshot for the same reason.

**Evidence.**
```c
lens_squall.c:177  `pq_verdict_t v = s_verdict;`  — in k_squall_display, no xSemaphoreTake
lens_squall.c:167-169  `*score = s_verdict.score;` / `*ceiling = s_verdict.ceiling;`  — in k_squall_stage, no lock
pharos_squall.c:117  `memset(out, 0, sizeof(*out));`
lens_squall.c:129  `if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) { return false; }`  — the accessor that exists and is used by k_squall_row
```

**Why it matters.** A repaint landing inside the memset window shows a torn verdict — typically UNKNOWN with score 0 and no family chips — for one frame, in the middle of a live reading. It is cosmetic on the glass but not in k_squall_stage, whose score and ceiling are consumed by the Aegis chain as a DISRUPT-stage finding; a torn read there puts a wrong number into a cross-lens correlation rather than onto a screen that repaints a moment later.

**Fix.** Use the existing accessor in both: `pq_verdict_t v; if (!pharos_lens_squall_snapshot(&v)) return false;` in k_squall_display, and the same in k_squall_stage, returning false when the lock cannot be taken so the chain skips this tick rather than consuming a half-written verdict.


## survey

**What it does.** The Survey is the one lens with no radio of its own: it accumulates facts other lenses push into a session-long table deduplicated by MAC address (networks with their census grade + fault flags, probing devices with their leaked-name counts, pentest-hardware kinds), then turns those counts into a headline, a ranked set of plain-English sentences that must each fit 25 characters, and a detail page — deliberately with no 0..100 score, because "there is no honest 0..100 for what is this place like".

**Prior art.** Wireless site-survey and wardriving practice converges on three things this lens is measured against.

(1) Passive discovery is a function of per-channel DWELL, not of elapsed wall time. Vanhoef/Goovaerts/Acar, "Improving Privacy through Fast Passive Wi-Fi Scanning" (NordSec 2019, papers.mathyvanhoef.com/nordsec2019.pdf) measures AP-discovery rate against dwell time directly: dwell "heavily influences the total duration of a passive scan", discovery improves up to roughly the 100 ms beacon interval and "increasing the dwell time past 100 ms did not yield more benefits", measured against ~30 discoverable APs at their office. Cisco's RRM white paper (cisco.com/c/en/us/td/docs/wireless/controller/technotes/8-3/b_RRM_White_Paper/) sizes off-channel dwell against the beacon interval for the same reason, and notes beacons not seen during a scan are simply absent from that sample point. Our project already knows this — `pharos_radio_dwell_permil()` is documented as "the number the engines multiply their confidence ceiling by: camped => ~1000, hopping => roughly 1000 / n_channels" and nine lenses use it — but the Survey, the one screen that aggregates a whole session, uses it nowhere and reports wall-clock minutes instead.

(2) A defensible RF site report records coverage and its own limits, not just an inventory. PCI DSS 11.2.1 rogue-AP scanning is expected to cover 2.4, 5 and 6 GHz precisely because a single-band sensor cannot support a negative finding (securitymetrics.com/blog/wireless-access-point-protection-finding-rogue-wi-fi-networks, accessagility.com/blog/pci-dss-4.0-and-wireless-wifi). Wireless-pentest methodology writeups list the deliverable as AP inventory + channel-usage summary + the survey path/equipment used (thecyphere.com/blog/what-is-wireless-penetration-testing/). Our Spectrum, Census and Rival lenses each carry their deafness permanently on screen; Survey is the outlier that does not.

(3) Working implementations are explicit about what their data model does and does not keep. Kismet's wardrive mode (kismetwireless.net/docs/readme/configuring/wardrive/) documents exactly what it throws away — `dot11_ap_only_survey=true`, `dot11_fingerprint_devices=false`, `dot11_keep_ietags=false`, `kis_log_channel_history=false` — so a reader knows the limits of the log; and Kismet/WiGLE key on BSSID, with per-record first/last-seen timestamps, so "how long was this here" is always answerable. Where our approach differs and is better: our Probe engine already does the thing Kismet's wardrive mode switches off, relinking randomised MACs by IE fingerprint + sequence continuation (`dev->identities++`) in the spirit of Vanhoef's and Martin et al.'s randomisation work — but the Survey then throws that link away at the hook boundary and re-keys on the rewritten address, which is the single most consequential gap found here.

### [high] bug — Send present=false for tools that have gone, or "pentest hardware in the room" latches for the whole session
`components/pharos_lens_rival/lens_rival.c:398`

**Wrong.** `pharos_survey_tool` has exactly one producer in the firmware, and it only ever passes `true`. The loop it sits in enumerates `prv_device_at_now(...)`, which skips anything the Rival engine has already dropped ("if (stale_on_listen(d, s->listen_us)) { continue; }") — so a Flipper that leaves the room simply stops being enumerated, and no call with present=false is ever made. `psv_note_tool` only flips `present` on an explicit call, so the flag set on first sight stays set until reboot. The engine supports the transition and test_survey.c:258 exercises it (`psv_note_tool(&s, 6, false, T0 + 2ull * MIN)` then CHECK_EQ(r.tools_present, 0)); nothing in the firmware ever calls it that way.

**Evidence.**
```c
lens_rival.c:391-399 — "So every kind currently on the list is pushed as present, and the survey keeps the ones that stop appearing, in the past tense." ... `pharos_survey_tool((uint8_t)d.kind, true);`   (no call site anywhere passes false: `grep -rn pharos_survey_tool components/ main/` returns only this line)
```

**Why it matters.** A Flipper Zero walks past the operator at 09:00. At 17:00 the Survey headline still reads "pentest hardware in the room" (pharos_survey.c:176-177), the home ring still shows alert level 2 (lens_survey.c:78), and psv_line still prints "a pentest device is here". The past-tense branches — "a pentest device was here" and "%u pentest seen earlier" (pharos_survey.c:281-283) — are dead code on the device. This is a present-tense claim about the room that the receiver has no current evidence for, which is exactly the failure mode Rival's own comment says the survey exists to fix.

**Fix.** Two parts. (1) In pharos_survey.c, make absence non-creating so the caller can be dumb: change `if (s->n_tool >= PSV_MAX_TOOLS) { return; }` to `if (!present || s->n_tool >= PSV_MAX_TOOLS) { return; }` — a kind never heard present was never here, and must not be added as an absent row. (2) In lens_rival.c's feed block, build `bool live[PRV_KIND_COUNT] = {0}` while walking prv_device_at_now, then after the walk call `pharos_survey_tool(k, live[k])` for every k in 1..PRV_KIND_COUNT-1. With (1) in place that is safe for kinds never seen. Add an integration-shaped host test: note kind present, then feed a pass where it is absent, and assert the line flips to the past tense.

### [high] bug — The headline ladder and the ring alert ignore `weak` and `wps`, so a room of WEP networks reports "nothing alarming here"
`components/pharos_engine/pharos_survey.c:192`

**Wrong.** psv_summarise counts `weak` and `wps` and psv_line prints them, but the headline ladder branches only on tools_present, open, no_mfp, devices and networks — `weak` and `wps` never influence the headline. The lens repeats the omission for the ring alert and for the family pips. The terminal branch is reachable with genuinely bad networks because pharos_census.c only sets PC_CAP_NO_MFP for networks that are neither open nor WEP: "if (!open && !wep) { out->caps_applied |= PC_CAP_NO_MFP; }" (pharos_census.c:105-113). A WEP AP therefore arrives carrying PSV_NET_WEAK and nothing else (lens_census.c:461), and a WPA2 AP that is mfp_capable but advertises a WPS PIN arrives carrying PSV_NET_WPS and nothing else.

**Evidence.**
```c
pharos_survey.c:189-193 — "} else if (out->devices) { out->headline = "devices are naming their networks"; } else if (out->networks) { out->headline = "nothing alarming here"; }"  /  lens_survey.c:78 — "o->alert = r.tools_present ? 2u : (r.open || r.no_mfp) ? 1u : 0u;"  /  lens_survey.c:61-62 — "o->families = (uint8_t)((r.networks ? 1u : 0u) | (r.no_mfp ? 2u : 0u) | ...)" with fam_label[1] = "WEAK"
```

**Why it matters.** Three WEP access points in a small office produce networks=3, weak=3, open=0, no_mfp=0. The glass then says headline "nothing alarming here", ring dark, WEAK pip dark — while row two of the same screen says "3 use broken crypto" and the counts row says "worst grade seen: F" in red. A screen that contradicts itself is worse than either half, and the branch also states safety, which the house rules forbid outright. The same hole swallows WPS-PIN-on-WPA2, the classic Reaver target.

**Fix.** In psv_summarise, insert before the `devices` branch: `else if (out->weak) out->headline = "networks using broken crypto";` then `else if (out->wps) out->headline = "networks leaving WPS on";` (both fit the 25-char row bound; test_survey.c already checks that). Change the terminal branch from "nothing alarming here" to "nothing alarming heard yet" so it reports the sample rather than asserting the room is safe, and update the CHECK at test_survey.c:215 to match. In lens_survey.c change the alert to `(r.open || r.no_mfp || r.weak || r.wps) ? 1u : 0u` and light the WEAK pip from `(r.no_mfp || r.weak || r.wps)`. Add a host case: PSV_NET_WEAK only, and PSV_NET_WPS only, must each produce a headline that is not "nothing alarming".

### [high] dishonesty — `minutes` is wall clock since boot; the Survey is the only aggregator that drops the dwell its sources computed
`components/pharos_engine/pharos_survey.c:171`

**Wrong.** The Survey records no coverage information of any kind — `grep -n "dwell\|ceiling\|permil\|confidence" pharos_survey.c pharos_survey.h lens_survey.c` returns nothing. Its only time figure is elapsed wall time since psv_reset at boot, presented as if it were survey effort ("over %u min", "surveyed for"). Meanwhile every contributing lens computes real dwell: lens_census.c:256 builds its grading context with `.dwell_permil = pharos_radio_dwell_permil(pharos_radio_channel())` and then pushes the resulting grade into the Survey with the dwell stripped off. Elapsed time is not even Census's airtime: the tower rotates armed watches and gives Census a slice per lap (pharos_tower.h:177-188 — "a census of the networks around you is a STANDING FACT ... looking at it every lap buys nothing while costing the event detectors airtime"), so Census may hold the radio for a small fraction of the minutes displayed, each slice hearing roughly one channel in fourteen.

**Evidence.**
```c
pharos_survey.c:170-172 — "if (now_us > s->started_us) { out->minutes = (uint32_t)((now_us - s->started_us) / 60000000ull); }"  /  pharos_radio.h:99-102 — "Per mille of recent wall time spent on `channel`. This is the number the engines multiply their confidence ceiling by: camped => ~1000, hopping => roughly 1000 / n_channels."
```

**Why it matters.** Every count on this screen is a floor set by coverage, and the screen offers a number that looks like coverage but is not. An operator reads "23 networks seen here / over 40 min / nothing alarming here" and concludes the place has been characterised, when Census may have held the radio for four of those forty minutes at ~7% of one channel. This is the house rule verbatim — an absence claim on a hopping receiver must be scaled by measured dwell — and it is the one lens that breaks it, using an instrument the project already built and nine other lenses already use.

**Fix.** Add coverage to the accumulator rather than inferring it. Extend the hook to `pharos_survey_network(bssid, grade, flags, uint16_t dwell_permil)` (callers pass the value they already compute) and carry in psv_t a `uint32_t heard_ms` plus a running mean dwell advanced on each feed; expose `uint32_t listened_min` and `uint16_t coverage_permil` on psv_report_t. Render a DIM counts row "listened" / "%u of %u min" beside the existing "surveyed for", and a row "channel coverage" / "~%u%%". Keep `minutes` as elapsed but label it "elapsed", not "surveyed for". Gate the universal quantifier in finding 4 on coverage as well as sample size.

### [high] dishonesty — "every network here is floodable" fires on a sample of one, and "here" claims the room rather than the sample
`components/pharos_engine/pharos_survey.c:181`

**Wrong.** The `every` branch has no minimum-sample guard: with one BSSID heard and that one carrying PSV_NET_NO_MFP, `out->no_mfp == out->networks` is true and the device asserts a universal fact about the place. The branch directly below it carries a comment explaining exactly this class of error for the word "most", so the failure mode was understood and then not applied one line up. test_survey_quantifiers_are_honest pins {4,4} -> "every" but never tests {1,1} or {2,2}. Both this string and the "most" string say "here", scoping the quantifier to the room, when the only defensible scope is the set of networks actually heard on one band by a hopping receiver.

**Evidence.**
```c
pharos_survey.c:180-188 — "} else if (out->no_mfp && out->no_mfp == out->networks) { out->headline = "every network here is floodable"; } else if (out->no_mfp * 2u > out->networks) { out->headline = "most networks are floodable"; } else if (out->no_mfp) { /* "Most" when it is one in four is the kind of overstatement that makes a device untrustworthy the first time somebody checks it. ... */"
```

**Why it matters.** Walk into a corridor; one AP is heard in the first seconds; the headline says "every network here is floodable". Twenty seconds later a second network appears and the headline flips to "some". A headline that swings on the second sample is one nobody reads the headline of twice — which is the exact sentence the adjacent comment uses to justify the "most" fix.

**Fix.** Guard the universal: `else if (out->no_mfp && out->no_mfp == out->networks && out->networks >= 4)` -> "every network heard floods"; add a fallthrough for small samples that states the count instead of quantifying, e.g. `else if (out->no_mfp && out->no_mfp == out->networks)` -> "all %u heard are floodable" (fits 25 chars up to 3 digits, but headline is a const char* so use a small static table or reuse "some networks are floodable"). Change "here" to "heard" in both the every- and most-strings. Extend the cases table in test_survey_quantifiers_are_honest with { 1, 1 }, { 2, 2 }, { 3, 3 } asserting the non-universal wording.

### [high] blind-spot — The one screen titled "what this place is like" never states it is deaf above 2.4 GHz
`components/pharos_lens_survey/lens_survey.c:115`

**Wrong.** k_survey_row's fixed block offers exactly five context rows — surveyed for, worst grade seen, hidden names, network names leaked, counts are complete — and none of them states the receiver's band. The lens summary is "What this place is like: every network and device seen so far", the row header is "WHAT THIS PLACE IS LIKE", the first sentence is "%u networks seen here" and the headline can read "every network here is floodable". Every other summarising lens in this repo carries its deafness permanently instead: lens_spectrum.c:8-9 "on screen, permanently: this radio hears 2.4 GHz only"; lens_census.c:221 `prt_bool(&w, "band_5ghz", false); /* this radio is deaf above 2.4 GHz */`; lens_rival.c "Three rows of context first - including, permanently, the two things this receiver is structurally deaf to. A quiet Rival screen must never be read as 'there is nothing here'." Survey is the outlier, and it is the one that aggregates.

**Evidence.**
```c
lens_survey.c:115-153 — `switch (k) { case 0: ... "surveyed for" ... case 1: ... "worst grade seen" ... case 2: ... "hidden names" ... case 3: ... "network names leaked" ... case 4: ... "counts are complete" ... }`  (no band row)  vs docs/SAFETY.md:53 — "It hears 2.4 GHz only. A quiet screen is not a quiet building — most modern traffic is on 5/6 GHz, where this device is deaf."
```

**Why it matters.** In a normal dual-band home or office the 2.4 GHz radio carries the IoT gear and the guest SSID while the traffic lives on 5/6 GHz. The Survey reports "4 networks seen here / every network here is floodable" for a site with fourteen BSSIDs, and the operator has no way to know from this screen that ten of them were never eligible to be counted. PCI DSS 11.2.1 rogue-AP scanning expects 2.4/5/6 GHz coverage for exactly this reason: a single-band sensor cannot support a claim about the site. The project already wrote the sentence and put it on three other screens.

**Fix.** Add a permanent DIM row at the top of the fixed block (shift the existing cases down by one): left "hears 2.4 GHz only", right "5/6 GHz unseen", tone PHAROS_TONE_DIM, rendered whatever the counts are. Change psv_line's first sentence from "%u networks seen here" to "%u on 2.4 GHz here" (17 chars at 3 digits, inside the 25-char bound test_survey.c:163 enforces) so the band travels with the count into the console `survey` output and the headline's sibling lines too.

### [high] dishonesty — `trackable` is built from the forgeable locally-administered bit while the proven relink sits unused in the same struct
`components/pharos_lens_probe/lens_probe.c:227`

**Wrong.** psv_report_t::trackable is documented "leaking despite address randomisation" and rendered as "%u beat MAC randomising" — a claim that this receiver defeated randomisation. It is computed from bit 1 of the first address octet, which is a bit the device sets about itself: it says the device claims to be randomising, not that anything was beaten. The Probe engine meanwhile already computes the real thing — `dev->identities` ("distinct MACs linked to this device"), incremented only after an IE-fingerprint match plus a sequence-counter continuation inside PP_LINK_WINDOW_US — and raises PP_NOTE_RELINKED at pharos_probe.c:306 when identities > 1. That field is in scope at the survey call site on the very next line and is not passed.

**Evidence.**
```c
lens_probe.c:227-228 — "const bool randomised = (d->addr[0] & 0x02) != 0; pharos_survey_device(d->addr, d->n_networks, randomised);"  /  pharos_probe.c:205-208 — "if (seq_continues(c->last_seq, p->seq)) { dev = c; dev->identities++; memcpy(dev->addr, p->addr, 6);"  /  pharos_probe.c:305-307 — "if (d->identities > 1) { out->notes |= PP_NOTE_RELINKED; }"
```

**Why it matters.** A phone that randomises once at boot and never rotates during the session is counted as having "beaten MAC randomising" when nothing was beaten — a forgeable, self-declared field raising a positive claim. A phone that genuinely was followed across four MAC changes counts the same as that one. The strongest evidence the device holds about its own headline privacy finding — frames that actually arrived and were provably linked — is discarded at the hook boundary in favour of an inference.

**Fix.** Extend the hook to `pharos_survey_device(const uint8_t mac[6], uint8_t names, bool randomised, uint8_t identities)` and store `uint8_t identities` in psv_dev_t (keep the max, same rule as `names`). Split the report: `trackable` becomes the count of devices with identities > 1 (positive evidence, rendered "%u followed across MACs"), and the current LA-bit population becomes a separate `randomised` count shown only on a counts row, never as a claim that randomisation was defeated. That also makes finding 7's fix a one-line change, since identities is already the merge signal.

### [high] bug — The device dedup key is the address the Probe engine deliberately rewrites, so one rotating phone becomes N devices
`components/pharos_lens_probe/lens_probe.c:228`

**Wrong.** psv_note_device dedups on the 6-byte MAC (`same_addr` / memcmp). The lens pushes `d->addr` — which the Probe engine overwrites in place whenever it successfully relinks a randomised device to its new address. So a phone the engine has correctly merged into ONE pp_device_t is presented to the Survey under a different key every time it rotates, and the Survey allocates a fresh psv_dev_t each time, carrying the full (and monotonically growing) n_networks with it.

**Evidence.**
```c
pharos_probe.c:206-208 — "dev = c; dev->identities++; memcpy(dev->addr, p->addr, 6);"  (the merged device's address is replaced)  /  lens_probe.c:228 — "pharos_survey_device(d->addr, d->n_networks, randomised);"  /  pharos_survey.c:77-84 — "for (unsigned i = 0; i < s->n_dev; i++) { if (same_addr(s->dev[i].addr, mac)) {"
```

**Why it matters.** One phone rotating its MAC five times over an hour yields devices=5, trackable=5, and names_leaked = 5x its remembered-network list. The line "5 devices leak names" describes one device, and the inflation is exactly proportional to how hard that device is trying to be private — the opposite of what the number claims. It also burns five of the 64 PSV_MAX_DEVICES slots per phone, so dev_full and the "counts are a minimum" caveat fire early in a busy place, degrading the honesty caveat into noise. Header rule 1 ("a count of distinct addresses that actually appeared") does not rescue this, because the Survey renders those addresses as "devices".

**Fix.** Give pp_device_t a `uint8_t first_addr[6]`, set once in the creation path at pharos_probe.c:218-223 and never touched by the relink branch, and push `d->first_addr` as the survey key while keeping `d->addr` as the current address for the Probe lens's own rows. Add a host test in test_privacy.c: feed two addresses that the engine relinks by fingerprint + sequence, push both through psv_note_device keyed on first_addr, and assert r.devices == 1 and names_leaked equals the single device's list length.

### [high] ux — psv_tool_t::first_us and last_us are maintained and never read; the header's own worked example is unimplemented
`components/pharos_engine/pharos_survey.c:162`

**Wrong.** psv_note_tool maintains first_us on insert and advances last_us on every present sighting, but psv_summarise's tool loop reads only `kind` and `present`. No duration is ever derived, so nothing reaches psv_report_t, psv_line, the lens rows, or the console `survey` command. The header opens by advertising precisely this as one of the four things the Survey answers.

**Evidence.**
```c
pharos_survey.h:25 — "A Flipper Zero was present for 3 minutes."  /  pharos_survey.c:162-167 — "for (unsigned i = 0; i < s->n_tool; i++) { out->tools++; if (s->tool[i].present) { out->tools_present++; } }"
```

**Why it matters.** How long the thing was here is the one fact a session log holds that a live reading structurally cannot — it is the stated reason the Survey exists at all (lens_survey.c:9-11, "The device knew something worth knowing about the room and had nowhere to put it"). It is collected, stored in the struct, and dropped on the floor. "A pentest device was here" tells an operator nothing actionable; "was here 40 min" tells them whether somebody sat down.

**Fix.** Add `uint32_t tool_longest_min` to psv_report_t, computed in the existing loop as `(uint32_t)((s->tool[i].last_us - s->tool[i].first_us) / 60000000ull)` with a `last_us >= first_us` guard, keeping the maximum across kinds. Add a psv_line variant to the tools block: present -> "here for %u min" when tool_longest_min, past tense -> "was here %u min" (both inside the 25-char bound at three digits). Note this only becomes truthful once finding 1 is fixed — until a tool is ever marked absent, last_us advances forever and the duration is just uptime.

### [medium] blind-spot — The survey never resets and has no reset control, so "this place" can be three places
`components/pharos_ui/pharos_ui.c:2026`

**Wrong.** psv_reset is called exactly once, during UI init. There is no reset path anywhere else: the console registers a read-only `survey` command (console_glue.c:909 -> cli_survey, which only reads), and k_survey has no `on_select`, so the centre tap — which the lens contract documents as the operator's one live control — is inert on this lens. Counts only rise. Yet the device already knows when the operator has moved: pharos_ui_has_travelled() exists and lens_vigil.c:143 already consumes it.

**Evidence.**
```c
pharos_ui.c:2026 — "psv_reset(&s_survey, (uint64_t)esp_timer_get_time());"  (only call site)  /  lens_survey.c:159-173 — the k_survey initialiser has .on_mount, .display and .row but no .on_select  /  lens_vigil.c:143 — "pv_set_moved(&s_engine, pharos_ui_has_travelled(0));"
```

**Why it matters.** Carry the device from home to a train to an office and the Survey merges all three into one page headed "WHAT THIS PLACE IS LIKE", with a headline that says "every network here". The operator who wants a clean read of the room they are standing in has no way to get one short of a power cycle, and nothing on the screen warns that the accumulated picture spans places. That undermines every count on the page, because the page's whole premise is spatial.

**Fix.** Three small changes. (1) Give k_survey an `.on_select` that calls a new `pharos_survey_restart()` wrapping psv_reset — consistent with the centre tap being the operator's live control on other lenses — and show a one-frame "survey restarted" in `why`. (2) Add `survey reset` to cli_survey (argc check, then the same call). (3) Record the step count at psv_reset in psv_t, expose `bool moved` on psv_report_t from pharos_ui_has_travelled(mark), and render a DIM row "moved since this began" / "yes" rather than silently merging places.


## watch

**What it does.** Watch counts 802.11 deauthentication/disassociation frames in a trailing 15 s window and grades them out of 100 across four families — duty-corrected RATE, targeting SHAPE, FORGERY (802.11w contradiction, sequence order, frozen counter, RSSI split, ghost source) and AFTERMATH (a rejoin stampede after a burst) — capped by a dwell-derived confidence ceiling that a "hard" contradiction may raise to 88, with the lens camping the radio on the pressure channel for 20 s when the RATE family fires.

**Prior art.** Kismet (phy_80211.cc, kismet_alerts.conf): DEAUTHFLOOD is a per-BSSID counter — `if (now - ...get_client_disconnects_last() > 1) set_client_disconnects(1); else inc_client_disconnects(1);` then `if (...get_client_disconnects() > 10)`, i.e. >10 disconnects for ONE BSSID inside one second. The `alert=DEAUTHFLOOD,5/min,2/sec` line in kismet_alerts.conf is alert rate-limiting ("the rate defines the number of total alerts per time period which may be raised for each alert type... the burst rate defines the number of alerts which can be sent before throttling"), not a detection threshold — our engine cites those two numbers as thresholds. Kismet also keeps a SEPARATE broadcast-disconnect alert and documents its benign cause ("Either the AP is shutting down or this is indicative of a possible denial of service attack"), and its DISASSOCTRAFFIC alert carries an explicit channel-hopping false-positive warning, which is the same honesty posture Pharos takes. nzyme (nzyme.org/docs, /knowledge/wifi): calls them "disconnection frames" and counts frames "addressed at or originating from access points of a monitored network" — i.e. it keeps direction (AP-sourced vs station-sourced) explicit, which our hit ring discards; and instead of inferring forgery from absence it compares a per-BSSID frame FINGERPRINT (hash of fixed frame parameters) against an operator-supplied expected set, alerting on any unknown fingerprint — positive evidence about a frame that arrived, exactly the principle in our house rules, applied where we use "never heard it beacon". 802.11w/BIP (IEEE 802.11w-2009, CWNP): unicast robust management frames are CCMP-encrypted and "Protected frame field of frame control field is set"; group-addressed ones are integrity-protected ONLY, via an appended Management MIC IE (MMIE), not encrypted and with no Protected bit — so a WIDS that equates "Protected bit clear" with "not MFP-protected" mis-reads every legitimate broadcast deauth on an 802.11w network. Standard behaviour also requires an AP to send deauth with reason 6/7 unprotected to a station with which no security association exists ("no PMF protection for frames transmitted before the 4-way handshake completes"). Sequence-number spoof detection (Guo & Chiueh-style, which our header already cites as gap-threshold-prone): our order-only test is a genuine improvement over the literature's gap thresholds — the defects below are in the bound that guards it, not in the idea.

### [high] dishonesty — Do not read a BIP-protected broadcast deauth as proof of forgery
`components/pharos_engine/pharos_watch.c:716`

**Wrong.** PW_FORGE_MFP_PROOF is raised whenever >=2 disconnects from the dominant source lack PHAROS_DOT11_F_PROTECTED on a network advertising MFP-required. But 802.11w protects GROUP-ADDRESSED robust management frames with BIP: they are integrity-protected only, never encrypted, and the Protected Frame bit stays 0 — the proof lives in an appended MMIE (element ID 76) that this firmware never parses. So every genuine broadcast deauth/disassoc an 802.11w AP sends (shutdown, channel move, band steering) is scored as a frame that 'could not have come from the device it names'. The codebase already defines the right flag for this, PHAROS_DOT11_F_MFP_SEEN in pharos_core/include/pharos_event.h:52 ('frame carried an MMIE'), and nothing in pharos_radio.c ever sets it — the detector is reading its own missing instrumentation as its single strongest finding, and that finding also sets hard=true, which raises the ceiling to 88.

**Evidence.**
```c
if (claimed->rsn_flags & PHAROS_RSN_F_MFP_REQUIRED) {
            out->notes |= PW_NOTE_MFP_TARGET;
            if (dom_unprotected >= 2) {
                out->forgery |= PW_FORGE_MFP_PROOF;
                forge += 24;
                hard = true;
            }
        }
/* and the counter it rests on, line 573: */
            if (!(h->flags & PHAROS_DOT11_F_PROTECTED)) {
                unprot++;
            }
```

**Why it matters.** Reproduced on the host (engine compiled standalone): an MFP-required AP beaconing normally that sends 3 broadcast deauths with its OWN sequence counter and its OWN RSSI yields forge=0x01 (MFP_PROOF), PW_NOTE_HARD set, ceiling raised, band ELEVATED 44, and the glass prints the strongest 'why' line in the product — 'unprotected on MFP net' — about an access point that is simply rebooting. Kismet's own broadcast-disconnect alert text names that exact benign cause. This is the one claim the engine calls a contradiction rather than an estimate, so it is the one that must never be wrong.

**Fix.** Two parts, both receive-only. (1) Instrument: in pharos_radio.c's management-frame path, for DEAUTH/DISASSOC walk the element chain starting at DOT11_HDR_MIN+2 (after the reason code) with the existing pharos_dot11_find_ie_from(); if element ID 76 (MMIE) of length 16 or 24 is present, set PHAROS_DOT11_F_MFP_SEEN. (2) In pw_evaluate, count a frame toward dom_unprotected only when it carries neither PHAROS_DOT11_F_PROTECTED nor PHAROS_DOT11_F_MFP_SEEN. Until (1) ships, exclude group-addressed frames (mac_is_broadcast(h->dst)) from dom_unprotected entirely and let them reach at most PW_FORGE_MFP_HINT — a broadcast deauth with no visible MMIE is suspicious, but it is not the dwell-independent proof the header promises.

### [high] bug — Stop counting sequence violations from outside the analysis window
`components/pharos_engine/pharos_watch.c:734`

**Wrong.** claimed->seq_back and claimed->seq_fwd are incremented in pw_observe() and never decremented, aged or bucketed by time. They are cleared only by memset() in ap_admit() when the BSSID is first admitted or evicted. pw_evaluate() — documented as 'Grade the trailing ctx->window_ms ending at now_us' — sums the all-time totals, so a burst of violations from any point in the device's uptime keeps scoring in every later window, keeps the FORGERY family lit, keeps setting hard=true (ceiling 88) and keeps printing 'counter went backwards' on the glass.

**Evidence.**
```c
        const uint32_t viol = (uint32_t)claimed->seq_back + claimed->seq_fwd;
        out->seq_violations = (uint16_t)clamp_u32(viol, 0, 0xFFFF);
        if (claimed->beacons >= 3 && viol >= 3) {
            out->forgery |= PW_FORGE_SEQ_ORDER;
            forge += 12 + clamp_u32(viol - 3u, 0, 8);
            hard = true; /* order violations survive hopping: they are not rates */
        }
```

**Why it matters.** Reproduced: after a 60-violation attack ends, five minutes later the same AP legitimately disconnects two clients — the verdict reports seq_violations=60, c_forgery=20, the FORGERY family lit and PW_NOTE_HARD set, on two frames that are beyond reproach. The operator is shown a red 'counter violations 60' and the why-line 'counter went backwards' for an event that is not in the window they are looking at. Every other number on that page (observed, peak, burst, rejoins) is windowed; this one silently is not.

**Fix.** Window the violations the way the disconnects are already windowed. Cheapest fix that keeps the struct small: replace seq_back/seq_fwd (uint16 each) with a 16-bit-per-second bitmap plus a seconds stamp — e.g. `uint32_t viol_sec; uint16_t viol_bits;` marking which of the last 16 seconds saw a violation — or keep `uint64_t last_viol_us` plus a count that is zeroed in pw_observe whenever the new violation is more than PW_WINDOW_SLOTS seconds after the previous one. In pw_evaluate, count only violations inside [start_us, now_us]. Add a regression test: attack, advance 5 minutes, feed two genuine disconnects, assert seq_violations == 0 and (notes & PW_NOTE_HARD) == 0.

### [high] bug — Make the sequence bound uninformative when the AP's counter rate could not be measured
`components/pharos_engine/pharos_watch.c:164`

**Wrong.** ap_note_seq_rate() discards any beacon-to-beacon sample whose advance exceeds 2048, reading it as 'the counter went backwards'. But a busy AP heard once per hop cycle is exactly that: at 2.6 s between heard beacons (13 channels x 200 ms survey dwell) an AP carrying ~800 frames/s advances more than 2048 every time, so EVERY rate sample is thrown away, seq_rate_peak stays 0, and ap_seq_bound() collapses to its bare 96-step floor. A genuine deauth arriving 150 ms after the last heard beacon has then advanced ~120-450 steps, fails seq_plausible(), and is counted as an order violation. Above ~4096 steps per heard interval the same guard aliases the wrap and measures a rate far too LOW, with the same result. The header states the opposite guarantee.

**Evidence.**
```c
    const uint16_t adv = seq_ahead(ap->last_beacon_seq, new_seq);
    if (adv > 2048u) {
        return; /* the AP's own counter went backwards: not a rate sample */
    }
/* and the bound it starves, line 188: */
    return ((rate * elapsed_ms) / 1000u) * 4u + 96u;
/* against the promise at line 283: 'Guarded by ap_seq_bound so a busy AP - whose
   counter runs fast on data frames we never see - is never accused.' */
```

**Why it matters.** Reproduced across a sweep: a hopping receiver (dwell_permil 77) watching ONE honest AP that sends only genuine deauths. With 2 beacons heard per visit the engine is clean at every traffic level. With 1 beacon heard per visit — which is what happens whenever a beacon is lost to contention, a full bus or PHY error — an AP at 900 fps or 3000 fps produces seq_violations=14, PW_FORGE_SEQ_ORDER, PW_NOTE_HARD and the FORGERY family lit. That is the ceiling-raising, corroboration-granting family firing on the busy-AP negative the design was built to refuse, in the posture the device ships in.

**Fix.** An interval whose advance cannot be read must produce an uninformative bound, not the floor. Add `bool seq_rate_unknown;` and `uint16_t seq_rate_dt_ms;` to pw_ap_t: set seq_rate_unknown = true in ap_note_seq_rate() on the `adv > 2048` discard (and on the dt_ms < 10 / > 5000 discards), clear it and record dt on a usable sample. In ap_seq_bound(), `if (ap->seq_rate_unknown || elapsed_ms > 4u * ap->seq_rate_dt_ms) return 4096u;` — seq_plausible() already treats any bound >= 3000 as 'refuse to draw a conclusion', so the test goes silent instead of accusing. Add the negative test alongside test_watch_seq_gaps_are_not_violations: one beacon per 2.6 s visit, 900 fps of unseen traffic, genuine deauths, assert seq_violations == 0.

### [high] bug — Count each rejoin second once instead of once per overlapping burst
`components/pharos_engine/pharos_watch.c:785`

**Wrong.** For every second whose disconnect count clears burst_min, the loop adds the rejoins of the following three seconds. During a sustained flood most seconds qualify, so the same rejoin seconds are added two or three times over. rejoins_after therefore is not a subset of rejoins: it routinely exceeds it, which both inflates the interp() curve and makes the `share >= 75` bonus automatic — the very branch whose comment promises that 'a network where clients associate all day scores nothing here'.

**Evidence.**
```c
            if (s->disconnects < burst_min) continue;
            /* the three seconds that follow this burst */
            for (uint32_t d = 1; d <= 3; d++) {
                const uint32_t sec = s->sec + d;
                if (sec > now_sec) break;
                const pw_slot_t *nx = &e->slots[sec % PW_WINDOW_SLOTS];
                if (nx->sec == sec) {
                    rejoins_after += nx->rejoins;
                }
            }
/* and the share it then computes, line 806: */
        const uint32_t share = rejoins ? (rejoins_after * 100u) / rejoins : 0u;
```

**Why it matters.** Reproduced: a 10-second flood with one rejoin pair per second reports rejoins_after=48 of rejoins=20; a flapping-client scenario reports 144 of 60. AFTERMATH is one of the two corroborating families that unlock the alarm band, and here it pins at 18/18 on traffic that never justified it. The lens prints the raw pair on the case-file page — row 12 renders 'clients came back  144/60', a ratio above 100% that tells the operator the device cannot count.

**Fix.** Mark, then sum. Build a 16-bit mask over the window's seconds: first pass sets bit (sec % PW_WINDOW_SLOTS) for each of the three seconds following each qualifying burst second; second pass adds each marked slot's rejoins exactly once. Clamp rejoins_after to rejoins as a belt-and-braces invariant and assert it in the test. Separately, rejoins are tallied address-blind at line 266 (`slot_for(e, sec)->rejoins++`), so associations to unrelated neighbouring APs count as 'clients came back' — the auth/assoc frame's a1 is the BSSID, so at minimum compute the share against rejoins whose a1 matches out->src.

### [high] dishonesty — A station is not expected to beacon - do not call its own disconnect a ghost
`components/pharos_engine/pharos_watch.c:699`

**Wrong.** PW_FORGE_GHOST fires whenever the dominant source has never been heard to beacon, scaled by dwell. Stations never beacon. A laptop or phone leaving a network, sleeping, or flapping sends deauth/disassoc under its OWN address (a2 = station, a3 = BSSID), so every station-sourced disconnect scores 14 forgery points at camped dwell — past the c_forgery >= 10 threshold that lights the FORGERY family, which is one of the two families that count as corroboration. The engine has the direction information and throws it away: pharos_ev_dot11_t carries a3 and PHAROS_DOT11_F_TO_DS, but pw_hit_t (pharos_watch.h:279) stores only src/dst and pw_observe never compares a2 with a3.

**Evidence.**
```c
    if (!claimed || claimed->beacons == 0) {
        /* Never heard this BSSID beacon. Suggestive of a forged source, but
         * while hopping we very plausibly just missed the beacons, so the
         * points are scaled by how much of the channel we actually heard. */
        const uint32_t ghost = (14u * dwell) / 1000u;
        if (ghost) {
            out->forgery |= PW_FORGE_GHOST;
            forge += ghost;
        }
```

**Why it matters.** The dwell scaling only protects the hopping posture — and the lens camps itself (lens_watch.c:237) the moment the RATE family fires, which drives dwell to ~1000 and the ghost term to its full 14. Reproduced: ONE flapping client, camped, every frame genuine and station-sourced, 3 disassoc+reauth cycles a second for 10 s. Verdict: 61/96 SUSPICIOUS with ALL FOUR families lit, why-line 'source never beaconed', hint 'Shape is wrong. Camp to confirm.' Fourteen points short of FLOOD LIKELY for a misbehaving phone. Every disconnect in test_watch.c is AP-sourced (a2 = ap->bssid), so this negative is untested. nzyme keeps 'addressed at' and 'originating from' the AP as distinct counts for exactly this reason.

**Fix.** Add `uint8_t bssid[6];` (a3) to pw_hit_t and copy f->a3 in pw_observe. In pw_evaluate, when the dominant source is station-sourced (!mac_eq(h->src, h->bssid), or PHAROS_DOT11_F_TO_DS set), suppress PW_FORGE_GHOST outright — a station has no beacon to be missing — and prefer the AP-sourced subset when choosing the dominant source. Consider tallying station-sourced disconnects into a separate counter so RATE and SHAPE are computed over frames that claim to come from an access point. Add a negative test: a client that flaps for ten seconds must not light the FORGERY family.

### [high] bug — Bound the burst run to half a second and key it on the transmitter too
`components/pharos_engine/pharos_watch.c:499`

**Wrong.** The run that produces max_burst is keyed on destination alone and is bounded only by the gap between ADJACENT frames, not by the span of the run. So (a) a slow trickle at one victim grows without limit as long as no two consecutive frames are more than 500 ms apart, and (b) broadcast disconnects from different transmitters merge into one run, because ff:ff:ff:ff:ff:ff is one destination. Both contradict the comment that defines the signal, and both feed the +5/+8 shape points that let SHAPE fire on its own.

**Evidence.**
```c
    /* Burst shape: the longest run of consecutive disconnects aimed at one
     * destination inside half a second. Every deauth tool emits in tight runs
     * (aireplay-ng defaults to 64 per burst); an access point disconnecting a
     * client that timed out sends one or two. */
...
        if (run_open && mac_eq(run_dst, h->dst) &&
            (run_last_us - h->t_us) <= 500000ull) {
            run_len++;
```

**Why it matters.** Reproduced: 2.2 disconnects per second at one victim for 12.6 s reports max_burst=28 and c_shape=8 — the SHAPE family lit by a 'burst' that is 25x longer than the half second the comment claims, at a rate an order of magnitude below the 16/s the '8 inside half a second' rule describes. Separately, six neighbouring APs each sending one broadcast disassoc per second report max_burst=6 from six different radios. The case-file row 'longest burst' tells the operator one transmitter hammered one address; neither number means that.

**Fix.** Track run_start_us alongside run_last_us and close the run when `run_start_us - h->t_us > 500000ull` (walking backwards), so the whole run must lie inside half a second as documented. Add the source to the run key: `mac_eq(run_dst, h->dst) && mac_eq(run_src, h->src)`. Keep the existing thresholds — with the run correctly bounded, >=8 inside 500 ms is once again the tool signature it was meant to be. Add a test asserting a 2/s trickle yields max_burst <= 2.

### [high] dishonesty — Correct the Kismet anchor - 5/min and 2/sec are alert throttles, not thresholds
`components/pharos_engine/include/pharos_watch.h:37`

**Wrong.** The rate curve is justified by a citation that does not say what the comment claims. `alert=DEAUTHFLOOD,5/min,2/sec` in kismet_alerts.conf is Kismet's alert RATE LIMITING — its own documentation reads 'The rate defines the number of total alerts per time period which may be raised for each alert type. The burst rate defines the number of alerts which can be sent before throttling takes place.' Kismet's actual detection test, in phy_80211.cc, is `if (dot11info->bssid_dot11->get_client_disconnects() > 10)` against a counter that resets after one idle second: more than ten disconnects for ONE BSSID inside one second. Our curve starts scoring at 0.08/s and calls 2/s 'a burst worth an alert', two orders of magnitude below the cited source, and it counts every BSSID in the air together where Kismet counts per-BSSID.

**Evidence.**
```c
 *   RATE (0..34)      How much disconnect traffic there is, duty-corrected,
 *                     plus the peak second. Anchored on Kismet's long-standing
 *                     DEAUTHFLOOD thresholds (5/min sustained, 2/sec burst) so
 *                     "background" and "flood" mean what a WIDS operator
 *                     already expects them to mean.
/* and pharos_watch.c:655 */
    /* Anchored on Kismet's DEAUTHFLOOD defaults so the words mean what a WIDS
     * operator expects: ~0.08/s (5/min) is where "notable" starts and 2/s is a
     * burst worth an alert. */
```

**Why it matters.** The promise in that comment is that an operator reading FLOOD LIKELY can map it onto what their WIDS would have said, and they cannot. In practice PW_FAM_RATE lights at about 1.4/s aggregated across every access point in earshot, or at any single second holding five disconnects from any mix of sources — routine in a station or an office. That is not only a scoring matter: lens_watch.c:237 uses PW_FAM_RATE alone to seize the radio and camp for 20 s, so the device stops surveying because six APs housekept at once.

**Fix.** Fix the comment first — cite phy_80211.cc's >10 per BSSID per second and note that kismet_alerts.conf's pair is a throttle. Then decide the anchor deliberately: keep the low start if the aim is a sensitive hint, but compute the RATE family over the DOMINANT SOURCE's disconnects rather than the air-wide total (the hit ring already carries per-source counts, and dom_hits is already computed) so a room full of healthy APs cannot accumulate into 'notable'. If the aggregate is kept, say so in the comment and raise the first knee toward Kismet's 10/s so the words keep their meaning.

### [medium] ux — Show the notes - the operator never learns the numbers were qualified
`components/pharos_lens_watch/lens_watch.c:335`

**Wrong.** The engine computes eight note bits describing how much the verdict should be trusted — PW_NOTE_NO_RATE, PW_NOTE_SAMPLED, PW_NOTE_THIN_DWELL, PW_NOTE_LOSSY, PW_NOTE_SHORT_WINDOW, PW_NOTE_HARD, PW_NOTE_MFP_TARGET, PW_NOTE_PROTECTED — and the lens renders none of them. k_watch_display() (line 379) uses band, score, families, why and history; the twenty case-file rows (line 458) never mention notes; the only consumer is the JSON report, as a bare integer.

**Evidence.**
```c
    prt_u32(&w, "notes", v.notes);
/* the only place v.notes is read in the whole lens - grep for PW_NOTE_ in
   components/pharos_lens_watch/ returns nothing */
```

**Why it matters.** Two of these change what the numbers on the glass mean. PW_NOTE_NO_RATE says the engine refused to quote a rate because it had under 400 ms of channel time — the rate row then reads 'rate 0/34' in dim, which an operator reads as 'no disconnect traffic', the opposite of 'I could not measure'. PW_NOTE_SAMPLED says the shape figures came from a sample, which is guaranteed during a real flood: PW_MAX_HITS is 256 while the window is 15 s, so above ~17 frames/s the broadcast share, victim count, burst and reason monoculture describe only the last few seconds. The device is at its least transparent exactly when it matters.

**Fix.** Add one row at the end of the case file — left 'observation', right a compact code built from the notes (e.g. 'HOP/SAMP', 'NO RATE'), tone WARN when any of NO_RATE, SAMPLED, LOSSY or SHORT_WINDOW is set. Cheaper and sharper: when PW_NOTE_NO_RATE is set write the rate row's right column as '--/34' instead of '0/34', and when PW_NOTE_SAMPLED is set append '*' to the four shape rows. Both fit the existing 12-character right column.

### [medium] dishonesty — Do not paint a test that never ran green
`components/pharos_lens_watch/lens_watch.c:531`

**Wrong.** The 'counter violations' row is the only PHAROS_TONE_GOOD in the lens, and it is given on a zero that can mean two different things. pw_evaluate assigns out->seq_violations only inside the branch where the claimed BSSID was actually heard beaconing (pharos_watch.c:735); when the source was never heard — the PW_FORGE_GHOST path — seq_violations stays at its memset zero, as do rssi_delta and rssi_spread. The case file then shows 'counter violations 0' in green and 'level vs its beacon 0dB/0' in neutral while the FORGE pip is lit on the live face.

**Evidence.**
```c
        snprintf(out->left, sizeof(out->left), "counter violations");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.seq_violations);
        out->tone = v.seq_violations ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
```

**Why it matters.** Green is the one colour this firmware otherwise refuses to use, because 'we found nothing' is not 'nothing is there' on a receiver that hears one channel at a time. Here it is shown for a test that could not run at all — and shown on the same page as a lit forgery pip, which reads as the device contradicting itself. Reproduced in the ghost path: forgery=0x20 with seq_violations=0 and rssi_delta=0.

**Fix.** Give the verdict an explicit 'this test ran' signal rather than inferring it from a zero: either add a note bit (PW_NOTE_SEQ_TESTED, set when claimed && claimed->beacons >= 3) or have the lens ask for the same condition. Then row 11 prints '-' with PHAROS_TONE_DIM when the test did not run, TONE_GOOD only when it ran and found nothing, TONE_BAD when it found something; row 10 does the same for the level split instead of printing a measured-looking '0dB/0'.

### [medium] blind-spot — The lens is deaf to disconnection attacks that use no disconnect frame
`components/pharos_engine/pharos_watch.c:270`

**Wrong.** pw_observe() drops every management subtype that is not beacon, probe response, auth, assoc/reassoc request, deauth or disassoc. The standard pivot against a network where plain deauth does not work — an MFP-required network, which is exactly the network this engine is proudest of judging — is to forge a Channel Switch Announcement (element 37 in a beacon, or a spectrum-management action frame) and walk every client off to a dead channel, or to starve them with the Quiet element. Neither emits a single deauth, so Watch reports QUIET while the network empties. A grep for 'channel switch', 'CSA', 'quiet element' and '802.11h' across components/ and docs/ returns nothing: no other lens covers it either.

**Evidence.**
```c
    if (f->subtype != PHAROS_ST_DEAUTH && f->subtype != PHAROS_ST_DISASSOC) {
        return;
    }
```

**Why it matters.** On an 802.11w network the deauth families can only ever report absence, and the band advice for QUIET is 'No disconnect traffic in view' — true, and the wrong question. An operator holding the device in a corridor during a CSA attack gets QUIET with a clean conscience. The evidence would be strongly positive rather than absent: a CSA element appearing in beacons from a BSSID that has never announced one before, with a switch count that never counts down, or arriving at an RSSI outside the spread that BSSID's own beacons occupy — the same RSSI-split machinery pw_ap_t already keeps.

**Fix.** Receive-only and cheap. In the radio's beacon path, run pharos_dot11_find_ie_from() for element 37 (Channel Switch Announcement) and 36 (Quiet); when present, carry the new channel and switch count on the event. In the watch engine, store per-AP 'has ever announced a switch' plus the last announced target, and raise a distinct finding when a CSA claims a BSSID whose beacons keep arriving on the old channel afterwards, or whose CSA-bearing beacon's RSSI is outside rssi_ewma_x4 +/- 3*rssi_dev_x4. This is a new family, not a tweak to an existing one, so it deserves its own band text and its own negative test (a genuine DFS radar move or a band-steering AP must not fire it).


## whisper

**What it does.** Whisper runs five integer Goertzel probes (1, 18, 19, 20, 21 kHz) over 10 ms / 480-sample microphone windows, keeps no audio, and scores three families — LEVEL (margin over a per-band noise floor), NARROW (strongest ultrasonic probe minus the next strongest) and PERSISTENT (duty cycle over a 32-window / 320 ms bit history) — into a 0..100 QUIET/TRACE/TONE PRESENT/PERSISTENT/BEACON LIKELY band with ceilings for a deaf mic, a loud room, a short history and the 21 kHz edge of the capture path, then holds the displayed verdict still with a 4-evaluation confirm counter and a 5-point hysteresis kerb.

**Prior art.** I compared against three concrete bodies of work, reading source and config rather than descriptions.

(1) SoniControl (FH St. Pölten, GPLv3) — the only published open-source ultrasonic firewall. I read the Octave reference detector at github.com/fhstp/SoniControl/SoniControl_Octave/detector/. Its constants are explicit: `bufferSizeInMS=46.440` (2048-sample FFT), `backgroundBufferSize = 10` seconds, `medianBufferSize = 1.5` seconds, `cutoffFrequency = 16800` with the comment "lower limit for prontoly! All other technologies send above 18kHz", `decisionThreshold = 0.5` (KL divergence), and `decisionThresholdNearby = 3.5` ("If the RMS Energy in the Nearby band is N-times higher than in the neighboring bands above and beneath"). Four design choices differ from ours in ways that matter. (a) Its background model is `median(backgroundModelBuffer,2)` — a per-bin MEDIAN over ten seconds. Ours is a one-directional ratchet with no median and no bound (Finding 1). (b) It FREEZES the background model while a detection is live ("if currently a detection ... is active, do not further update the background model ... This avoids that the model learns wrong information"), which is the correct way to get the property our comment claims; ours gets it by never adapting at all. (c) `normalizeSpectrum` divides each frame by its own sum, so broadband level changes cancel before any comparison; we compare absolute dB figures. (d) It analyses EVERY FFT bin from 16.8 kHz up (~23 Hz resolution), not four probes 1 kHz apart (Findings 3 and 5). Its persistence rule is "more than half of the past 1.5 s of frames", against our 320 ms.

(2) Arp, Quiring, Wressnegger & Rieck, "Privacy Threats through Ultrasonic Side Channels on Mobile Devices" (IEEE EuroS&P 2017) — the canonical measurement of the real deployments (234 Android apps carrying the SilverPush SDK). SilverPush occupies roughly 18.7–19.5 kHz, and the modulation is an FSK alphabet: each symbol gets its own frequency inside the band, sent one after another. Our four probes at 1 kHz spacing do not resolve a sub-kHz FSK alphabet, and the symbol hopping deliberately defeats a single-band narrowness test (Findings 3 and 4).

(3) Google Nearby / Chromecast guest-mode pairing, described in the literature as direct-sequence spread spectrum in the 18.5–20 kHz band at 94.5 b/s, plus the "sparse, stable comb of strong tones" fingerprinting described by the ultrasniff Android project (github.com/Mavic-Pro/ultrasniff, 15–24 kHz waterfall). Both make the same point: a real beacon is usually a COMB or a SPREAD, not a single steady tone. Our NARROW family is written to reject exactly that shape (Finding 4).

Two things our lens does that the prior art does not, and that I found no reason to change: the structural no-buffer Goertzel (SoniControl keeps a 10-second cyclic sample buffer and a `bufferHistory` of raw audio; we genuinely cannot retain audio), and the explicit DEAF/short-history ceilings. SoniControl has no equivalent of "I cannot hear" as a distinct answer.

Sources: https://github.com/fhstp/SoniControl , https://ar5iv.labs.arxiv.org/html/1807.07617 , https://mlsec.org/docs/2017a-eurosp.pdf , https://github.com/Mavic-Pro/ultrasniff

### [high] bug — Make the noise floor adaptive in both directions; today it ratchets only toward silence and calls an empty room a beacon
`components/pharos_engine/pharos_acoustic.c:147`

**Wrong.** pac_observe() only ever moves floor[b] toward QUIETER. The 'louder than the floor' branch is empty, so the floor is a monotonic non-decreasing ratchet. Because to_dbfs() is an integer log2 of a single 480-sample Goertzel bin, the per-window level of stationary Gaussian noise swings over ~39 units (a 2-d.o.f. power estimate fades to mag2=0 -> level 120), and the ratchet chases those fades. The floor therefore converges on the DEEPEST FADE of the estimator rather than on the room, climbing 1 unit per 10 ms window with no bound until it hits 120 in every band. Once the floor sits 9 units above the typical level, `present` is true in essentially every window, so LEVEL, NARROW and PERSISTENT all light with no tone in the room at all. The comment above the code claims two behaviours the code does not implement: 'a room with a constant hum settles wherever that hum is' (the floor can only settle on a hum that was already present in window 1, because floor_valid is set after the first window and the floor can never come back down) and 'Rising fast and falling slowly' (it never falls).

**Evidence.**
```c
        /* The noise floor is a slow MAXIMUM of the quiet level - remember,
         * smaller means louder - so a band that is usually silent has a floor
         * near 120 and a room with a constant hum settles wherever that hum
         * is. Rising fast and falling slowly means a beacon cannot raise its
         * own floor and hide inside it. */
        if (!e->floor_valid) {
            e->floor[b] = lvl;
        } else if (lvl > e->floor[b]) {
            e->floor[b] = (uint8_t)(e->floor[b] + 1); /* quieter: drift up slowly */
        } else if (lvl < e->floor[b]) {
            /* louder than the floor: do not chase it, that is the signal */
        }
```

**Why it matters.** I reproduced this on the host with two independent noise generators. Feeding ONLY stationary broadband noise at amp=400 (~-38 dBFS, ordinary room noise with the lens's own +30 dB mic gain) and no tone whatsoever: at t=0.3 s one seed already reports 'BEACON LIKELY 80/92, fams=0x07, duty=96%, 18 kHz' and holds it for over ten seconds; by t=30 s every one of six seeds reports at least PERSISTENT (60-68) and the floors have ratcheted to 109/116/120/120. Across 180 runs (15 amplitudes x 12 seeds x 60 windows) the engine NEVER once returned QUIET -- the bottom rung of the vocabulary is unreachable in any room that is not perfectly silent, and perfect silence is the DEAF path. The second half is just as bad: walk out of a quiet room into a shop with a constant 19 kHz presence sensor and the engine reports BEACON LIKELY 80/92 permanently, with no recovery after 20 s of simulated time, because the floor learned the quiet room and can never come down. This is the single failure that makes every other honesty control in the lens moot, and the lens's own display comment already documents the symptom ('this lens sat at ELEVATED in every ordinary room'). The existing test does not catch it because test_acoustic_quiet_room() runs only 24 windows, before the ratchet has done its work.

**Fix.** Three integer-only changes in pac_observe(), no float, no malloc, no transmission. (a) Smooth before the floor sees it: add `uint8_t smooth[PAC_BAND_COUNT]` and compute `e->smooth[b] = (uint8_t)((3u*e->smooth[b] + lvl) / 4u);` then use `smooth` (not `lvl`) for both the floor update and the `present` test -- this alone removes the 39-unit estimator swing that the ratchet is chasing. (b) Add the missing downward branch, rate-limited and gated so a beacon still cannot hide inside its own floor. Add `uint8_t floor_cool[PAC_BAND_COUNT]` and replace the empty else-if with: `else if (sm < e->floor[b]) { if ((e->seen[b] & 0xFFFFu) == 0xFFFFu) { if (++e->floor_cool[b] >= 16u) { e->floor[b]--; e->floor_cool[b] = 0; } } else { e->floor_cool[b] = 0; } }`. Sixteen unbroken windows (160 ms) of presence before the floor may move, then one unit per 16 windows: a standing hum is absorbed in ~6 s, while a 1-second beacon burst moves the floor by at most 6 units and cannot bury its own 9-unit threshold. This is SoniControl's 'do not update the background model while a detection is active', expressed as a rate limit instead of a freeze. (c) Bound the ratchet: `if (e->floor[b] > (unsigned)e->smooth[b] + 24u) e->floor[b] = (uint8_t)(e->smooth[b] + 24u);` so the floor can never sit more than 24 units (8 doublings) above what the band is actually doing. Then add the regression the suite is missing to test/host/test_acoustic.c: 3000 windows (30 s) of stationary broadband noise at several amplitudes and several seeds must never exceed PAC_BAND_TRACE and must never set PAC_FAM_PERSISTENT.

### [high] bug — Let the named frequency change while the severity holds; the confirm counter can never adopt a new band
`components/pharos_engine/pharos_acoustic.c:461`

**Wrong.** pac_hold_apply() uses ONE counter (h->agree) for two independent decisions: the severity band and the named frequency. When the fresh verdict's severity equals what is displayed, the function takes the early-return branch and unconditionally resets `h->agree = 0`. The only place h->shown_band is ever written (outside priming) is the `h->agree >= PAC_CONFIRM` branch, which is only reachable when v->band != h->shown. So while the severity is stable the frequency counter is reset on every single tick and can never reach PAC_CONFIRM. The line `if (v->strongest == h->shown_band) { h->cand_band = h->shown_band; }` is dead weight -- it only writes cand_band when it already equals shown_band. The comment directly above states the intended behaviour, which the code does not implement.

**Evidence.**
```c
    if (v->band == h->shown) {
        h->agree = 0;
        h->candidate = h->shown;
        /* The named frequency may still drift while the severity holds, so
         * it is only adopted once it too has settled. */
        if (v->strongest == h->shown_band) {
            h->cand_band = h->shown_band;
        }
        v->strongest = h->shown_band;
        return false;
    }
```

**Why it matters.** Reproduced deterministically on the host: seed the hold with {PERSISTENT, score 65, 19 kHz}, then apply 500 consecutive evaluations all saying {PERSISTENT, score 65, 20 kHz}. After 500 evaluations the displayed frequency is still '19 kHz'. On the glass this means: once the lens parks at PERSISTENT or BEACON LIKELY -- which is where it will sit for as long as the operator is looking at it -- the frequency on the detail line and the red row in the band table are frozen at whatever the FIRST evaluation guessed. That first evaluation runs after one 10 ms window with PAC_NOTE_SHORT set, before any band is eligible on recurrence, so `best` comes from the pass-1 fallback: the noisiest, least-settled reading in the whole session is the one that gets locked in. The operator is then told to act on a frequency the engine currently disagrees with, and 'move and retest' gives them no way to notice, because moving changes the severity only rarely. The existing test only checks the frequency across a severity CHANGE ('with the frequency it settled on'), which is the one path that does work.

**Fix.** Give the frequency its own counter. Add `uint8_t agree_band;` to pac_hold_t (pharos_acoustic.h, next to `agree`), cleared by pac_hold_reset's memset and in the priming branch. Replace the `v->band == h->shown` branch body with: `h->agree = 0; h->candidate = h->shown; bool moved = false; if (v->strongest == h->cand_band) { if (h->agree_band < 255u) h->agree_band++; } else { h->cand_band = v->strongest; h->agree_band = 1u; } if (h->cand_band != h->shown_band && h->agree_band >= PAC_CONFIRM) { h->shown_band = h->cand_band; h->agree_band = 0u; moved = true; } v->strongest = h->shown_band; return moved;` -- returning true so lens_whisper.c logs the change. Also set `h->agree_band = 0` wherever h->shown_band is assigned in the PAC_CONFIRM branch, so the two counters stay independent. Extend test_acoustic_verdict_is_held_still() with the case above: severity constant, strongest moving 19 kHz -> 20 kHz, must be adopted after exactly PAC_CONFIRM evaluations and not before.

### [high] blind-spot — Probe on a finer grid: a carrier between the 1 kHz-spaced probes loses two whole verdict bands
`components/pharos_engine/pharos_acoustic.c:11`

**Wrong.** The engine probes four discrete ultrasonic frequencies 1000 Hz apart. At n=480 and 48 kHz the Goertzel bin width is 100 Hz, so consecutive probes are 10 bins apart and there is no coverage in between. A carrier that does not sit on a probe centre suffers scalloping loss and, worse, splits almost equally into the two neighbouring probes -- which collapses the NARROW family, since c_narrow is computed as (best margin - second best margin). There is no windowing function applied before the Goertzel either, so the rectangular-window sidelobe skirt is the only thing covering the gap.

**Evidence.**
```c
static const uint32_t k_hz[PAC_BAND_COUNT] = {
    1000, 18000, 19000, 20000, 21000,
};
```

**Why it matters.** SilverPush -- the deployment this lens's own header is written about -- occupies roughly 18.7-19.5 kHz, and encodes each symbol as its own frequency inside that band. I swept a steady carrier across exactly that range on the host, amplitude 9000, present in 3 windows out of 4: 19.000 kHz gives BEACON LIKELY 76 (c_narrow=24), but 19.300 kHz gives TONE PRESENT 46 (c_narrow=2) and 19.400 kHz gives TONE PRESENT 49 (c_narrow=2). The same beacon, 300 Hz away, drops two verdict bands and loses the NARROW family entirely -- and the alert level falls from 3 ('act on this') to 1 ('worth knowing'), so it no longer interrupts anybody. 18.500 kHz gives TONE PRESENT 45 against 76 at 19.000. Worse, the actual SilverPush modulation defeats it outright: an FSK alphabet of 10 symbols spaced ~89 Hz across 18.7-19.5 kHz, 50 ms per symbol, reads TONE PRESENT 57 with c_narrow=6 and families=0x05 -- no NARROW family, never reaches BEACON LIKELY at any amplitude. Whether the lens fires is decided by where the advertiser happened to put the carrier, which is not a property of the threat.

**Fix.** Two changes, both integer and both cheap on a 240 MHz S3 (one Goertzel is ~480 MACs per 10 ms window). (a) Apply a window before the probe so the skirt is not load-bearing. Add a 480-entry Q15 Hann table (or derive it from the existing cos_q14[] by index) and multiply each sample as it is consumed inside goertzel_q's loop: `const int64_t xw = ((int64_t)x[k] * w[k]) >> 15;` -- this keeps the no-buffer property exactly, since the window is applied per sample on the fly, and drops the first sidelobe from -13 dB to -31 dB. (b) Make each named band the MAX over sub-probes instead of a single centre. Keep PAC_BAND_18K..PAC_BAND_21K as the display vocabulary, but compute each from four sub-probes at centre-375, centre-125, centre+125 and centre+375 Hz, taking the largest mag2 and remembering which sub-probe won. That gives 250 Hz worst-case offset instead of 500, removes the equal-split into two neighbours, and lets the detail row print the sub-probe's actual frequency ('18.9 kHz') instead of the band label. Sixteen Goertzels plus the audible reference is ~8200 MACs per 10 ms window -- under 1% of one core. Add a host test that sweeps 17.8-21.2 kHz in 100 Hz steps and asserts the verdict never varies by more than one band across the sweep.

### [high] blind-spot — Add a band-limited-energy family; spread-spectrum beacons are structurally rejected by NARROW
`components/pharos_engine/pharos_acoustic.c:291`

**Wrong.** The NARROW family is the lens's only discriminator between 'a deliberate signal' and 'a room', and it is defined purely as the gap between the strongest ultrasonic probe and the second strongest. Every beacon technology that spreads its energy across the band rather than concentrating it in one tone therefore scores c_narrow ~= 0 and is classified as the negative that NARROW exists to reject. The engine has no other way to tell a band-limited transmitter from broadband room noise -- there is no out-of-band reference probe and no comparison against the audible band.

**Evidence.**
```c
    /* --- NARROW: a tone, not noise ------------------------------------
     *
     * A beacon lives in one band. Broadband noise - a fan, a hiss, a hand
     * across a table - lifts every probe together, and lifting every probe
     * together is exactly what this must NOT call a beacon. Score the gap
     * between the strongest ultrasonic band and the next strongest. */
```

**Why it matters.** Google Nearby / Chromecast guest pairing transmits direct-sequence spread spectrum in 18.5-20 kHz at 94.5 b/s -- precisely a signal that lifts every probe together. I synthesised it on the host (BPSK on a 19.25 kHz carrier, 1500 chips/s, main lobe 18.5-20 kHz) and ran it against the engine at four amplitudes. At amp 12288 out of 32767 -- roughly -8 dBFS, a deafeningly loud ultrasonic transmission with the 18/19/20/21 kHz probes reading -39/-24/-27/-45 dB -- the verdict is TONE PRESENT 58, c_narrow=2, families=0x05. It never reaches BEACON LIKELY at any amplitude I tried, because the louder the beacon gets the more it lifts every probe, and the more the engine reads it as a room. The same applies to the 'sparse, stable comb of strong tones' shape that the ultrasniff project fingerprints, and to any chirp. A person standing in front of a Chromecast being handed an ultrasonic pairing token gets told 'An inaudible tone is here' at alert level 1, not 'Inaudible beacon' at alert level 3.

**Fix.** Add a fourth family PAC_FAM_SHELF (1u << 3) that recognises the shape NARROW cannot: energy confined to the ultrasonic band while the audible reference does not move. A room, a fan, a hiss or a hand on a desk raises 1 kHz too; a transmitter does not. Concretely: add two out-of-band guard probes at 16500 and 22500 Hz to k_hz (both inside Nyquist at 48 kHz) and track their peaks alongside the others. In pac_evaluate, compute `ultra_mean` = mean of peak[18K..21K] and `guard_max` = max(peak[GUARD_LO], peak[GUARD_HI]); if `ultra_mean >= guard_max + 12` AND the audible margin has not risen with it (audible_best did not improve by more than 6 over the history), set PAC_FAM_SHELF and score it 0..18 linearly over a shelf of 12..30 units. Widen the families bitmask loop from `b < 3` to `b < 4`, let SHELF substitute for NARROW in the two-family requirement, and keep the existing rule that BEACON still requires PAC_FAM_PERSISTENT so a burst of band-limited noise cannot reach the top on its own. The NEGATIVE this family must refuse to fire on is a room whose whole noise floor rises -- which is exactly what the guard probes at 16.5 and 22.5 kHz measure: if they rise with the band, it is the room, not a transmitter. Add host tests for both: the DSSS fixture above must reach PAC_BAND_BEACON, and a broadband-noise-level step across the whole spectrum must not set PAC_FAM_SHELF.

### [high] dishonesty — Refuse to name an ultrasonic frequency when the energy leaked in from outside the band
`components/pharos_engine/pharos_acoustic.c:154`

**Wrong.** The `present` test asks only whether a probe is 9 units above its own floor. It never asks whether the energy in that probe actually originated at that frequency. With a rectangular window and probes 10 bins apart, a loud tone anywhere in the audible top end leaks into the 18-21 kHz Goertzels far above the 9-unit threshold, and the engine then reports that leakage by name as an inaudible beacon. Nothing in the engine can detect this: there is no out-of-band guard probe, no mainlobe shape test, and PAC_NOTE_LOUD_ROOM cannot help because it keys on the 1 kHz reference (`e->audible_best <= 30u`), which a 15-17 kHz tone barely moves.

**Evidence.**
```c
        /* Present = meaningfully above this band's own floor. 9 dB is three
         * doublings of power; a room does not do that to a single narrow
         * band by accident. */
        const bool present = (e->floor[b] >= 9u) && (lvl + 9u <= e->floor[b]);
```

**Why it matters.** Reproduced on the host with a single pure tone at -8.7 dBFS and nothing else in the ultrasonic band. 15.0 kHz -> 'TONE PRESENT 50, 19 kHz'. 17.0 kHz -> 'TONE PRESENT 56, 19 kHz'. 17.05 kHz -> 'PERSISTENT 68, 18 kHz', with the 18 kHz probe reading -45 dB, 33 units above its floor. Even 6.3 kHz -> 'TONE PRESENT 50, 18 kHz'. In every case the 1 kHz reference sat at -63 to -72 dB, so PAC_NOTE_LOUD_ROOM never fires and the ceiling stays at 92. These are not hypothetical sources: a CRT or retro display flyback runs at 15.625/15.734 kHz, a Mosquito anti-loitering deterrent at ~17.4 kHz, and SoniControl's own config sets its cutoff at 16800 with the note 'lower limit for prontoly'. All of them are audible to some people, none of them is an inaudible cross-device tracking beacon, and the lens reports them under a frequency label that is wrong by one to eleven kilohertz. This breaks the house rule directly: positive evidence about the arriving signal is being invented from a probe that measured something else, and the operator is told to act on a number the device never measured.

**Fix.** Two parts, both receive-only. (a) The Hann window from Finding 3 drops the sidelobe skirt by ~18 dB and removes most of this. (b) Add a mainlobe test that can say 'this did not come from here'. With the sub-probes from Finding 3 already in place, a tone genuinely at the band centre gives a peak at the centre sub-probe with both outer sub-probes below it; leakage from outside gives a MONOTONE ramp across the four sub-probes toward whichever edge faces the true source. In pac_evaluate, for the winning band, if the sub-probe margins are monotone increasing toward the lowest or highest sub-probe AND the adjacent out-of-band guard (16500 or 22500 Hz, added in Finding 4) is at least as strong as the winning band, set a new note PAC_NOTE_OUT_OF_BAND (1u << 4) and clamp the ceiling to 30 -- below TONE PRESENT -- because the engine cannot say where the energy came from. Surface it in lens_whisper.c's `why` line as 'energy from outside 18-21 kHz'. Add the host test: pure tones at 6300, 12000, 15000, 16000, 17000, 17050 and 17500 Hz at -8.7 dBFS must all stay at or below PAC_BAND_TRACE and must all set PAC_NOTE_OUT_OF_BAND.

### [high] dishonesty — Stop printing 'Nothing above the room.' when the microphone is dead
`components/pharos_lens_whisper/lens_whisper.c:217`

**Wrong.** When PAC_NOTE_DEAF is set the engine does the honest thing -- ceiling 0, score 0 -- but score 0 maps to PAC_BAND_QUIET, and the lens then renders that band's name and hint unconditionally. The advice line is taken from pac_band_hint(PAC_BAND_QUIET), which is the string 'Nothing above the room.'. So the glass simultaneously shows 'MIC SILENT - cannot hear' on the detail line and 'Nothing above the room.' as the advice, under the headline word QUIET. Because ceiling is 0, the HUD's `d->ceiling > 0 && d->ceiling < 100` test fails and it prints a bare '0' with no ceiling tick, so nothing on screen marks the score as suppressed either.

**Evidence.**
```c
    if (v.notes & PAC_NOTE_DEAF) {
        snprintf(o->detail, sizeof(o->detail), "MIC SILENT - cannot hear");
    } else {
        snprintf(o->detail, sizeof(o->detail), "%s  %u%% of windows",
                 pac_probe_name(v.strongest), v.duty_pct);
    }
    snprintf(o->advice, sizeof(o->advice), "%s", pac_band_hint(v.band));
```

**Why it matters.** This is the exact failure the project's own host test declares to be the worst one a detector can make. test/host/test_acoustic.c:148-150 says: 'A dead microphone must say DEAF. "I heard nothing" and "I cannot hear" are different answers, and reporting the second as the first is the single most dishonest thing a detector can do.' The engine passes that test; the lens then undoes it at the only layer the operator actually reads. The QUIET hint 'Nothing above the room.' is an affirmative all-clear about a room the device cannot hear, and the word QUIET is what fills the band field. An operator glancing at the face -- which is the whole design premise of the face -- reads a clean bill of health from a broken microphone. Finding 1 makes this worse in practice, not better: QUIET is unreachable in a live room, so on this lens the word QUIET on screen effectively means the mic is dead, and it is the one situation where the lens says something reassuring.

**Fix.** Give 'cannot hear' its own vocabulary rather than borrowing QUIET's. In pharos_acoustic.h add PAC_BAND_DEAF to pac_verdict_band_t (placed before PAC_BAND_QUIET so the band_with_kerb `lo[]` table and the >= comparisons in k_whisper_display keep their ordering -- or add it as a separate flag if renumbering the enum is invasive), return 'CANNOT HEAR' from pac_band_name and 'Not a reading. Check the mic.' (28 chars, under the 34-char limit the vocabulary test enforces) from pac_band_hint, and set out->band = PAC_BAND_DEAF in pac_evaluate whenever PAC_NOTE_DEAF is set. Then in k_whisper_display set `o->has_score = false;` and `snprintf(o->big, sizeof(o->big), "--")` for that case, so the HUD shows a dash instead of a zero that reads as a measurement. Extend test_acoustic_deaf_is_not_quiet() to assert `v.band != PAC_BAND_QUIET` and that strstr(pac_band_hint(v.band), "Nothing") == NULL, and extend test_acoustic_vocabulary() to cover the new band.

### [high] ux — Carry a real raw_score through the acoustic verdict instead of feeding the HUD the capped one
`components/pharos_lens_whisper/lens_whisper.c:229`

**Wrong.** struct pharos_lens_display documents raw_score as 'what the evidence earned BEFORE the caps'. Whisper assigns it the post-cap score, so raw_score and score are identical by construction and the field carries no information. The underlying reason is that pac_verdict_t is the only engine verdict in this firmware with no raw_score member -- pharos_watch.h, pharos_rival.h, pharos_squall.h, pharos_flood.h, pharos_census.h, pharos_karma.h, pharos_sentinel.h, pharos_vigil.h, pharos_twin.h, pharos_harvest.h and pharos_aegis.h all have one, and their engines set it before applying caps. So the lens has nothing correct it could assign.

**Evidence.**
```c
    o->score = v.score;
    o->raw_score = v.score;
    o->ceiling = v.ceiling;
```

**Why it matters.** The ceilings are the honesty mechanism of this lens, and this is where that mechanism becomes invisible. lens_watch.c:546-547 shows the pattern the rest of the firmware uses -- a detail row printing '%u/%u' of raw_score against ceiling, toned WARN when `v.raw_score > v.score` -- and lens_aegis.c:160 does the same. Whisper's detail page has no such row, so an operator looking at a 21 kHz finding sees '62 / 62' and reasonably concludes the evidence is maxed out, when in fact the evidence earned 80 and PAC_NOTE_EDGE_OF_HEARING clipped 18 points off it precisely because the microphone path is unreliable up there. The same hiding happens for PAC_NOTE_SHORT (ceiling 45) and PAC_NOTE_LOUD_ROOM (ceiling 66). 'The score was held back, and here is by how much' is the most valuable thing this lens computes and it never reaches the screen.

**Fix.** Add `uint8_t raw_score;` to pac_verdict_t in pharos_acoustic.h next to `score` and `ceiling`. In pac_evaluate, immediately after `uint32_t score = (uint32_t)out->c_level + out->c_narrow + out->c_persist;` and before any of the family or ceiling clamps, set `out->raw_score = (uint8_t)((score > 100u) ? 100u : score);`. In lens_whisper.c change line 229 to `o->raw_score = v.raw_score;` and add a detail row after case 3 mirroring lens_watch.c: left 'capped at', right `snprintf(..., "%u/%u", v.raw_score, v.ceiling)`, tone `(v.raw_score > v.score) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM`. While adding that row, also surface the note the operator currently never sees -- extend the `why` chain in k_whisper_display with `else if (v.notes & PAC_NOTE_SHORT) snprintf(o->why, sizeof(o->why), "not enough windows yet");`, since PAC_NOTE_SHORT clamps the ceiling to 45 and is the only note with no on-screen explanation at all.

### [medium] ux — Feed the activity ribbon; whisper leaves it flat, which the HUD contract reads as 'nothing happened'
`components/pharos_lens_whisper/lens_whisper.c:231`

**Wrong.** k_whisper_display never touches o->history[] and never sets o->has_history, so pharos_hud_live takes the else branch and calls ribbon_set(i, 0, rgb) for all 16 slots. Whisper and footprint are the only two of the fifteen PHAROS_LENS_OBSERVE lenses that do this; census, karma, mirage, probe, rival, roster, harvest, spectrum, sentinel, vigil, ward, squall and watch all fill it.

**Evidence.**
```c
    o->score = v.score;
    o->raw_score = v.score;
    o->ceiling = v.ceiling;
    o->has_score = true;
```

**Why it matters.** pharos_lens.h states the contract for that array explicitly: 'All zero draws a quiet timeline, which is itself information.' By the HUD's own definition, whisper therefore paints a positive claim -- sixteen seconds of nothing -- on every frame, including frames where the band field says BEACON LIKELY. That is a statement the lens never made and the evidence does not support. It also costs the operator the one thing the ribbon is for and that matters most here: shape over time is how you tell a beacon that pulses on a duty cycle from a monitor whine that is simply always on, and it is the check the 320 ms `seen` history is far too short to make (32 windows at 10 ms covers a third of a second, whereas SoniControl's equivalent decision runs over a 1.5 s median on top of a 10 s background).

**Fix.** Keep a 16-slot one-second ring in the lens rather than the engine, so the engine stays a pure measurement. Add `static uint8_t s_ribbon[PHAROS_DISP_HISTORY]; static uint32_t s_ribbon_ms;` and in whisper_tick accumulate dt_ms; every time it crosses 1000, shift s_ribbon left by one and write `s_ribbon[15] = (uint8_t)((v.duty_pct * 255u) / 100u)` (duty_pct is already computed and already 0..100, so this needs no new engine state and no float). In k_whisper_display, memcpy s_ribbon into o->history and set o->has_history = true. If you would rather show intensity than recurrence, use the LEVEL sub-score instead: `(uint8_t)((v.c_level * 255u) / 30u)`.

### [medium] dishonesty — Say so when the capture rate puts the ultrasonic probes above Nyquist, instead of reporting QUIET
`components/pharos_engine/pharos_acoustic.c:65`

**Wrong.** goertzel_q correctly refuses to invent a result above Nyquist and returns 0, which to_dbfs turns into 120 (silence). But pac_evaluate has no idea this happened. It sees four ultrasonic probes reading 120, finds no margin, and returns score 0 / PAC_BAND_QUIET with the full ceiling of 92 and no note set -- a clean 'nothing above the room' produced by a receiver that is physically incapable of hearing above the audible band. The 1 kHz reference still works at any sane rate, so PAC_NOTE_DEAF does not fire either; the liveness check confirms the microphone is alive while the probes that matter are structurally dead.

**Evidence.**
```c
    const uint64_t pos_x256 = ((uint64_t)hz * 2u * 64u * 256u) / rate;
    if (pos_x256 >= 64u * 256u) {
        return 0; /* at or beyond Nyquist: this probe cannot mean anything */
    }
```

**Why it matters.** Reproduced on the host: 40 windows of room noise plus a healthy 1 kHz tone, observed at sample_rate 16000, returns 'QUIET, score 0, ceiling 92, notes 0x00, levels 1k=-78 18k=-120 19k=-120 20k=-120 21k=-120'. This is exactly the failure mode the house rules name -- a detector reading its own missing instrumentation as a finding -- and it is the same class of bug the lens has already been bitten by once: the comment at lens_whisper.c:77-91 records that esp_codec_dev_read's return code was misread as failure and 'The screen said "MIC SILENT - cannot hear" ... while the ES7210 was up, unmuted and delivering audio the whole time.' The exposure today is latent rather than live, because lens_whisper.c hardcodes WHISPER_RATE 48000 and bails if esp_codec_dev_open fails -- but nothing reads back the rate the codec actually negotiated, the comment at lens_whisper.c:52-54 shows the team already knows a wrong default here silently invalidates every band, and the engine ships as a standalone host-testable library that any future caller can hand a different rate.

**Fix.** Make the engine state it. Add `bool rate_ok;` to pac_engine_t and set it in pac_observe before the band loop: `e->rate_ok = (sample_rate > 2u * k_hz[PAC_BAND_COUNT - 1u]);` (42 kHz for the 21 kHz probe). Add `#define PAC_NOTE_NO_BAND (1u << 5)` to pharos_acoustic.h and, in pac_evaluate, `if (e->windows > 0u && !e->rate_ok) { out->notes |= PAC_NOTE_NO_BAND; ceiling = 0; }` alongside the PAC_NOTE_DEAF clause, so the verdict takes the same 'this is not a reading' path a dead microphone does. Surface it in k_whisper_display by extending the DEAF branch -- 'CAPTURE RATE TOO LOW' on the detail line, has_score false -- so it can never read as an all-clear. On the lens side, verify rather than assume: after esp_codec_dev_open succeeds, log the negotiated `fs.sample_rate` and refuse to start if it is not WHISPER_RATE, and add a host test that observes at 16000, 22050 and 32000 Hz and asserts PAC_NOTE_NO_BAND with ceiling 0 at each.
