# Pharos deep review - bugs, vulnerabilities, dead features, UX

Seven subsystems read line by line against the source, several findings
reproduced by compiling the engines on the host. Every finding was then put
to an independent agent told to REFUTE it: 52 verdicts, 45 survived, 7
refuted. This file is the raised set with full evidence.


## concurrency (pharos_bus.c / pharos_radio.c / pharos_bsp.c)

**Coverage.** Read in full: components/pharos_core/pharos_bus.c (118 lines), components/pharos_core/include/pharos_bus.h (62), components/pharos_radio/pharos_radio.c (786), components/pharos_bsp/pharos_bsp.c (822). Read as corroboration: components/pharos_core/pharos_lens.c (activate/deactivate ordering, ~105 lines), components/pharos_ui/pharos_ui.c (lines 40-120, 1250-1340, 1410-1470, 2020-2200 - the pump, the lens mutex, the UI loop), components/pharos_sense/pharos_sense.c (~90), main/main.c (95-160), main/console_glue.c (468-520, 720-780), lens_vigil.c/lens_roster.c/lens_rival.c/lens_watch.c start+camp paths, test/host/test_pharos.c test_bus() and test/host/Makefile, sdkconfig core-affinity lines, managed_components/waveshare__esp32_s3_touch_amoled_1_75c/esp32_s3_touch_amoled_1_75c.c (bsp_display_rotation_set, bsp_display_lock), managed_components/espressif__esp_lvgl_adapter/src/adapter/esp_lv_adapter.c (task + lvgl_mutex), and ~/esp/esp-idf/.../local/esp_wifi_types_native.h (wifi_pkt_rx_ctrl_t). Built and ran a 3-thread reproduction against the real pharos_bus.c with cc; probe source and binary at /private/tmp/claude-501/-Users-b0mba-at0mica-Documents-at0m-b0mb-Project/93a81b3f-4bd0-4db1-888f-3d5aa61a14b9/scratchpad/conc_bus_mp.c

### [high] vulnerability — The single-producer ingest ring has three concurrent producers; head can move backwards and the consumer then never sees "empty" again
`components/pharos_radio/pharos_radio.c:167`

**Wrong.** pharos_bus.h states the contract in its opening line: "One single-producer / single-consumer lock-free ring per ingest path", and pharos_bus.c's ordering argument opens "Single producer, single consumer, on two cores of an ESP32-S3 sharing coherent SRAM." pharos_bus_push() is written to that contract - it does a non-atomic read-modify-write of head with no lock:

    const uint32_t head = bus->head;
    const uint32_t tail = bus->tail;
    ...
    bus->slots[head & bus->mask] = *ev;
    PHAROS_RELEASE();
    bus->head = head + 1;
    bus->accepted++;

pharos_radio.c pushes onto that same bus from THREE different FreeRTOS tasks:
  - line 482, promisc_cb() -> `if (!pharos_bus_push(s.bus, &ev))` - Wi-Fi driver task, priority 23, core 0 (sdkconfig: CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y). Line 372 is a second push from the same task.
  - line 167, emit_dwell() -> `pharos_bus_push(s.bus, &ev);` - reached only from apply_channel() (line 494), which is reached only from hop_task() (line 511), created at line 577 as `xTaskCreatePinnedToCore(hop_task, "pharos_hop", 3072, NULL, 5, NULL, 0)` - priority 5, core 0.
  - line 683, ble_gap_event() -> `pharos_bus_push(s_ble_bus, &ev);` - NimBLE host task (CONFIG_BT_NIMBLE_PINNED_TO_CORE=0).

The Wi-Fi task at priority 23 preempts hop_task at priority 5 on the same core. If that preemption lands anywhere inside hop_task's push - and the window spans a 96-byte pharos_event_t struct copy (sizeof(pharos_event_t)=96, measured) between the load of bus->head and the store to bus->head - the Wi-Fi task pushes one or more frames and advances head, and then hop_task resumes and writes `bus->head = head + 1` using its stale head. head moves BACKWARDS.

Once head < tail, pharos_bus_pop()'s only emptiness test - `if (head == tail) return false;` - is false forever (for ~2^32 pops). The analytics pump then reads uninitialised/stale slots as real events, 256 per call, indefinitely.

Three lenses hand the SAME `&s_bus` to both radios, so they run all three producers at once: lens_vigil.c:83+86, lens_roster.c:52+53, lens_rival.c:141+144. Vigil is the worst case - it runs a full 13/14-channel survey plan at 250 ms dwell (so emit_dwell fires ~4x/second) AND the BLE observer.

**Evidence.**
```c
pharos_bus.c:45-60
    const uint32_t head = bus->head;
    const uint32_t tail = bus->tail;
    if ((uint32_t)(head - tail) > bus->mask) {
        bus->dropped++;
        return false;
    }
    bus->slots[head & bus->mask] = *ev;
    PHAROS_RELEASE();
    bus->head = head + 1;
    bus->accepted++;

pharos_bus.c:70   if (head == tail) { return false; }   /* the ONLY emptiness test */

pharos_radio.c:167   pharos_bus_push(s.bus, &ev);            /* emit_dwell, hop_task prio 5 */
pharos_radio.c:482   if (!pharos_bus_push(s.bus, &ev)) {     /* promisc_cb, wifi task prio 23 */
pharos_radio.c:683   pharos_bus_push(s_ble_bus, &ev);        /* ble_gap_event, NimBLE task */
pharos_radio.c:511   apply_channel(p->channels[s.plan_idx]); /* hop_task body */
pharos_radio.c:577   if (xTaskCreatePinnedToCore(hop_task, "pharos_hop", 3072, NULL, 5, NULL, 0) != pdPASS) {

lens_vigil.c:83   if (!pharos_radio_rx_start(&plan, &s_bus)) {
lens_vigil.c:86   if (!pharos_radio_ble_scan_start(&s_bus)) {
```

**Trigger.** On hardware: open the Vigil lens anywhere with ordinary Wi-Fi traffic. It runs the 250 ms-dwell survey plan plus the BLE observer against one bus, so the priority-5 hop task pushes a DWELL event ~4x/second into a ring the priority-23 Wi-Fi task is also pushing into.

Reproduced on the host against the real pharos_bus.c (probe at <scratchpad>/conc_bus_mp.c; build: cc -std=c11 -O1 -I components/pharos_core/include conc_bus_mp.c components/pharos_core/pharos_bus.c -o conc_bus_mp). Two producer threads, one consumer, 200000 pushes each. Three consecutive runs:
  offered=400000 accepted=149116 dropped=32248 acc+drop=181364 (must equal offered)
  head=173056 tail=39594467  head-tail=4255545885 (pending; >CAP means corrupt)
  head moved BACKWARDS 22472 times; head<tail seen 39424270 times
  ---
  offered=400000 accepted=836 dropped=256192 acc+drop=257028
  head moved BACKWARDS 35 times; head<tail seen 39997106 times
  ---
  offered=400000 accepted=14068 dropped=199074 acc+drop=213142
  head moved BACKWARDS 1555 times; head<tail seen 39922050 times

test/host/test_pharos.c test_bus() only ever drives the ring from one thread, so nothing in CI covers this.

**Impact.** Two distinct harms, both aimed squarely at the honesty rules.

1. Silent fabricated evidence. Once head rolls back below tail, pharos_ui_pump()'s `while (n < 256 && pharos_bus_pop(...))` hands the active lens up to 256 stale or never-written pharos_event_t per 2 ms tick, forever. Those are 96-byte structs read as real 802.11 frames and BLE reports: addresses, RSSI, subtypes, flags. A detector that manufactures frames it never heard is worse than a detector that misses things - the device would report deauth floods, tracker follows and evil twins that do not exist, and there is nothing on-screen that could distinguish this from a real attack.

2. The confidence ceiling is computed from corrupted counters. The probe shows accepted+dropped != offered - increments are lost to the same unsynchronised RMW. pharos_bus_yield_permil() feeds exactly the mechanism pharos_bus.h describes as "not a debug statistic - it is evidence": "every verdict carries the drop count and every engine is expected to widen its uncertainty when frames were lost". A yield figure derived from a lost-update race understates how much was dropped, so ceilings come out too HIGH - the unsafe direction.

**Fix.** Keep pharos_bus.c pure and single-producer; serialise the producers in pharos_radio.c, which is the ESP-IDF glue and the only place that knows there is more than one. Add at file scope:

    static portMUX_TYPE s_bus_mux = portMUX_INITIALIZER_UNLOCKED;

and bracket all three call sites (lines 167, 372, 482, 683):

    portENTER_CRITICAL_SAFE(&s_bus_mux);
    const bool ok = pharos_bus_push(s.bus, &ev);
    portEXIT_CRITICAL_SAFE(&s_bus_mux);

The critical section covers one 96-byte copy and two stores - well under a microsecond, affordable even in the Wi-Fi task's hard real-time budget, and it needs no extra RAM. Do not move the lock into pharos_bus.c: that would drag ESP-IDF into a pure-C11 component and break the host build.

Then make pop() defend itself rather than trusting head: replace `if (head == tail)` with `if ((uint32_t)(head - tail) == 0 || (uint32_t)(head - tail) > bus->mask + 1u) return false;` so a corrupted head can never be read as "billions of events pending" again - a detector must not read its own broken instrumentation as a finding.

Add a host regression test under test/host that runs two pthread producers against one bus and asserts accepted + dropped == offered and that head is monotonic. The existing test_bus() is single-threaded and cannot catch this.

### [high] bug — Frames are stamped with the hopper's current channel instead of the per-frame channel the driver reports, so frames around every retune are attributed to the wrong channel
`components/pharos_radio/pharos_radio.c:266`

**Wrong.** promisc_cb() labels each event with shared state owned by another task:

    ev.u.dot11.channel = s.channel;

`s.channel` is written by apply_channel() on hop_task (priority 5), and it is written BEFORE the hardware is actually retuned:

    s.channel = pharos_region_clamp_channel(channel);
    s.channel_since_us = now;
    esp_wifi_set_channel(s.channel, WIFI_SECOND_CHAN_NONE);

So there are two separate windows in which the label is wrong. First, between the `s.channel = ...` store and esp_wifi_set_channel() returning, the radio is still on the old channel while s.channel already says the new one. Second, the Wi-Fi driver buffers received frames and invokes the promiscuous callback from its own task, so frames captured on channel N can be delivered after s.channel has already become N+1.

The driver already hands the correct answer to the callback. wifi_pkt_rx_ctrl_t (esp-idf/components/esp_wifi/include/local/esp_wifi_types_native.h:60) declares `unsigned channel: 4;  /**< primary channel on which this packet is received */`, and promisc_cb already reads three other fields out of the same struct (rx_ctrl.rx_state at line 249, rx_ctrl.sig_len at 255, rx_ctrl.rssi at 265). rx_ctrl.channel is simply not used.

**Evidence.**
```c
pharos_radio.c:265-266
    ev.u.dot11.rssi = (int8_t)pkt->rx_ctrl.rssi;
    ev.u.dot11.channel = s.channel;

pharos_radio.c:488-497 (apply_channel)
    if (s.channel && s.channel_since_us) {
        const uint32_t visit_us = (uint32_t)(now - s.channel_since_us);
        account_dwell(s.channel, visit_us);
        emit_dwell(s.channel, visit_us);
    }
    s.channel = pharos_region_clamp_channel(channel);
    s.channel_since_us = now;
    esp_wifi_set_channel(s.channel, WIFI_SECOND_CHAN_NONE);

esp_wifi_types_native.h:60
    unsigned channel: 4;          /**< primary channel on which this packet is received */
```

**Trigger.** Any hopping plan - i.e. every lens except a camped one. pharos_scan_plan_survey() sets dwell_ms = 200, so hop_task retunes 5 times a second; Vigil sets plan.dwell_ms = 250. On each retune, the frames the Wi-Fi driver still has queued from the outgoing channel are delivered to promisc_cb after s.channel has already been advanced, and are stamped with the incoming channel. Observable from the console: run `spectrum`, sit next to a single AP that is known to be on channel 1, and watch a fraction of its beacons appear attributed to channel 2.

**Impact.** A frame that physically arrived on channel N is recorded as having arrived on channel N+1. This is inference from the scheduler's intent overriding positive evidence about the frame that actually arrived - the exact inversion the project's honesty rules forbid. Lenses that reason per channel act on it: Locate aims the operator at a channel, Spectrum draws the band picture, and Watch's camp-on-pressure logic (lens_watch.c:239 watch_camp_on(pressure)) can camp on a channel it was pushed toward by mislabelled frames, i.e. park the receiver on the wrong channel and then report what it does not hear there. The error rate scales with how busy the air is and with how fast the plan hops, so it is worst exactly when the detector matters most.

**Fix.** Use the driver's per-frame ground truth and fall back to s.channel only when the driver reports nothing usable:

    const uint8_t rx_chan = (uint8_t)pkt->rx_ctrl.channel;
    ev.u.dot11.channel = (rx_chan >= PHAROS_CHAN_MIN && rx_chan <= PHAROS_CHAN_MAX)
                             ? rx_chan : s.channel;

One bitfield read in the hot path, no new state, no lock. It also removes a cross-task read of s.channel from the callback. While there, move `s.channel = pharos_region_clamp_channel(channel);` to AFTER esp_wifi_set_channel() returns so the published channel never runs ahead of the hardware - that matters for pharos_radio_channel(), which the console and lens_watch.c:153 both read.

### [medium] bug — imu_probe() publishes s_imu_present before the self-test that decides it, and leaves the in-progress flag latched, so a concurrent caller is told the wrong answer - the bug the comment above it says was fixed
`components/pharos_bsp/pharos_bsp.c:539`

**Wrong.** The comment at lines 525-538 says the in-progress flag is what makes a concurrent caller safe: "a caller arriving in that window read s_imu_present as false and concluded there was no IMU. That is exactly what happened on hardware: the sampler task asked during the 200 ms configure, was told there was no sensor, and deleted itself." But the guard makes a second caller return from imu_probe() immediately, and pharos_bsp_imu_present() then returns whatever s_imu_present happens to hold at that instant:

    if (s_imu_probed || s_imu_probing) {
        return;
    }
    ...
    bool pharos_bsp_imu_present(void)
    {
        imu_probe();
        return s_imu_present;
    }

imu_probe() blocks for at least 100 ms of vTaskDelay plus I2C traffic, and for all of it s_imu_present is still false. So a concurrent caller is told "no IMU" on a board whose IMU is working - unchanged from the bug described. Worse, at line 600 s_imu_present is set true BEFORE the two checks that decide it, so a caller arriving after that line and before the gravity band check is told "IMU present" on a part that is about to be rejected.

Separately, two of the four exits never restore the flags. The gravity-fail path at line 620-628 returns having set only s_imu_present = false, and the success path falls off the end of the function at line 631. In both cases s_imu_probed stays false and s_imu_probing stays TRUE for the rest of the boot. The state machine only works by accident: every later call short-circuits on the latched s_imu_probing rather than on s_imu_probed, and the probe can never be retried.

**Evidence.**
```c
pharos_bsp.c:539-542
    if (s_imu_probed || s_imu_probing) {
        return;
    }
    s_imu_probing = true;

pharos_bsp.c:599-601
    int32_t x = 0, y = 0, z = 0;
    s_imu_present = true;
    if (!pharos_bsp_imu_read(&x, &y, &z)) {

pharos_bsp.c:620-628   (gravity-fail exit: no s_imu_probed, no s_imu_probing clear)
    if (mag2 < 700L * 700L || mag2 > 1400L * 1400L) {
        s_imu_present = false;
        ESP_LOGW(TAG, ...);
        return;
    }
    ESP_LOGI(TAG, "motion sensing live (%ld,%ld,%ld mg at rest)", (long)x, (long)y, (long)z);
}                              /* success exit: same omission */

pharos_bsp.c:634-638
    bool pharos_bsp_imu_present(void)
    {
        imu_probe();
        return s_imu_present;
    }

pharos_ui.c:2083 (the single, non-repeating consumer)
    pm_set_present(&s_motion, pharos_bsp_imu_present());
```

**Trigger.** Three tasks call pharos_bsp_imu_present(): the console REPL (console_glue.c:502, the `imu` command), the UI/main task (pharos_ui.c:2083), and the motion sampler (pharos_sense.c:36). main.c starts the console at line 104 and only then calls pharos_ui_run() at line 125.

Concrete sequence: with a serial terminal attached, send `imu` as soon as the console banner appears. The console task enters imu_probe() and sits in its two vTaskDelay(50 ms) calls; the main task reaches pharos_ui.c:2083, hits the `s_imu_probing` guard, returns immediately and reads s_imu_present == false.

The gravity-fail latch is reachable with no race at all: hold the board so it is accelerating (walking with it in hand) during the first second of boot so |a| leaves the 700..1400 mg band, and the probe exits at line 626 with s_imu_probing stuck true and s_imu_probed stuck false for the whole boot.

**Impact.** pm_set_present(&s_motion, ...) is called exactly once in the entire firmware (pharos_ui.c:2083) and nothing re-checks it, so a single false answer latches "no IMU" for the whole boot. The Home/watchtower motion reading then sits at UNKNOWN forever on a board whose accelerometer is working - and, per the project's own rule that "nothing is measuring" must never be reported as "nothing is moving", UNKNOWN is what every downstream question gets: pharos_ui_has_travelled() can never answer, so the "following" judgement that Vigil and Locate depend on is permanently unavailable with no on-screen explanation. The inverse window (line 600 publishing true before the self-test) is the more dangerous direction: it lets a caller adopt an IMU that the very next two checks are about to reject as mis-scaled, which is the railed-accelerometer failure the surrounding comment was written to prevent.

**Fix.** Make the published state match the probe's lifecycle, and make a concurrent caller WAIT rather than guess:

1. Delete `s_imu_present = true;` at line 600. Compute the verdict into a local and publish once at the end: `s_imu_present = ok;` immediately before setting s_imu_probed.
2. Give every exit the same three stores. Simplest structurally: make the body a static helper returning bool, and have imu_probe() be `s_imu_probing = true; const bool ok = imu_probe_body(); s_imu_present = ok; s_imu_probed = true; s_imu_probing = false;` so no return path can skip them.
3. In pharos_bsp_imu_present(), do not return mid-probe state. Spin briefly on the flag before reading the answer:

    bool pharos_bsp_imu_present(void)
    {
        imu_probe();
        for (int i = 0; i < 100 && s_imu_probing; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return s_imu_present;
    }

That is bounded at 1 s, matches the 1 s belt-and-braces delay pharos_sense.c already pays, and turns "asked at the wrong moment" into "waited" instead of into a permanent false negative. Declare s_imu_probed/s_imu_probing/s_imu_present volatile since they are now read across tasks by design.

### [medium] bug — pharos_bsp_rotate() drives the panel IO from the console task with no LVGL lock, against the contract this file states in its own header
`components/pharos_bsp/pharos_bsp.c:762`

**Wrong.** The file's opening comment states the rule without exception: "bsp_display_start() spins up the BSP's own LVGL task, so every widget call anywhere in Pharos must be bracketed by that lock." pharos_bsp_init() honours it for the draw buffers, with a comment saying why - "Under the LVGL lock, because the adapter's task is already running" - and takes bsp_display_lock(1000) at line 342.

Eight lines later, still inside pharos_bsp_init(), rotation_restore() is called with no lock held (line 352, after bsp_display_unlock() at line 344), and it calls pharos_bsp_rotate(), which calls bsp_display_rotation_set(). The vendor implementation (managed_components/waveshare__esp32_s3_touch_amoled_1_75c/esp32_s3_touch_amoled_1_75c.c:555-591) ends in:

    return esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &madctl, 1);

and takes no lock of its own. That is the same esp_lcd IO handle the LVGL flush path drives via esp_lcd_panel_draw_bitmap(). The adapter serialises its own drawing by running lv_timer_handler() under s_ctx.lvgl_mutex on its own task (managed_components/espressif__esp_lvgl_adapter/src/adapter/esp_lv_adapter.c:700, 751, 1665) - the mutex that bsp_display_lock() takes. This path bypasses it.

pharos_bsp_rotate() additionally does NVS flash work (nvs_open/nvs_set_i32/nvs_commit at lines 767-772) on the same unlocked path, which disables cache on both cores for the duration of the write.

**Evidence.**
```c
pharos_bsp.c:20-21 (the stated contract)
 * bsp_display_start() spins up the BSP's own LVGL task, so every widget call
 * anywhere in Pharos must be bracketed by that lock.

pharos_bsp.c:339-352
        /* Before anything is drawn: move the draw buffers into internal DMA
         * RAM so a flush never has to allocate. Under the LVGL lock, because
         * the adapter's task is already running. */
        if (bsp_display_lock(1000) == ESP_OK) {
            display_buffers_to_internal(disp);
            bsp_display_unlock();
        } else {
            ...
        }
        bsp_display_brightness_set(100);
        rotation_restore();

pharos_bsp.c:760-772
    if (bsp_display_rotation_set(r) != ESP_OK) {
        return false;
    }
    s_rotation = degrees;
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "rot", degrees);
        nvs_commit(h);
        nvs_close(h);
    }

esp32_s3_touch_amoled_1_75c.c:591
    return esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &madctl, 1);

console_glue.c:729 (the runtime caller, on the REPL task)
    if (!pharos_bsp_rotate(deg)) {
```

**Trigger.** With any lens running and the watchtower repainting (pharos_ui.c repaints every 100 ms under PHAROS_PAINT_LOCK_MS), type `rotate 90` on the USB console. The console REPL task issues an out-of-band MADCTL command on the panel IO handle while the adapter task may be inside a draw_bitmap for the current flush. The boot path hits the same window at pharos_bsp.c:352 whenever NVS holds a saved rotation, since esp_lv_adapter_start() has already been called by then.

**Impact.** Two tasks issue commands to the same CO5300 over one esp_lcd IO handle with nothing serialising them. A MADCTL parameter landing between a flush's address-window command and its pixel payload corrupts that frame; on a rotation restore at boot it lands while the splash is being painted. This is the same class of display fault the long comment at pharos_bsp.c:150-198 was written to eliminate, reintroduced through a path that was simply never brought under the lock. I have confirmed the two paths share the IO handle and that this one holds no lock; I have not confirmed on hardware whether esp_lcd's SPI layer happens to absorb the interleaving, which is why this is medium rather than high.

**Fix.** Bracket the panel touch in pharos_bsp_rotate() with the same lock the rest of the file uses, and keep the flash write outside it so NVS never stalls the LVGL task:

    bool pharos_bsp_rotate(int degrees)
    {
        ... switch on degrees ...
        if (!pharos_bsp_display_lock(1000)) {
            ESP_LOGW(TAG, "could not take the LVGL lock to rotate");
            return false;
        }
        const esp_err_t rc = bsp_display_rotation_set(r);
        pharos_bsp_display_unlock();
        if (rc != ESP_OK) {
            return false;
        }
        s_rotation = degrees;
        /* NVS outside the lock: a flash commit disables cache on both cores. */
        ... nvs_open/set/commit/close ...
    }

Note pharos_bsp_display_lock() is the wrapper that already gets the ESP_OK-vs-bool inversion right (line 628), so use it rather than bsp_display_lock() directly. rotation_restore() at line 352 then inherits the fix with no change.


## console — main/console_glue.c (the ESP-IDF glue and CLI), reviewed against its callee surfaces

**Coverage.** Read main/console_glue.c in full, all 977 lines, top to bottom. To judge what each command actually does I also read: components/pharos_engine/pharos_console.c (532 lines, the pure dispatcher the glue wires) and components/pharos_engine/include/pharos_console.h (full); components/pharos_engine/pharos_roster.c (rd_export/rd_expire/rd_observe_wps/admit, ~150 lines); components/pharos_lens_roster/lens_roster.c (locking discipline, ~150 lines); components/pharos_lens_watch/lens_watch.c and components/pharos_lens_harvest/lens_harvest.c (camp entry points); components/pharos_lens_squall/lens_squall.c (grep: no camp entry point exists); components/pharos_lens_rival/lens_rival.c (pharos_lens_rival_raw bounds); components/pharos_ui/pharos_ui.c (ring_at/ring_toggle/ring_cycle_period/ring_save, pharos_ui_tower_dump, battery poll and batt_mode, ~200 lines across); components/pharos_audio/pharos_audio.c (set_volume clamp); components/pharos_engine/pharos_tower.c (ptw_set_period, ptw_state_name); components/pharos_bsp/pharos_bsp.c + include (battery API); test/host/test_console.c (331 lines, to check existing coverage); build/pharos.map (real .bss placement of the console's static buffers). Four probes compiled and run on the host under /private/tmp/pharos_probe against the real engine sources: probe_squall.c (reproduces glue_set_channel's dispatch against the real pharos_console.c), probe_devices.c / probe_dev3.c / probe_dev4.c (real pharos_roster.c), probe_out.c (PC_OUT_CAP), probe_tower.c (verbatim tower_dump format strings).

### [high] dishonesty — `squall camp <ch>` reports "camped on channel N" and never retunes the radio
`main/console_glue.c:101`

**Wrong.** glue_set_channel() dispatches on the requested lens id with a two-branch strcmp chain that knows only wifi.watch and wifi.harvest. The command table advertises `squall [camp <ch>|survey]` (pharos_console.c:412) and cmd_squall forwards its arguments to run_scan(), which validates the channel, calls ops->set_channel(ch) and then prints "wifi.squall: camped on channel %d". glue_set_channel falls off the end of the if/else, so nothing happens — there is no pharos_lens_squall_camp() anywhere in the tree (grep over components/ finds only the snapshot accessor). The operator is told the receiver is parked on one channel while it keeps hopping 1..13. This is a positive claim about the radio's posture that is simply false, and it is exactly the case pharos_radio.c:113 names as the one that matters: "`squall camp 6`, the posture with the highest ceiling and the one an operator reaches for when they suspect jamming". `squall survey` is the same no-op, but benign, since surveying is already the default.

**Evidence.**
```c
    if (strcmp(id, "wifi.watch") == 0) {
        if (channel < 0) pharos_lens_watch_survey();
        else pharos_lens_watch_camp((uint8_t)channel);
    } else if (strcmp(id, "wifi.harvest") == 0) {
        if (channel < 0) pharos_lens_harvest_survey();
        else pharos_lens_harvest_camp((uint8_t)channel);
    }
}
```

**Trigger.** At the prompt: `squall camp 6`. Reproduced on the host — /private/tmp/pharos_probe/probe_squall.c links the real components/pharos_engine/pharos_console.c to an ops table that is glue_activate()/glue_set_channel() copied verbatim with the lens entry points replaced by counters:
  $ watch camp 6     -> console says: wifi.watch: camped on channel 6
     lens calls: watch=1 harvest=0 squall=0  <== radio was retuned
  $ harvest camp 6   -> console says: wifi.harvest: camped on channel 6
     lens calls: watch=0 harvest=1 squall=0  <== radio was retuned
  $ squall camp 6    -> console says: wifi.squall: camped on channel 6
     lens calls: watch=0 harvest=0 squall=0  <== NOTHING HAPPENED

**Impact.** The operator who suspects a channel is being jammed camps on it, is told they are camped, and then reads a retry/denial verdict computed from roughly a thirteenth of the dwell they think they have. Squall's own ceiling is honest — it reads pharos_radio_is_camped() at lens_squall.c:109 — so the verdict stays capped low, and the low score looks like "no jamming here" rather than "you never actually camped". A false negative on the one lens whose whole job is to distinguish busy from jammed.

**Fix.** Add the two entry points Squall lacks, modelled on pharos_lens_harvest_camp()/_survey() (lens_harvest.c:84-104): latch s_camp_requested/s_camp_channel so a deferred activation picks them up, and restart the radio only when Squall is already the active lens. Then add the third branch to glue_set_channel. Until those exist, the smaller correct fix is to make the miss loud rather than silent: give glue_set_channel a final `else` that reports the lens has no camp control, and have run_scan report failure rather than printing "camped on channel %d" unconditionally.

### [high] dishonesty — `devices` silently drops devices from the export while the header line claims the full count
`main/console_glue.c:524`

**Wrong.** cli_devices() exports into a fixed 3072-byte buffer, but prints the header count from pharos_lens_roster_count(), which counts the whole roster and not what was exported. rd_export() stops cleanly at the cap (pharos_roster.c:704: `if (n <= 0 || (unsigned)n >= cap - w) break;`) and returns only the bytes written — it has no way to say "there was more", and the caller never compares the row count to the device count. The footer then invites the operator to feed the list to a CVE database, which is exactly the use where a silently short list is worst.

**Evidence.**
```c
    static char buf[3072];
    const unsigned n = pharos_lens_roster_export(buf, sizeof(buf), !raw);
    ...
    printf("# vendor\tmodel\tclass\taddress\texposure  (%u devices%s)\n",
           pharos_lens_roster_count(), raw ? "" : ", redacted");
    fwrite(buf, 1, n, stdout);
```

**Trigger.** A full roster (PR_ROSTER_MAX is 64) in a dense block of flats, then `devices full`. Measured on the host with the real pharos_roster.c (/private/tmp/pharos_probe/probe_dev4.c — 64 APs, every other one naming itself in WPS as "Archer AX55 v1.0" / "RT-AX58U" / "EX6120" / "DIR-825 rev B1"):
  count=64  printed=60  would-be=64  LOST=4  (bytes 3200 vs cap 3072)
With full-length WPS model strings (the struct allows 24 characters) it is far worse — probe_devices.c, same 64 devices, redacted:
  rd_count() = 64 / rows actually printed = 51 / untruncated rows = 64
  LOST 13 device(s); header would still say "64 devices"
No host test covers this: test/host/test_roster.c never calls rd_export at a cap smaller than the roster.

**Impact.** The one command whose stated purpose is a complete offline inventory ("feed them to a CVE database") quietly returns an incomplete one, and the header asserts completeness. A device that never reached the ticket reads as a device that was not there. Redaction makes rows shorter, so the unredacted `devices full` — the mode used when the addresses matter — is the mode that truncates first.

**Fix.** Count the newlines in the returned bytes and print that number next to the roster count, and emit an explicit line when they differ, e.g. "# TRUNCATED: 60 of 64 devices printed - the rest did not fit". Better still, have rd_export take an unsigned *from index and let cli_devices loop, draining the roster a bufferful at a time, so the export is complete regardless of buffer size; the buffer can then also shrink (see the internal-DRAM finding).

### [high] bug — pharos_console_run() prints the output sink but never checks out.overflow, and the console's own `help` already overflows it
`main/console_glue.c:247`

**Wrong.** pc_out_t carries an explicit `overflow` flag precisely so a caller can tell that output was dropped — pc_print() sets it and refuses the whole string rather than writing a partial one (pharos_console.c:24-27). pharos_console_run() ignores the flag and prints whatever fitted, so an over-long result ends mid-list with no indication. The engine's own help text is already past the cap: it produces 1010 bytes and loses the entry for `stop`.

**Evidence.**
```c
    pc_exec(&ops, line, &out);
    if (out.len) {
        fputs(out.buf, stdout);
    }
    fflush(stdout);
```

**Trigger.** /private/tmp/pharos_probe/probe_out.c, linked against the real pharos_console.c:
  `help` output = 1010 bytes of PC_OUT_CAP 1024, overflow=YES - SILENTLY CUT
  last 60 chars: |fence is clean\n  region     set the regulatory channel plan\n|
The `stop` row — the last line of the [system] section, and the command an operator most needs when a lens is running — is gone. test/host/test_console.c:292-294 asserts help contains "receive-only", "watch" and "locate", all of which appear early, so the test passes over the truncation.

**Impact.** Today the CLI routes `help` to esp_console's own help command (console_glue.c:844 registers it and the table loop skips "help"), so this particular loss is not visible at the prompt — but the shared sink is one added command or one longer status line away from silently cutting any other output, and pharos_console_run() is the only place that could notice. A truncated status line is a detection tool dropping findings without saying so.

**Fix.** Two lines in pharos_console_run(): after fputs, `if (out.overflow) fputs("  [output truncated - the console sink is full]\n", stdout);`. Separately, either raise PC_OUT_CAP or have cmd_help stream per category, and add a host test that asserts every name in pc_table() appears in the `help` output — that assertion fails today.

### [medium] vulnerability — `devices` reads the roster from the REPL task without the mutex the roster file documents as mandatory
`main/console_glue.c:525`

**Wrong.** lens_roster.c takes s_lock on every other path into s_roster and says why in a block comment: rd_expire() on the UI task is a second writer against rd_observe_*() on the analytics core. pharos_lens_roster_count() (lens_roster.c:385) takes the lock. pharos_lens_roster_export() (lens_roster.c:378) does not — it calls rd_export(&s_roster, ...) bare. cli_devices runs on a third task, the console REPL, and walks the whole table while the analytics core is inserting into it. admit() also bumps r->n *before* it initialises the slot (pharos_roster.c:160-176: `r->n++;` ... `memset(d, 0, sizeof(*d)); memcpy(d->mac, mac, 6); d->in_use = true;`), so the reader's loop bound can cover a slot the writer is still building.

**Evidence.**
```c
main/console_glue.c:
    const unsigned n = pharos_lens_roster_export(buf, sizeof(buf), !raw);
components/pharos_lens_roster/lens_roster.c:
    unsigned pharos_lens_roster_export(char *buf, unsigned cap, bool redact)
    {
        return rd_export(&s_roster, buf, cap, redact);
    }

    unsigned pharos_lens_roster_count(void)
    {
        if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
            return 0;
        }
```

**Trigger.** `lens net.roster`, wait for a busy RF environment to fill the table, then type `devices` repeatedly while beacons are arriving. Two observable consequences, both from the same race. (1) An exported row can mix fields from the device that just vacated a reused slot with the one that replaced it — vendor/class from the old, MAC from the new. (2) The lock-free header count is worse: pharos_lens_roster_count() takes the mutex with a zero timeout and returns 0 on contention, so a `devices` issued while roster_event() holds the lock prints "# vendor model class address exposure  (0 devices, redacted)" immediately above a full listing.

**Impact.** Memory safety holds — vendor only ever points at string literals (rd_observe_wps stores only k_known[] entries) and model[] is always NUL-terminated by the memset — so the consequence is a wrong row rather than a crash. But a wrong row in the evidence export is an attribution error in a security finding, and the "(0 devices)" header is a visible contradiction against the rows underneath it.

**Fix.** Give pharos_lens_roster_export() the same shape as pharos_lens_roster_count(): take s_lock (a short timeout is fine — a miss should return 0 and let cli_devices say "roster busy, try again" rather than read torn). Return the device count through an out-parameter from the same critical section so the header and the rows come from one consistent view.

### [high] bug — The console holds 5152 bytes of scarce internal DRAM permanently for scratch buffers used a few times a session
`main/console_glue.c:245`

**Wrong.** Three static buffers in this file — the output sink, the roster export buffer and the tower dump buffer — land in internal .bss, the pool the project's own constraints call scarce and the pool the display flush and the Wi-Fi driver contend for. None carries EXT_RAM_BSS_ATTR, although the build has CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y and the codebase already uses the attribute for exactly this purpose (lens_rival.c:56: `EXT_RAM_BSS_ATTR static raw_adv_t s_raw[RIVAL_RAW_MAX];`). None of the three is touched by an ISR or by DMA; they are all filled by printf-family calls on the REPL task.

**Evidence.**
```c
main/console_glue.c:
    static pc_out_t out; /* static: keep it off the task stack (1 KB) */
    static char buf[3072];   /* cli_devices */
    static char buf[1024];   /* cli_tower */
build/pharos.map, the shipped link map — 0x3fcaXXXX is internal SRAM, PSRAM would be 0x3cXXXXXX:
 .bss.buf$1     0x3fcaa508      0xc00 esp-idf/main/libmain.a(console_glue.c.obj)
 .bss.buf$2     0x3fcab108      0x400 esp-idf/main/libmain.a(console_glue.c.obj)
 .bss.out$5     0x3fcab508      0x408 esp-idf/main/libmain.a(console_glue.c.obj)
                0x3fcab910       0x18 esp-idf/main/libmain.a(console_glue.c.obj)
```

**Trigger.** No input needed — it is unconditional at link time. 0xc00 + 0x400 + 0x408 + 0x18 = 5152 bytes of internal DRAM reserved from boot, on a device where `diag` exists specifically to report how little of that pool is left ("internal  : %u KB free (DMA-capable; the display flush and the wifi driver compete for this)").

**Impact.** Five kilobytes of the tightest resource on the board, held for the lifetime of the firmware so that three console commands can format text. On a build where the flush buffer and the Wi-Fi driver are already competing, this is the cheapest 5 KB available anywhere in the tree.

**Fix.** Mark all three EXT_RAM_BSS_ATTR, as lens_rival.c does. The pc_out_t sink is the only one worth a second thought — it is written by every command — but it is still only touched from the REPL task at human typing speed, so PSRAM latency is irrelevant. If the paging fix from the `devices` finding lands, buf$1 can also shrink from 3072 to a few hundred bytes.

### [high] bug — `ring` parses its index with atoi, so any non-numeric word silently means watch 0
`main/console_glue.c:550`

**Wrong.** atoi() returns 0 for text it cannot parse and cannot report failure, so the range check `i < 0 || (unsigned)i >= n` passes every typo through as index 0 — which is wifi.watch, the deauth watch the ring comment calls "the headliner". The period argument has the same flaw plus a second one: an out-of-range period is never rejected, it just spins the cycle loop to its limit. pharos_ui_ring_cycle_period() steps 1→2→3→4→1 (PTW_MAX_PERIOD is 4) and each step calls ring_save(), which does nvs_set_u8 ×2, nvs_set_u16, nvs_set_blob and nvs_commit.

**Evidence.**
```c
        const int i = atoi(argv[1]);
        if (i < 0 || (unsigned)i >= n) {
            printf("watch out of range (0-%u)\n", n ? n - 1u : 0u);
            return 1;
        }
        if (argc >= 3) {
            const int p = atoi(argv[2]);
            for (int k = 0; k < 8; k++) {
                uint8_t cur = 1;
                pharos_ui_ring_at((unsigned)i, NULL, NULL, &cur, NULL);
                if ((int)cur == p) {
                    break;
                }
                pharos_ui_ring_cycle_period((unsigned)i);
            }
        } else if (!pharos_ui_ring_toggle((unsigned)i)) {
```

**Trigger.** Type `ring off` — a natural guess for pausing the rotation, and the help hint "[reset | <n> [1-4]]" does not rule it out. atoi("off") is 0, argc is 2, so the deauth watch is disarmed and saved to NVS. The printed table does show it as "off", but nothing says a word was misread. Second trigger: `ring 3 9` (or `ring 3 every`) — p is out of range or 0, no cycle ever matches, so the loop runs its full 8 iterations, performing 8 NVS commits, ending back where it started (8 is two whole cycles of 4) and printing the unchanged table with no error.

**Impact.** A mistyped word switches off a detection watch and persists that to flash. An out-of-range period is accepted in silence and costs 8 flash commits per attempt — the one path in the console that writes NVS in a loop.

**Fix.** Parse with strtol and check the end pointer: `char *end; const long i = strtol(argv[1], &end, 10); if (end == argv[1] || *end) { printf("ring: '%s' is not a watch number\n", argv[1]); return 1; }`. Same for the period, and reject p outside 1..PTW_MAX_PERIOD before the loop instead of letting the loop discover it. Bound the loop by PTW_MAX_PERIOD rather than the magic 8, so it cannot write the same value twice.

### [high] bug — `alarm vol` casts to uint8_t before the clamp, so `alarm vol 300` sets 44%
`main/console_glue.c:370`

**Wrong.** The argument is run through atoi() and truncated to uint8_t at the call site. pharos_audio_set_volume() clamps above 100 (pharos_audio.c:258: `s_volume = (pct > 100) ? 100 : pct;`) but it only ever sees the low 8 bits, so values above 255 wrap before the clamp can see them. The help line one screen up promises a validated range: "alarm on | off | vol <0-100> | test". Nothing rejects anything.

**Evidence.**
```c
    if (strcmp(w, "vol") == 0) {
        if (argc < 3) { printf("vol <0-100>\n"); return 1; }
        pharos_audio_set_volume((uint8_t)atoi(argv[2]));
        printf("volume %u%%\n", (unsigned)pharos_audio_volume());
        return 0;
    }
```

**Trigger.** `alarm vol 300` → (uint8_t)300 is 44 → the alarm is set to 44%, not clamped to 100%. `alarm vol 256` → 0, the alarm is silenced while the operator believes they asked for maximum. `alarm vol loud` → atoi is 0 → silenced. `alarm vol -1` → (uint8_t)(-1) is 255 → clamped to 100, which is the one case that happens to come out sensibly.

**Impact.** Bounded by the fact that the command echoes the resulting volume, so an operator who reads the reply sees 44%. But the failure mode of the wrong direction — asking for more volume and getting an inaudible one — is an alarm that does not wake anybody, on a device whose alarm is the only output when the screen is off.

**Fix.** Parse with strtol into a long, reject a non-numeric tail and anything outside 0..100 with the same message the no-argument branch already prints, and only then cast. Four lines, and it makes the help text true.

### [medium] bug — `tower` can outgrow its 1024-byte buffer and lose the last watches with no notice
`main/console_glue.c:613`

**Wrong.** cli_tower dumps the whole 16-watch ring into a fixed 1024-byte buffer. pharos_ui_tower_dump() truncates safely at the end (it NUL-terminates either at k or at cap-1) but says nothing about having run out, and cli_tower cannot tell — the function returns void. The dump is also where a watch's latched finding appears ("[caught ALARM 137s ago]", up to 39 extra characters a line), so the output is longest exactly when there is something to read. Separately, the accumulator inside tower_dump is `k += (size_t)snprintf(...)`, which adds the would-be length, so after the first truncation `buf + k` is a pointer beyond one past the end of the array — it is only passed to snprintf with a size of 0 and never dereferenced, but it is undefined behaviour and it is one edit away from being a write.

**Evidence.**
```c
static int cli_tower(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char buf[1024];
    pharos_ui_tower_dump(buf, sizeof(buf));
    printf("%s", buf);
    return 0;
}
```

**Trigger.** Run the ring at its defaults (16 entries, 11 armed, 5 off) in an environment where several watches have latched a finding, then type `tower`. Modelled on the host with pharos_ui_tower_dump's format strings copied verbatim (/private/tmp/pharos_probe/probe_tower.c), with a 44-character headline and four latched watches:
  header bytes = 161 / full dump bytes = 1031 / cli_tower buffer = 1024
  watches that fit = 15 of 16 -> the tail is cut, silently
The cut falls on the last rows of the ring, which at the default ordering are the disarmed watches — but the ordering is the operator's to change with `ring`, and a longer headline or a fifth latched watch moves the cut earlier.

**Impact.** The command exists to answer "whose turn, and what each watch found". Losing the tail loses findings, and losing them silently means the operator reads a short list as a complete one. Marginal today (1031 against 1024) rather than routine, which is why the confidence is medium and not high.

**Fix.** Have pharos_ui_tower_dump return the number of bytes it wanted to write (it already computes it in k) and let cli_tower print "  [%u watches not shown - buffer full]" when that exceeds cap. Raise the buffer to 2048 and move it to PSRAM with EXT_RAM_BSS_ATTR at the same time. Inside tower_dump, clamp the accumulator after each snprintf (`if (k > cap) k = cap;`) so the out-of-range pointer is never formed.

### [medium] dead-feature — No console command reports battery charge or charging state, although the telemetry is live and the header lists it as a system command
`main/console_glue.c:295`

**Wrong.** pharos_console.h documents the SYSTEM category as "fence, region, battery, help", but there is no battery command in pc_table() and none registered in pharos_console_start(). cli_diag advertises itself as "board, display, radio and fence state" and prints display, touch, LVGL lock, HUD, alarm, internal heap, PSRAM and heap — every board fact except the one an operator holding an untethered device asks first. The data is already there and already honest: pharos_bsp_battery() returns soc_pct, mv, charging and present from the AXP2101, and pharos_bsp.c:394 explains that it refuses to fabricate a percentage when the PMU does not answer. The UI polls it every twelve seconds (pharos_ui.c:2198) and an on-screen mode already exists and persists to NVS (pharos_ui_batt_mode_next, pharos_ui.c:494), reachable only by walking to the System lens settings row (lens_system.c:498).

**Evidence.**
```c
pharos_console.h:
    PC_CAT_SYSTEM,     /* fence, region, battery, help     */
main/console_glue.c, the whole of cli_diag's board report — no battery line:
    printf("alarm     : %s\n", ...);
    printf("internal  : %u KB free (DMA-capable; the display flush and the "
           "wifi driver compete for this)\n", ...);
    printf("psram free: %u KB\n", (unsigned)(st.psram_free / 1024));
    printf("heap free : %u B (min %u)\n", ...);
```

**Trigger.** Type `diag` — the command whose registered help is "board, display, radio and fence state" — on a device running on battery. Nothing in the output says whether it is charging or how much charge is left, and no other command does either. The only route to the figure is to switch to the System lens and drive the detail page by hand: `lens sys.system`, `nav detail`, `tap <row>`.

**Impact.** An operator running the device untethered has no way to ask it how long it will keep watching, and no way from the console to turn the on-screen readout on — a setting the firmware already implements and already remembers. Not a correctness defect; a hole in the surface, and a small one to close.

**Fix.** Add three lines to cli_diag, guarded so an absent PMU says so rather than printing zeros, e.g. `pwr_battery_t b; if (pharos_bsp_battery(&b) && b.present) printf("battery   : %u%% %umV %s\n", b.soc_pct, b.mv, b.charging ? "charging" : "on battery"); else printf("battery   : no PMU answered - no reading (not 0%%)\n");`. Then register a `battery` command that prints the same line and takes `battery screen <auto|always|off>`, forwarding to pharos_ui_batt_mode_next()/pharos_ui_batt_mode() so the persisted on-screen readout is reachable without walking the touch UI.


## geometry (components/pharos_ui/pharos_dial.c, pharos_round.c, pharos_style.c)

**Coverage.** Read in full: components/pharos_ui/pharos_dial.c (487 lines), pharos_round.c (151), pharos_style.c (147), and their three headers pharos_dial.h (236), pharos_round.h (105), pharos_style.h (271) — the headers carry most of the design rationale so they were read before judging anything. Read callers to check the model against the renderer: pharos_hud.c lines 190-215 (set_text_fit_r), 800-900 (chip row + home_dot_deg + pharos_hud_home), 1180-1320 (ring paint and hit test); pharos_ui.c lines 480-660 (home map, label-width and capacity budget) and 1975-2070 (build_dial); pharos_engine/pharos_tower.c ptw_set_armed (to establish that n==1 is a reachable state); tools/render/pharos_render.c lines 150-175, 325, 555-580. Checked existing coverage in test/host/test_ring.c (320 lines, read in full), test_style.c lines 140-240, test_engines.c lines 520-600, and test/host/Makefile. Ran the full host suite: 8135 checks, 0 failures — every finding below is latent, none breaks the build. Wrote and compiled 8 probes under /private/tmp/pharos_geo (probe1..probe8.c) against the real sources with cc; all five findings are reproduced, none is speculative.

### [high] bug — The home ring's one-watch case models the label a quarter-turn from where it is drawn, so a single armed watch never gets its name
`components/pharos_ui/pharos_dial.c:213`

**Wrong.** ring_label_box() converts its angle with pr_polar's convention — t = (a - 90) then plain cos/sin, so a is degrees clockwise from 12 o'clock. The n>=2 branch was converted to that convention (the long comment above it describes exactly that fix). The n<2 branch was not: the literal -90.0f is a leftover from the old EAST-zero model, where -90 meant 'up'. Run through the new conversion it becomes t = -180 deg, i.e. (-r, 0) — nine o'clock. The renderer, home_dot_deg() in pharos_hud.c:816, returns 0.0f for n<=1 and pr_polar() puts the dot and the name at twelve o'clock. Same quantity, two conventions, one quarter-turn apart — the precise failure the comment block at lines 217-227 says was eliminated.

**Evidence.**
```c
    const float a = (n < 2u) ? -90.0f
                             : (225.0f + 270.0f * (float)i / (float)(n - 1u));
    /* pr_polar's convention, because pr_polar is what draws these.
...
    const float t = (a - 90.0f) * 3.14159265f / 180.0f;
    const float cx = rad * cosf(t);
    const float cy = rad * sinf(t);

/* and the renderer, pharos_hud.c:816 */
static float home_dot_deg(unsigned i, unsigned n)
{
    if (n <= 1u) return 0.0f;
    return 225.0f + 270.0f * (float)i / (float)(n - 1u);
}
```

**Trigger.** Arm exactly one watch and leave the rest disarmed (ptw_set_armed, pharos_tower.c:270-282, explicitly permits this — it only refuses to disarm the last one), then look at the home face. Reproduced on the host with /private/tmp/pharos_geo/probe8.c compiled against the real sources: pd_ring_layout(1,74,16,12,&r) gives r_even=142; pd_ring_label_fits(&r,0,1,74,16) returns false, because the modelled box at nine o'clock is x[-179,-105] y[-8,+8], which crosses the PD_HERO_HALF_W/PD_HERO_Y0/PD_HERO_Y1 keep-out. The same box evaluated at the angle the HUD actually draws — twelve o'clock, x[-37,+37] y[-150,-134] — clears the glass, the core and the headline. probe1.c prints the mismatch directly: n=1 i=0 renderer rel(0,-142) vs model rel(-142.0, 0.0).

**Impact.** pharos_hud.c:1262 gates every ring name on pd_ring_label_fits(), so with one armed watch the ring shows a coloured dot with no name at all. The operator sees a verdict colour and no way to tell which watch produced it without leaving the home face. The name is only rescued if that watch also happens to be the live lens with the tower running, which sets h->active and paints it in the centre band (pharos_hud.c, s_h_active); with the tower off, or on any other lens, there is nothing. It is a name-dropped-silently failure rather than text through the headline, but it is the same root cause the file's comments treat as the cardinal sin: a layout modelling different geometry from the renderer.

**Fix.** Make the n<2 branch produce the angle the renderer produces: `const float a = (n < 2u) ? 0.0f : (225.0f + ...)`. Better, delete the duplication — export home_dot_deg() (or move it into pharos_dial.c beside ring_label_box) and have both the HUD and ring_label_box call the one function, so the angle for a given (i, n) cannot be written twice again. Add a host test in test_ring.c that asserts, for every n from 1 to 16 and every i, that ring_label_box's centre equals pr_polar(pd_ring_label_r(&r,i), home_dot_deg(i,n)) to within a pixel — that assertion is model-independent and would have caught this.

### [high] bug — pd_ring_fits() omits the headline keep-out that ring_score() and pd_ring_label_fits() both apply, so pd_ring_capacity() over-reports
`components/pharos_ui/pharos_dial.c:275`

**Wrong.** There are three near-identical keep-out checkers in this file. pd_ring_label_fits (lines 254-263) and ring_score (lines 345-366) both reject a label box that overlaps the headline footprint. pd_ring_fits (lines 275-325) checks the safe radius, the PD_RING_CORE_R disc and the neighbour gaps — and stops. It has no headline clause. pd_ring_capacity is built entirely on pd_ring_fits and its comment claims that makes them agree; it does not, because the two functions are asking different questions.

**Evidence.**
```c
/* pd_ring_fits, lines 298-307 - the core disc, and then straight to the neighbour loop */
        {
            const float nx = (ax0 > 0.0f) ? ax0 : ((ax1 < 0.0f) ? -ax1 : 0.0f);
            const float ny = (ay0 > 0.0f) ? ay0 : ((ay1 < 0.0f) ? -ay1 : 0.0f);
            if (sqrtf(nx * nx + ny * ny) < (float)PD_RING_CORE_R) {
                return false;
            }
        }

/* ring_score, lines 360-365 - the clause pd_ring_fits is missing */
            if (ax0 < (float)PD_HERO_HALF_W && ax1 > -(float)PD_HERO_HALF_W &&
                ay0 < (float)PD_HERO_Y1 && ay1 > (float)PD_HERO_Y0) {
                return -1.0f;
            }

/* pd_ring_capacity, lines 394-408 */
    /* Defined in terms of pd_ring_fits, so the capacity and the checker can
     * never disagree about what fits. */
```

**Trigger.** Reproduced with /private/tmp/pharos_geo/probe7.c. pd_ring_layout(6, 74, 16, 12, &r) settles on r_even=140 and honestly reports capacity=5. At that radius, positions i=1 (279 deg, box x[-175.3,-101.3] y[-29.9,-13.9]) and i=4 (441 deg, box x[101.3,175.3] y[-29.9,-13.9]) both sit inside the PD_HERO_HALF_W=148 / PD_HERO_Y0=-44 / PD_HERO_Y1=6 box — pd_ring_label_fits says NO for both. pd_ring_fits(&r, 6, 74, 16, 12) nevertheless returns TRUE. n=7 behaves identically at 270 and 450 degrees. Consequently pd_ring_capacity(74,16,12) walks straight past n=6 and n=7 and returns 9, while pd_ring_layout's own answer for those counts is 5.

**Impact.** Two things. (1) The capacity number pharos_ui.c:581 budgets names against is larger than the arrangement actually supports, so at six or seven armed watches the UI switches on more labels than can be placed and the per-position gate silently drops two of them — dots with no name among dots that have them, which pharos_ui.c:552-553 itself calls out as reading like a fault rather than a decision. (2) More seriously for maintenance, test_ring.c:171-176 states the invariant as 'no two label boxes touching, none through the headline, none off the glass' and then asserts it with pd_ring_fits, which does not check the headline. The separate sweep at test_ring.c:217-258 that is supposed to catch exactly this uses step = 360/n and a = -90 + step*i with plain cos/sin — a full-circle, EAST-zero model that neither pd_ring_layout nor pharos_hud.c draws. So the ring's headline guarantee currently has no test behind it at all.

**Fix.** Factor the keep-out into one static helper — `static bool label_box_clear(float x0,float y0,float x1,float y1)` returning false for off-glass, inside PD_RING_CORE_R, or inside the hero box — and call it from all three of pd_ring_label_fits, pd_ring_fits and ring_score, so a fourth clause can never be added to two of them. Then fix test_ring_sweep_every_count to build its boxes from ring_label_box's own model (or from pr_polar(pd_ring_label_r(...), home_dot_deg(i,n))) instead of its private full-circle formula. Expect pd_ring_capacity(74,16,12) to drop from 9 to 5 once this lands; if that is too pessimistic, the answer is to widen the searched radius range in pd_ring_layout, not to keep the checker blind.

### [high] bug — pd_gauge_layout() silently discards a denied segment when arcs[] is already full, and reports capped=false
`components/pharos_ui/pharos_dial.c:140`

**Wrong.** The denied arc is emitted only `if (denied && out->n_arcs < PD_MAX_ARCS)`. When the array has just been filled by the allowed half of the same component, the denial is dropped — and with it both out->denied_points and out->capped, which are the two fields that tell the caller anything was removed. The allowed arcs, which are the flattering half, are always kept; the honesty half is what gets thrown away. pharos_dial.h:8-12 states the opposite contract: the gauge draws 'the points that were capped away, rendered as a denied arc rather than silently dropped'. Separately, out->denied_points is a uint8_t accumulated with no saturation.

**Evidence.**
```c
        if (denied && out->n_arcs < PD_MAX_ARCS) {
            pd_arc_t *a = &out->arcs[out->n_arcs++];
            a->start_deg = cursor;
            a->sweep_deg = total_sweep * (float)denied / 100.0f;
            a->value = (uint8_t)denied;
            a->denied = true;
            cursor = norm_deg(cursor + a->sweep_deg);
            out->denied_points = (uint8_t)(out->denied_points + denied);
            out->capped = true;
        }
```

**Trigger.** Reproduced with /private/tmp/pharos_geo/probe6.c: `const uint8_t comps[6] = {10,10,10,10,10,10}; pd_gauge_layout(comps, 6, 55, 100, 135.0f, 270.0f, &g);`. The evidence sums to 60 and the engine allowed 55, so five points were removed. Result: n_arcs=6, capped=false, denied_points=0 — the five points vanish. The fifth component splits into an allowed arc of 5 (filling slot 6) and a denied arc of 5 that is never written. Drop the score to 50 and the same evidence gives capped=true, denied_points=10, which is the behaviour the header promises. The uint8 wrap is reproduced by the same probe: comps {200,200,200} with score 0 gives denied_points=88 against a true total of 600.

**Impact.** A caller drawing this gauge would show a full 270-degree sweep with no denied segment and no cap indication for a verdict that was in fact reduced — the reading looks like uncapped evidence. That is precisely the class of thing the project's honesty rules exist to prevent: a cap becoming invisible. Mitigating and worth stating plainly: pd_gauge_layout has no production caller today (grep across main/, components/ and tools/ finds only pharos_dial.c, its header, and test_engines.c:551-566), so nothing currently ships this to glass. It is a false exported contract waiting for its first caller, not a live field bug. The uint8 wrap needs component values above 100, which no current engine produces.

**Fix.** Reserve the last slot for the denial rather than losing it: when the array is full and `denied` is non-zero, still set out->capped = true and saturate out->denied_points (`unsigned t = out->denied_points + denied; out->denied_points = t > 255u ? 255u : (uint8_t)t;`) outside the arc-emission branch, so the flags stay truthful even when the drawing cannot. Better still, compute both flags in a first pass over all n components before any arc is emitted — they are a property of the evidence, not of how many arcs happened to fit. Add the 10x6-at-55 case to test_engines.c.

### [high] bug — pd_label_capacity/pd_label_size use a 0.60 em advance where the corrected house estimate is 0.62 em, under-estimating text width by up to 11%
`components/pharos_ui/pharos_dial.c:10`

**Wrong.** PD_ADVANCE_NUM/DEN is 3/5 and the integer division `((int32_t)size * 3) / 5` truncates on top of that. pharos_style.h:99-105 documents that this exact number was corrected to 0.62 em *because* 0.6 em was the estimate that wrote SENTINEL through the middle of the face, and says in terms: 'It is deliberately a slight OVER-estimate: guessing narrow puts text through the edge of the glass... Round up.' The dial's copy of the same font metric was never rounded up, and the comment above it still claims it is 'deliberately pessimistic', which is the opposite of what it is.

**Evidence.**
```c
/* Mean advance width as a fraction of the em, measured for the shipped face.
 * Deliberately pessimistic: it is better to shorten a string that would have
 * fitted than to clip one that would not. */
#define PD_ADVANCE_NUM 3
#define PD_ADVANCE_DEN 5 /* 0.60 em */
...
    const int32_t advance = ((int32_t)size * PD_ADVANCE_NUM) / PD_ADVANCE_DEN;

/* against pharos_style.h:105 */
#define PS_EM_W(px) (((px) * 62 + 50) / 100)
```

**Trigger.** Reproduced with /private/tmp/pharos_geo/probe5.c. Per-step advance, dial vs house: 64px -> 38 vs 40, 34px -> 20 vs 21, 20px -> 12 vs 12, 14px -> 8 vs 9 (11.1% narrow). At the one live call site, tools/render/pharos_render.c:564, `pd_label_capacity(16, 152, PR_SAFE_R - 8)` returns 31 characters; it budgeted those at 9 px each (279 px, inside the 284 px chord it measured), but at the house width of 10 px the run is 310 px, half-extent 155, while the chord half at PR_SAFE_R for that band is 154 — one pixel past the safe radius at each end. Sweeping pd_label_size over dy -200..200 and 1..40 characters against PR_SAFE_R finds 97 (dy, n) pairs where the size it approves needs more pixels than the chord holds at the house estimate; the worst are at the 14 px step, e.g. dy=-190 n=26 needs 234 px against a 208 px chord.

**Impact.** Every fit decision made through pd_label_size/pd_label_capacity is optimistic by up to 11% at the 14 px step and 5% at 64 px, in the one direction pharos_style.h says must never be taken. Today the blast radius is small and should be stated as such: these two functions are used only by tools/render (the host mockup renderer) and by test_engines.c — the firmware's own text fitting goes through ps_capacity/PS_EM_W, which is correct. So the live consequence is that the mockups the team reviews are slightly more permissive than the glass, which is exactly backwards for a tool whose job is to catch overruns before flashing. test_engines.c:592-594 only checks pd_label_size against pd_label_capacity, i.e. the under-estimate against itself, so it cannot see this.

**Fix.** Define the dial's advance in terms of the one that was corrected — `#define PD_ADVANCE(size) PS_EM_W(size)` (pharos_dial.c already sits beside pharos_style.h in the same component) — or, if pharos_dial.c must stay independent of the style scale, change the fraction to 62/100 with the same +50 rounding and delete the 'deliberately pessimistic' comment, which is now false. Then add a host assertion that PD's advance at every PD_TYPE_SCALE step is >= PS_EM_W of that step, so the two estimates can never drift apart again.

### [high] bug — ps_chip_w() budgets the chord for a 16 px chip while the renderer draws 30 px chips, and the host test repeats the same expression so it cannot notice
`components/pharos_ui/pharos_style.c:121`

**Wrong.** The chord is measured at PS_Y_CHIPS + PS_CARD_GAP. PS_CARD_GAP is a spacing constant (8), not half a chip's height; using it here silently asserts the chip is 16 px tall. pharos_hud.c:841 draws the chip with mk_surface(p, cw, 30, dx, PS_Y_CHIPS, ...) — 30 px tall, so its far edge is at dy = 119, not 112. The comment immediately above this line is right about the principle ('the room they have is the chord there - not the screen width') and the code then measures the chord at the wrong row.

**Evidence.**
```c
    const int16_t half = pr_chord_halfwidth(PS_INNER_R, PS_Y_CHIPS + PS_CARD_GAP);
    const int16_t avail = (int16_t)(half * 2 - 8);
    const int16_t gaps = (int16_t)(PS_CARD_GAP * (int)(n - 1u));
    const int16_t w = (int16_t)((avail - gaps) / (int)n);

/* the renderer, pharos_hud.c:841 */
            s_l_chip[i]     = mk_surface(p, cw, 30, dx, PS_Y_CHIPS, C_TRACK,
                                         LV_OPA_COVER, 15);
```

**Trigger.** Reproduced with /private/tmp/pharos_geo/probe3.c. PS_INNER_R = 191. ps_chip_w(4) = 69, so the row of four is 69*4 + 8*3 = 300 px wide, half-extent 150. The chord half-width at the budgeted dy=112 is 154, which passes. At the real far edge of a 30 px chip, dy = 104 + 15 = 119, the chord half-width at PS_INNER_R is 149 — the outer chips' bottom corners sit 1 px outside the radius the budget was taken from. To see it get worse, raise the chip to 40 px in pharos_hud.c:841: the far edge moves to 124, the chord half-width falls to 145, and the row overruns by 5 px per side with nothing reporting it.

**Impact.** Today the visible damage is nil and I want to be honest about that: PS_INNER_R already holds an 8 px margin inside the gauge track (PS_RING_R - PS_RING_W/2 - 8 = 191 against a track inner edge of 199), so a 1 px overrun eats margin rather than drawing on the arc, and the chord at PR_SAFE_R for that row is 189 so nothing approaches the glass. The finding is the latent trap: the layout is anchored to a constant that has no relationship to the thing it is standing in for, and test_style.c:207 re-derives the budget with the identical expression `pr_chord_halfwidth(PS_INNER_R, PS_Y_CHIPS + PS_CARD_GAP)`, which makes the assertion a tautology against ps_chip_w's own implementation. Any future change to the chip height — a taller chip to hold two lines, a larger label step — moves the row under the gauge and no test will say so.

**Fix.** Give the chip a named height next to PS_CARD_H in pharos_style.h (`#define PS_CHIP_H 30`), use `pr_chord_halfwidth(PS_INNER_R, PS_Y_CHIPS + PS_CHIP_H / 2)` in ps_chip_w, and have pharos_hud.c:841 pass PS_CHIP_H instead of the literal 30. Then rewrite the test_style.c assertion so it does not restate the formula: assert that the row's four corners, computed from ps_chip_w(4) and PS_CHIP_H, all satisfy pr_radius_of(...) <= PS_INNER_R. That form survives a change to either constant.

### [high] bug — ps_fit() and ps_capacity() disagree by one character, and test_style.c's dy grid steps straight over every case
`components/pharos_ui/pharos_style.c:34`

**Wrong.** ps_fit decides with pr_text_fits, which asks `(text_w / 2) <= half` — integer division that discards the odd half-pixel of a run whose width is odd. ps_capacity decides with `(half * 2) / cw`. For an odd text_w the first is one character more permissive than the second, so ps_fit can return a step whose ps_capacity is smaller than the character count it was asked about. test_style.c:180-186 asserts precisely that this cannot happen, but sweeps dy in steps of 20, and none of the failing offsets is a multiple of 20.

**Evidence.**
```c
/* pharos_round.c:86, what ps_fit decides on */
    return (text_w / 2) <= half;

/* pharos_style.c:60, what the caller then truncates with */
    return (unsigned)((half * 2) / cw);

/* test_style.c:180-186, the invariant and the grid that misses it */
        for (unsigned chars = 1; chars <= 24; chars++) {
            for (int16_t dy = -180; dy <= 180; dy = (int16_t)(dy + 20)) {
                const ps_type_t t = ps_fit(chars, dy, PR_SAFE_R);
                if (t < PS_TYPE_N) {
                    CHECK(ps_capacity(t, dy, PR_SAFE_R) >= chars,
```

**Trigger.** Reproduced with /private/tmp/pharos_geo/probe4.c, which runs the test's own invariant first on the test's grid and then at every integer dy in the same range. On the sampled grid: 0 disagreements — the suite passes. At every integer dy in that same range, same chars 1..24, same PR_SAFE_R: 12 disagreements. The first is dy=-170, chars=15: ps_fit returns PS_TYPE_TITLE (28 px, PS_EM_W=17, run 255 px, far edge dy=-184, chord half 127, and 255/2 = 127 passes by truncation) while ps_capacity(PS_TYPE_TITLE, -170, PR_SAFE_R) returns 14. Widening to all three working radii and chars 1..60 gives 194 such pairs.

**Impact.** The real-world consequence is close to zero and I would rather say so than inflate it: the disagreement is half a pixel, it sits inside the deliberate over-estimate PS_EM_W already carries, ps_capacity is the conservative one, and ps_capacity is the function the firmware actually uses (pharos_hud.c:205 set_text_fit_r). ps_fit itself has no production caller — grep across main/, components/ and tools/ finds it only in pharos_style.c, its header and test_style.c. What is worth an engineer's time is the second half: a documented contract ('the function may not disagree with the capacity function it is built on') is false, and the test that guards it samples every twentieth row, which is the same shape of gap that let the earlier layout bugs through. A reader who later wires ps_fit into a renderer inherits a one-character optimism.

**Fix.** Make ps_fit ask ps_capacity rather than pr_text_fits — `for (int t = PS_TYPE_METRIC; t >= PS_TYPE_MICRO; t--) if (ps_capacity((ps_type_t)t, dy, r) >= chars) return (ps_type_t)t;` — so there is one definition of 'fits' and the invariant becomes true by construction. Change the test's inner loop to `dy++` so the grid stops hiding half-pixel cases. While there, note that ps_text_w returns int16_t and PS_EM_W(48)*chars overflows above 1092 characters: ps_fit(1100, 0, PR_SAFE_R) returns PS_TYPE_HERO instead of PS_TYPE_N (probe3.c). No caller passes anything near that today — row fields are 26 and 12 bytes — but returning int32_t from ps_text_w, or clamping chars, costs nothing and removes a nonsense answer from a refusal function.


## hud

**Coverage.** Read both assigned files in full at their current state: components/pharos_ui/pharos_hud.c (1875 lines, all of it, in six sed passes) and components/pharos_ui/include/pharos_hud.h (312 lines). NOTE: both files were modified by a concurrent agent at 22:08 while I was reading (the battery-mode feature was added mid-review); every line number and quote below was re-verified against the post-edit file, but they may shift again. Also read for context: components/pharos_ui/include/pharos_style.h (bands/capacity), components/pharos_ui/pharos_style.c, components/pharos_ui/pharos_round.c, components/pharos_core/include/pharos_lens.h (pharos_lens_display, ~100 lines), components/pharos_ui/pharos_ui.c (the HUD's only caller: paint loop ~1912-2210, home fill ~535-700, detail calls ~1791/1834, theme_sync ~1808-1826, batt_apply ~502), main/console_glue.c cli_screen (~750-795), and the vendored LVGL v9 in managed_components/lvgl__lvgl/src/core/lv_obj_style.c, lv_obj_style_gen.c, lv_obj_pos.c and src/misc/lv_style.c to establish exactly which setters compare before invalidating. Built and ran two host probes under /private/tmp/pharos_probe linking the real pharos_style.c and pharos_round.c: probe.c reproduces pharos_hud_live()'s advice split verbatim against real engine advice strings, p2.c checks the new PS_Y_BATT readout against the safe radius (it fits: capacity 8 chars, "100% CHG" is exactly 8 - not a finding). Checked test/host for existing coverage: there is no host test of pharos_hud.c (it is entirely inside #ifdef ESP_PLATFORM), so none of these are covered.

### [high] bug — s_l_action2 is never created, so the second half of every LIVE advice line is silently dropped
`components/pharos_ui/pharos_hud.c:1503`

**Wrong.** pharos_hud_live() splits d->advice onto two lines and writes the tail to s_l_action2. s_l_action2 is declared at line 122 and is written at lines 1421 and 1503, but it is never assigned an object anywhere in the file - `grep -n 's_l_action2' pharos_hud.c` returns exactly three hits, none of them an assignment, and the LIVE build block creates s_l_why and s_l_action but stops there. It is therefore permanently NULL, and both set_text() and set_text_fit_r() open with `if (!o) return;`, so the write is a silent no-op. The first line is written with plain set_text(), and split_two() only ever ellipsises the TAIL buffer, never the head - so the head is emitted with no cut marker at all. The result is a fragment that looks like a finished sentence.

**Evidence.**
```c
static lv_obj_t *s_l_why, *s_l_action, *s_l_action2;
...
        s_l_why    = mk_label(p, PS_TYPE_LABEL, C_DIM,    0, PS_Y_ACTION - 26, "");
        s_l_action = mk_label(p, PS_TYPE_LABEL, C_ACCENT, 0, PS_Y_ACTION + 4, "");
...
        /* Two lines, split on a word. See the note on PS_Y_ACTION2: cutting
         * "Broad, spoofed deauth. Preserve the log." down to its first
         * sentence throws away the only part that says what to DO. */
        char a1[48], a2[48];
        const unsigned cap = ps_capacity(PS_TYPE_LABEL, PS_Y_ACTION, PR_SAFE_R);
        split_two(d->advice, cap < 47u ? cap : 47u, a1, sizeof a1, a2, sizeof a2);
        set_text(s_l_action, a1);
        set_text_fit(s_l_action2, PS_TYPE_LABEL, PS_Y_ACTION2, a2);
```

**Trigger.** Reproduced on the host. Probe at /private/tmp/pharos_probe/probe.c copies split_two() verbatim and links the real pharos_style.c/pharos_round.c; build with `cc -std=c11 -I components/pharos_ui/include -I components/pharos_core/include -o probe probe.c components/pharos_ui/pharos_style.c components/pharos_ui/pharos_round.c`. ps_capacity(PS_TYPE_LABEL, PS_Y_ACTION=170, PR_SAFE_R=224) = 27. Feeding it the real strings from pw_band_advice() (components/pharos_engine/pharos_watch.c:922):
  PW_BAND_LIKELY  "Sustained disconnects that do not add up. Preserve the log."
    -> glass shows: "Sustained disconnects that"      (dropped: "do not add up. Preserve the log.")
  PW_BAND_SUSPICIOUS "The shape looks wrong but nothing proves a forgery yet."
    -> glass shows: "The shape looks wrong but"       (dropped: "nothing proves a forgery yet.")
  PW_BAND_QUIET   "No disconnect traffic in view. This receiver hears one channel at a time."
    -> glass shows: "No disconnect traffic in"        (dropped: "This receiver hears one channel at a time.")
On hardware: run the Watch lens until it reaches SUSPICIOUS or LIKELY and read the bottom of the LIVE face.

**Impact.** Three separate harms, and the third is an honesty-rule breach. (1) The action is lost: PW_BAND_LIKELY renders as "Sustained disconnects that" and "Preserve the log." - the only instruction on the screen - never appears, which is precisely the failure PS_Y_ACTION2's own comment was added to prevent. (2) The cut is invisible: no ellipsis, no marker, so the operator has no signal that anything is missing. (3) The dropped half is systematically the QUALIFIER. "The shape looks wrong but" is left dangling without "nothing proves a forgery yet", and the QUIET advice loses "This receiver hears one channel at a time" - the dwell caveat that stops an absence-based reading being read as proof of absence. The truncation preferentially strips exactly the text that keeps the device honest.

**Fix.** Create the widget in the LIVE build block next to s_l_action, at the band it is written against: `s_l_action2 = mk_label(p, PS_TYPE_LABEL, C_ACCENT, 0, PS_Y_ACTION2, "");`. While there, note that s_l_why is built at `PS_Y_ACTION - 26` (=144) but fitted at PS_Y_WHY (=140) - use PS_Y_WHY in both places so the band and its capacity cannot drift. Add a host test under test/host that exercises split_two() against every engine's band-advice table and asserts that a1+a2 reconstructs the input, so a dropped line cannot ship silently again.

### [high] dead-feature — The `screen colour` channel-order diagnostic is erased by the next paint within 100 ms
`components/pharos_ui/pharos_hud.c:1832`

**Wrong.** pharos_hud_colourbars() is implemented as a bare page_show(PAGE_BARS), and the HUD has no concept of a page that holds. Every other entry point - pharos_hud_home(), pharos_hud_live(), pharos_hud_detail(), pharos_hud_guide() - begins with its own unconditional page_show(), so whichever one the UI loop calls next takes the screen back. The UI loop calls paint() every 100 ms (pharos_ui.c: `since_paint += dt_ms; if (since_paint >= 100) { since_paint = 0; paint(active); }`) and paint() dispatches on s_view, which the console command never touches - nothing in pharos_ui.c mentions the bars at all. So the patches are on the glass for at most one 100 ms frame.

**Evidence.**
```c
void pharos_hud_colourbars(void)
{
    if (!s_built) return;
    page_show(PAGE_BARS);
}

and in every other entry point, e.g.:

void pharos_hud_home(const struct pharos_hud_home *h)
{
    if (!s_built || !h) return;
    page_show(PAGE_HOME);
```

**Trigger.** Boot the device on any lens, open the console and run `screen colour`. The console prints "colour bars pushed... look at the panel" and "Run 'screen test' to go back", implying the page persists; the panel shows the patches for under a tenth of a second and then returns to HOME or LIVE. The only case it survives is VIEW_BROWSE, where paint() returns without painting.

**Impact.** The one diagnostic that can distinguish a wrong panel channel order from a correct one is unusable - and the header's own justification for it is that a photograph of a normal screen cannot tell you what is wrong, so there is no fallback. An operator who runs it sees a flash and reports "nothing happened", and the engineer on the other end of that report has no reading. It also makes the console text dishonest: it tells the user to go back with `screen test`, from a page they were never on.

**Fix.** Give the HUD a sticky page. Either add `void pharos_hud_hold(bool on)` that makes page_show() a no-op while set (cleared by a nav press or by `screen test`), or have cli_screen set a VIEW_BARS in pharos_ui so paint() returns early the way VIEW_BROWSE does. A hold flag inside the HUD is the smaller change and keeps the page-ownership rule in one file.

### [high] dishonesty — The colour-bar crib sheet in the console disagrees with the labels drawn on the glass
`main/console_glue.c:767`

**Wrong.** pharos_hud.c draws six patches in the order RED, GREEN, BLUE, WHITE, AMBER, BLACK. cli_screen() tells the operator to expect RED, GREEN, BLUE, YELLOW, WHITE, BLACK. Positions 4 and 5 disagree in both name and order: the glass says WHITE then AMBER (0xFFFFFF then 0xFFC34A), the console says YELLOW then WHITE. The whole point of the feature, per the header, is that the operator reports which label showed which colour; the two halves of that contract do not match.

**Evidence.**
```c
components/pharos_ui/pharos_hud.c:
        static const uint32_t rgb[6] = { 0xFF0000, 0x00FF00, 0x0000FF,
                                         0xFFFFFF, 0xFFC34A, 0x000000 };
        static const char *nm[6] = { "RED", "GREEN", "BLUE", "WHITE", "AMBER", "BLACK" };

main/console_glue.c:767:
        printf("  RED  GREEN  BLUE  YELLOW  WHITE  BLACK\n");
```

**Trigger.** Run `screen colour` on the console (and fix the previous finding so the page actually stays up). The console says patch 4 is YELLOW and patch 5 is WHITE; the panel labels patch 4 WHITE and patch 5 AMBER. An operator comparing the two reports a channel swap on a panel whose channels are correct.

**Impact.** Inverts the diagnostic's answer for two of the six patches. A correct panel reads as broken, and a genuinely swapped panel could read as correct, because the discrepancy the operator is being asked to look for is manufactured by the tool itself. The header's claim - "It turns a guess into a reading" - does not hold.

**Fix.** Make the console echo the HUD's own table rather than a second copy of it: export the names (e.g. `const char *const *pharos_hud_bar_names(unsigned *n)`) and print them in a loop, so the two can never drift. If that is more plumbing than it is worth, at minimum correct line 767 to "  RED  GREEN  BLUE  WHITE  AMBER  BLACK" and add a comment tying it to nm[] in pharos_hud.c.

### [high] bug — pharos_hud_rebuild() resets only two of the five dirty-check caches, so a theme change blanks the battery, the ribbon and the guide illustration
`components/pharos_ui/pharos_hud.c:1131`

**Wrong.** pharos_hud_rebuild() deletes every widget with lv_obj_clean() and builds fresh ones, all of them hidden or at their build-time defaults. It then resets s_aura_rgb and s_zones_detail so their dirty checks will re-fire - but leaves three other caches holding values that describe the widgets that no longer exist: s_batt_last (int, the packed pct+charging key), s_ribbon_level[PHAROS_DISP_HISTORY] (per-bar level), and s_g_step (the guide step on screen). Each of those guards an early return, so the corresponding widgets are never re-driven and stay in their freshly-built, blank state until the underlying value happens to change.

**Evidence.**
```c
void pharos_hud_rebuild(void)
{
    if (!s_built) return;
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;
    const hud_page_t was = s_current;
    lv_obj_clean(scr);
    s_built = false;
    s_aura_rgb = 0xFFFFFFFFu;
    s_zones_detail = -1;
    if (pharos_hud_create() && was < PAGE_N) {
        page_show(was);
    }
}

and the three guards it does not clear:

    const int key = (int)pct + (charging ? 1000 : 0);
    if (key == s_batt_last) return;
...
    if (s_ribbon_level[i] == level) return;
...
    if ((int)g->step == s_g_step) return;
```

**Trigger.** On the device: open Settings inside any lens, change the theme (pharos_ui.c theme_sync() calls pharos_hud_rebuild()). Then (a) the charge arc and the new percentage readout are hidden and stay hidden - batt_apply() is called every ~12 s with the same soc_pct and charging flag, so `key == s_batt_last` and pharos_hud_battery() returns before the show() calls - until the pack's SoC actually moves, which on this hardware is minutes. With PHAROS_BATT_ALWAYS selected the operator has explicitly asked for a permanent readout and it silently disappears. (b) Return to a LIVE lens: every ribbon bar whose history[] value is unchanged keeps the 7x6 C_TRACK stub mk_surface() built it as, so a steady ribbon reads as a dead timeline. (c) Exit the first-run guide while it is on step 0, change the theme, then replay the guide from Settings: g->step is 0 and s_g_step is still 0, so the function returns before g_hide_all() and before the switch, and the step's ghost outline and fingertip animation never appear.

**Impact.** A theme change - an ordinary, encouraged action - silently disables three unrelated pieces of the instrument. The battery one is the worst because it is the feature the operator turned on precisely so it would always be there, and a charge readout that is absent is indistinguishable from a charge readout that is fine. The ribbon one produces a flat timeline, which the ribbon's own comment says must never happen because "a hole in the ribbon reads as a rendering fault".

**Fix.** Reset every cache that describes widget state in the same block, next to the two that are already there: `s_batt_last = -2; s_g_step = -1; for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) s_ribbon_level[i] = 0xFF;` (0xFF is the sentinel pharos_hud_create() already uses). Better still, move all five into a small `static void hud_forget_state(void)` called from both pharos_hud_rebuild() and pharos_hud_create(), so the next cache somebody adds cannot be forgotten - this is the second time the list has been incomplete.

### [high] bug — HOME writes unguarded lv_obj_align()/opacity styles for every dot on every frame, so the steady-state repaint invalidates continuously
`components/pharos_ui/pharos_hud.c:1253`

**Wrong.** Lesson 2 in the file banner says every write is dirty-checked, and the file provides set_text/set_fg/set_bg/set_arc_rgb/show to do exactly that. The HOME dot loop bypasses them for geometry and opacity: lv_obj_align() and lv_obj_set_style_bg_opa()/lv_obj_set_style_text_opa() are called unconditionally for every visible dot and every named dot, every repaint. In the vendored LVGL v9 those do not compare. lv_obj_set_style_bg_opa/text_opa/border_opa/border_color/radius are thin wrappers over lv_obj_set_local_style_prop(), which ends in lv_obj_refresh_style(), which calls lv_obj_invalidate(obj) with no value test (twice, at lv_obj_style.c:250 and :279). lv_obj_align() is worse: lv_obj_set_pos() -> lv_obj_set_x/y DO compare, but lv_obj_align() first calls lv_obj_set_style_align() unconditionally, and LV_STYLE_ALIGN carries LV_STYLE_PROP_FLAG_LAYOUT_UPDATE (lv_style.c:47), so each call also sends LV_EVENT_STYLE_CHANGED and marks both the object and its parent page container layout-dirty. The same pattern appears in the LIVE chip loop (lines 1480-1481, border colour and opacity, on the page that runs while the radio is loaded) and the GUIDE pip loop (1735, 1739), where it sits immediately above a comment explaining why the animation below it IS guarded.

**Evidence.**
```c
        lv_obj_set_size(s_h_dot[i], size, size);
        lv_obj_align(s_h_dot[i], LV_ALIGN_CENTER, pt.x - PR_CX, pt.y - PR_CY);
        set_bg(s_h_dot[i], home_dot_colour(h->state[i]));
        const uint8_t f = h->fade[i];
        uint8_t opa = (f >= 3u) ? 64u : (uint8_t)(255u - f * 60u);
        if (active) opa = 255u;
        lv_obj_set_style_bg_opa(s_h_dot[i], opa, 0);
...
            lv_obj_align(s_h_name[i], LV_ALIGN_CENTER, lp.x - PR_CX, lp.y - PR_CY);
            set_fg(s_h_name[i], active ? C_ACCENT : C_DIMMER);
            lv_obj_set_style_text_opa(s_h_name[i], opa, 0);

LIVE, lines 1480-1481:
        lv_obj_set_style_border_color(s_l_chip[i], lv_color_hex(rgb), 0);
        lv_obj_set_style_border_opa(s_l_chip[i], lit ? 150 : LV_OPA_TRANSP, 0);

and the LVGL side, managed_components/lvgl__lvgl/src/core/lv_obj_style.c:242-250:
void lv_obj_refresh_style(lv_obj_t * obj, lv_part_t part, lv_style_prop_t prop)
{
    ...
    lv_obj_invalidate(obj);
```

**Trigger.** Boot to HOME with the usual twelve armed watches and leave the device completely still in a quiet room. paint() runs every 100 ms (pharos_ui.c) and VIEW_HOME dispatches unconditionally to paint_home() -> pharos_hud_home(&h), so with nothing changing at all the face still issues 12 lv_obj_align() calls, 12 bg_opa writes, plus an align and a text_opa write per named dot, ten times a second, forever - each one invalidating its object and marking the 466x466 page container's layout dirty. Confirm on device by watching the `painted=/missed=` counter the UI loop logs every 100 heartbeats while sitting on HOME versus sitting on BROWSE (which does not repaint at all).

**Impact.** The file's central claim - "In the steady state a repaint here invalidates nothing at all" - is false on HOME, which is the default page. Worse, that claim is the stated justification for raising the repaint rate: the UI loop's own comment says "Every write goes through a dirty check against what the widget already holds, so a reading that has not moved invalidates nothing at all and a faster loop costs only the comparisons." The rate was chosen on a premise the HUD does not meet, on a board where the display flush competes with the Wi-Fi driver for scarce internal DMA RAM and where dropped frames were already measured at ~7%. This is the same defect class as the original flicker, at smaller scale.

**Fix.** Add dirty-checked helpers alongside the existing ones and route these calls through them: `set_pos(o, dx, dy)` that reads lv_obj_get_x_aligned/lv_obj_get_y_aligned and returns early when unchanged (the align value itself is constant here, so set it once at build time in mk_surface/mk_label and use lv_obj_set_pos thereafter, which already compares); `set_bg_opa(o, v)` / `set_text_opa(o, v)` / `set_border(o, rgb, opa)` comparing against lv_obj_get_style_*. The dot positions only change when h->n changes, and the fade opacity only when ptw_freshness() steps, so in the steady state all of it collapses to comparisons - which is what the banner promises.

### [high] dead-feature — struct pharos_hud_home::score[] is filled per watch by the caller and never read by the HUD
`components/pharos_ui/include/pharos_hud.h:145`

**Wrong.** The header declares a per-watch score array in the home contract and pharos_ui.c fills it on every home paint, but pharos_hud.c never reads it - `grep 'h->score' pharos_hud.c` is empty. The ring draws dot colour from h->state[i] and dot opacity from h->fade[i], and the only score that reaches the glass is the scalar h->worst_score on the rim arc. The field is also the one member of the struct with no explanatory comment, while the block comment above it describes only `dots` (state) and `fade`.

**Evidence.**
```c
components/pharos_ui/include/pharos_hud.h:
    uint8_t state[PHAROS_HUD_HOME_MAX];
    uint8_t fade[PHAROS_HUD_HOME_MAX];
    uint8_t score[PHAROS_HUD_HOME_MAX];

components/pharos_ui/pharos_ui.c:549, inside the per-watch fill loop:
        h.label[d] = s_tower.w[i].name;
        h.state[d] = (uint8_t)s_tower.w[i].state;
        h.fade[d] = (uint8_t)ptw_freshness(&s_tower, i, now);
        h.score[d] = s_tower.w[i].score;
        s_home_map_n++;
```

**Trigger.** Arm two watches, one at state=2 with score 40 and one at state=2 with score 95. Both dots render identically: same colour (PS_HIGH from home_dot_colour(2)), same size, same opacity. The severity difference the caller measured, copied and handed over is discarded in pharos_hud_home().

**Impact.** Dead data on every home frame, and a real gap in the page the ring exists to serve: HOME can tell you WHICH watch wants attention but not HOW badly, so two dots of the same colour force the operator to step into each lens to rank them - the walk the ring is supposed to save. It is also a drift hazard: a future reader of the header will reasonably assume the score is on screen somewhere.

**Fix.** Either use it or delete it. Using it is cheap and stays inside the existing geometry - scale the dot diameter with the score (e.g. 14 px at 0 rising to 20 px at 100, with the active dot still the largest), or add a thin per-dot tick on the ring track. Deleting it means removing the array from struct pharos_hud_home and the assignment at pharos_ui.c:549. Whichever is chosen, give the field a comment saying what the HUD does with it, as every other field in that struct has.

### [high] dead-feature — pharos_hud_detail()'s `openable` argument is computed by the caller every frame and thrown away
`components/pharos_ui/pharos_hud.c:1555`

**Wrong.** The last parameter of pharos_hud_detail() is discarded with an explicit `(void)openable;` and nothing in the header documents what it is for - the DETAIL block comment describes rows, n, page and pages, and never mentions it. The caller does real work to produce it on every detail paint. Per-row affordance is already carried by rows[i].tappable, which drives the stripe and the chevron, so the page-level flag has no remaining job.

**Evidence.**
```c
components/pharos_ui/pharos_hud.c:
    set_text(s_d_page, pg);
    (void)openable;
}

components/pharos_ui/pharos_ui.c:1834, computing it every frame:
    pharos_hud_detail(lens_caps(active), active ? active->row_head_left : NULL,
                      active ? active->row_head_right : NULL, rows, n,
                      s_detail_page, pages, focus,
                      active && (active->row_expand || active->row_edit));
```

**Trigger.** Open the detail page of any lens that defines row_expand or row_edit (e.g. Settings). The caller evaluates `active && (active->row_expand || active->row_edit)` ten times a second and the HUD ignores the result; the page looks identical to one from a lens with neither hook.

**Impact.** Low on its own - a dead parameter and a few wasted cycles - but it is an undocumented argument in a public header, which is the kind of thing a future caller passes wrongly because there is nothing to pass it right against. It also hides a small missed affordance: a list whose rows can be opened currently announces that only per row, never at the page level.

**Fix.** Drop the parameter from pharos_hud.h, pharos_hud.c (both the ESP and the host stub) and the two call sites in pharos_ui.c. If it is wanted instead, spend it on something visible - the row-zone hint or the page counter's tone - and document it in the header where the other parameters are described.

### [medium] bug — split_two()'s ellipsis fixup writes one byte past the tail buffer whenever a caller passes cap == bn-1 with an over-long tail
`components/pharos_ui/pharos_hud.c:1348`

**Wrong.** The fixup writes b[cap-1], b[cap] and b[cap+1] but is guarded only by `strlen(b) > cap`, not by the size of b. b is an out-parameter of size bn, so the highest legal index is bn-1; the code can reach bn and beyond whenever cap >= bn-1. Today it is unreachable, and only by arithmetic that lives entirely in the callers: pharos_hud_browse passes `cap < 39u ? cap : 39u` with char l2[40], and pharos_hud_live passes `cap < 47u ? cap : 47u` with char a2[48], i.e. cap <= bn-2 or cap == bn-1; and when cap == bn-1 the preceding snprintf has already bounded strlen(b) to bn-1 == cap, so the condition is false. I am reporting it as a latent defect, not a live one - a third caller that passes cap == bn-1 with a differently-bounded b, or that widens the buffers without re-deriving the clamp, gets a one-byte stack write past a[]/b[].

**Evidence.**
```c
static void split_two(const char *s, unsigned cap, char *a, unsigned an,
                      char *b, unsigned bn)
{
...
    snprintf(b, bn, "%.*s", (int)(bn - 1), rest);
    /* If the tail still will not fit, ellipsize rather than clip mid-glyph. */
    if (strlen(b) > cap) { b[cap - 1] = '.'; b[cap] = '.'; b[cap + 1] = '\0'; }
}
```

**Trigger.** Not reachable from the two current call sites - I checked both and neither can satisfy it. It fires the moment someone writes, for example, `char t[16]; split_two(s, 15u, head, sizeof head, t, 32u);` - any call where cap is derived from something other than `bn - 1`. The write lands at t[16], one byte past a stack array.

**Impact.** None today. The cost is that a memory-safety property of a static helper is enforced four call-sites away by two hand-clamped constants that are not commented as load-bearing, on a device with no MMU where a one-byte stack write past a char array corrupts whatever the compiler placed next (in pharos_hud_browse that is l1[40]; in pharos_hud_live it is a1[48]).

**Fix.** Clamp inside the helper, where bn is known, rather than relying on callers: `unsigned e = cap < bn - 2u ? cap : bn - 2u; if (strlen(b) > e) { b[e - 1] = '.'; b[e] = '.'; b[e + 1] = '\0'; }` with an early `if (bn < 3u) return;`. Note the head buffer is already safe - `unsigned na = cut < an - 1 ? cut : an - 1;` does exactly this clamp - so the fix makes the two halves of the function consistent.


## lens-surfaces — every display() and row()/row_expand()/row_edit() in components/pharos_lens_*/lens_*.c

**Coverage.** All 20 lens files under components/pharos_lens_*/ (8,274 lines total). Read end to end: lens_aegis.c (185), lens_survey.c (175), lens_ring.c (251), lens_karma.c (255), lens_mirage.c (274), lens_probe.c (302), lens_locate.c (287), lens_census.c (975, both Census and Twin). Read from the first display/row function to EOF: lens_footprint.c (150-411), lens_harvest.c (150-381), lens_roster.c (150-415), lens_sentinel.c (130-365), lens_vigil.c (240-446), lens_ward.c (440-651), lens_system.c (110-759), lens_spectrum.c (180-301), lens_squall.c (160-292), lens_whisper.c (200-324), lens_rival.c (330-604), lens_watch.c (470-600 rows). Supporting reads: components/pharos_core/include/pharos_lens.h in full (the row/display contract); components/pharos_ui/pharos_ui.c paint()/paint_detail()/nav (lines 1095-1240, 1741-1990, 2120-2130); components/pharos_ui/pharos_hud.c set_text_fit_r/split_two/pharos_hud_live/touch zones (180-215, 1069-1085, 1330-1350, 1399-1505). Engine headers and implementations read to confirm each "computed but never shown" claim: pharos_twin.[ch], pharos_karma.[ch], pharos_aegis.[ch], pharos_probe.[ch], pharos_locate.[ch], pharos_sentinel.[ch], pharos_opsec.h, pharos_census.c, pharos_flood.c, pharos_vigil.c, pharos_watch.h. Checked test/host/ (test_locate.c, test_aegis.c, test_chain.c, test_privacy.c, test_sentinel.c, test_engines.c) — every host test exercises the ENGINE side; none renders a lens row or display string, so none of these findings is covered. Also read the headings of docs/lens-audit.md and deliberately did NOT re-report anything in its aegis/rival/spectrum/squall/survey/watch/whisper sections. Four host probes compiled with cc against the real engine sources and run: /private/tmp/pharos_probe/{locate_probe.c, rows_probe.c, system_expand_probe.c, advice_probe.c}.

### [high] bug — System: the transmit-fence evidence page is unreachable — its `sub` case labels are offset by ROW_EDITABLE_N (7)
`components/pharos_lens_system/lens_system.c:699`

**Wrong.** k_system_expand's arm for the transmit-fence row switches on `sub` (a 0-based line index inside one expansion) but labels its five cases with the row-index idiom `ROW_EDITABLE_N + -5` .. `ROW_EDITABLE_N + -1`. ROW_EDITABLE_N is 7, so those labels evaluate to sub == 2,3,4,5,6 and there is no case for sub == 0. paint_detail (pharos_ui.c:1758 and 1767-1773) probes an expansion from sub 0 and treats a false at sub 0 as 'this row has nothing to open', dropping straight back to VIEW_DETAIL. The five lines that prove the fence — the one claim the whole product rests on — can therefore never be drawn. The same copy-paste hits the ROW_ALARM/ROW_VOLUME arm at line 650. The comment at lines 234-240 introduces the relative-case idiom for the OUTER row switch; it was then applied to the inner sub switch, where it is simply wrong.

**Evidence.**
```c
    case ROW_EDITABLE_N + 0: /* transmit fence */
        switch (sub) {
        case ROW_EDITABLE_N + -5:
            snprintf(out->left, sizeof(out->left), "wrap traps linked");
            snprintf(out->right, sizeof(out->right), "%s",
                     s.fence.wrap_linked ? "yes" : "NO");

...and the enum it is offset by:
enum {
    ROW_THEME = 0,
    ROW_BRIGHT,
    ROW_ALARM,
    ROW_VOLUME,
    ROW_REGION,
    ROW_GUIDE,
    ROW_BATTERY,
    ROW_EDITABLE_N, /* everything from here down is read-only */
};
```

**Trigger.** Open System, press the bottom strip to reach the detail list, page to the row 'transmit fence / CLEAN' (row index 7), press the centre. Nothing opens. Reproduced with /private/tmp/pharos_probe/system_expand_probe.c, which compiles the identical enum and case labels:
  ROW_EDITABLE_N = 7
  the five sub labels evaluate to: 2 3 4 5 6
  row_expand(fence, sub=0) -> FALSE (end of page)
  ...
  paint_detail would count total=0 lines -> page closed immediately, tap does nothing

**Impact.** The device's central honesty claim is 'it cannot transmit'. The lens summary promises 'proof the transmit fence is clean'. The four pieces of evidence for that proof — wrap traps linked, BLE observer only, no TX symbol in image, attempts since boot — are written, maintained, and unreachable from the glass. An operator who taps the fence row to interrogate the claim gets a page that closes itself, which reads as a broken device at exactly the moment they were trying to verify it.

**Fix.** Change the inner labels in both arms to plain `case 0:` .. `case 4:`. Add a host test that walks row_expand(row, sub) from sub 0 for every row of every lens and asserts that any row whose first sub returns false has no later sub that returns true — that one test catches this class everywhere at once.

### [high] dishonesty — Locate: a target that has gone quiet reports 0 dBm, "too few samples yet" after 200 samples, and tells the operator to keep waiting for it to settle
`components/pharos_lens_locate/lens_locate.c:168`

**Wrong.** pl_evaluate_at's PL_TREND_LOST branch (pharos_locate.c:135-143) returns early, before out->locked, out->closeness, out->confidence and out->rssi_smoothed are assigned — so they stay at the memset zero. k_locate_display then reads locked==false and prints the two strings it reserves for a hunt that has not started yet, and prints rssi_smoothed as a level. It also discards the engine's own sentence for exactly this case. pharos_locate.h devotes fifteen lines of comment to why a stale Locate reading is the worst thing this lens can do ('a person walking across a building following a needle that is pointing at a memory'), and the display undoes it.

**Evidence.**
```c
    snprintf(o->detail, sizeof(o->detail), "%02x:%02x:%02x ch%u  %d dBm",
             t[3], t[4], t[5], (unsigned)ch, (int)v.rssi_smoothed);
    snprintf(o->advice, sizeof(o->advice), "%s",
             v.locked ? "Walk slowly; watch the trend."
                      : "Hold still to settle the reading.");
    if (!v.locked) {
        snprintf(o->why, sizeof(o->why), "too few samples yet");
    }

while the engine wrote, and the lens never reads:
        out->headline = "Gone quiet - stop and wait, or it has moved out of range";
```

**Trigger.** Set a target, hear 200 frames from it, then let it stop transmitting for more than PL_STALE_US (3 s). Reproduced by /private/tmp/pharos_probe/locate_probe.c, which links the real pharos_locate.c and replays k_locate_display verbatim:
  LIVE   trend=STEADY     samples=200 locked=1 closeness=73 conf=100 smoothed=-46
         detail="ef:01:02 ch6  -46 dBm" advice="Walk slowly; watch the trend."
  QUIET  trend=GONE QUIET samples=200 locked=0 closeness=0 conf=0 smoothed=0
         big="0" band="GONE QUIET" detail="ef:01:02 ch6  0 dBm"
         advice="Hold still to settle the reading." why="too few samples yet"
         engine headline it discarded: "Gone quiet - stop and wait, or it has moved out of range"  silent=10s

**Impact.** Three wrong statements at once on the lens somebody follows with their feet. 0 dBm is the strongest reading the scale can hold, printed at the instant the target vanished. 'too few samples yet' is flatly false with 200 samples, and it is the detector reading its own missing instrumentation as a finding. And 'Hold still to settle the reading' tells the operator to stand and wait for a transmitter that has stopped — the engine's own advice is to stand still OR accept that it has moved out of range, which is a different decision.

**Fix.** In k_locate_display, branch on v.trend == PL_TREND_LOST before the locked test: use v.headline (or pl_trend_advice(v.trend)) for o->advice, put v.silent_us in o->why as "quiet for %us", and suppress the dBm figure (print the last-heard v.rssi_now, which the LOST branch does fill, or '--'). Separately, make the LOST branch in pl_evaluate_at set out->locked from e->samples and carry rssi_smoothed forward, so no caller can mistake 'gone' for 'not started'. Add a host test in test/host/test_locate.c asserting that a LOST verdict never renders the not-locked strings.

### [high] dishonesty — Sentinel: with no baseline adopted, every row on the change page is a green tick asserting nothing changed
`components/pharos_lens_sentinel/lens_sentinel.c:310`

**Wrong.** ps_compare returns early when the baseline is not adopted, leaving every count at zero and setting PS_NOTE_NO_BASELINE (pharos_sentinel.c:113-118). k_sentinel_display reads s_baseline.adopted and says so honestly on the live face — but k_sentinel_row never consults it, and its tone ladders read a zero count as good news. The detail page, which is the whole point of the lens, becomes four green ticks about a comparison that was never made. PS_NOTE_NO_BASELINE reaches no row and no display line.

**Evidence.**
```c
    case 0:
        snprintf(out->left, sizeof(out->left), "new networks");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_new);
        out->tone = v.n_new ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD; return true;
    ...
    case 2:
        snprintf(out->left, sizeof(out->left), "security dropped");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_downgrade);
        out->tone = v.n_downgrade ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD; return true;
```

**Trigger.** Boot the device, start Sentinel without ever running `sentinel adopt` over USB, open the detail page. Reproduced by /private/tmp/pharos_probe/rows_probe.c against the real pharos_sentinel.c:
  SENTINEL no-baseline: band=UNCHANGED score=0 notes=0x01 headline="No baseline adopted - nothing to compare against"
    new networks               | 0           | GOOD
    gone missing               | 0           | GOOD
    security dropped           | 0           | GOOD
    security improved          | 0           | GOOD
    changed channel            | 0           | DIM
    renamed                    | 0           | DIM
    findings total             | 0           | DIM

**Impact.** 'security dropped: 0' in green is the single most reassuring line the device can draw, and here it means 'I have nothing to compare against'. That is the ambiguous zero in its purest form — the same 0 for 'no network got weaker' and 'I never looked'. It is also the detector reading its own missing instrumentation as a finding, which the honesty rules forbid outright.

**Fix.** At the top of k_sentinel_row, when the baseline is not adopted, emit one row — left "no baseline adopted", right "--", tone PHAROS_TONE_LIMIT (this is a state of the instrument, not of the air) — and return false for every other index, so the page cannot show a comparison it did not make. Take s_baseline.adopted under s_lock the way the snapshot does rather than reading it bare as k_sentinel_display currently does at line 285. Add a host test that a verdict carrying PS_NOTE_NO_BASELINE produces no PHAROS_TONE_GOOD row.

### [high] dishonesty — Karma: the engine's "No probe requests heard yet" headline is thrown away, and the face asserts honest access points from zero evidence
`components/pharos_lens_karma/lens_karma.c:185`

**Wrong.** pharos_karma.h states the detection rests entirely on probe requests: a KARMA responder is caught by answering names nobody advertised. When no probe requests are heard, the engine sets PK_NOTE_NO_PROBES and writes the honest headline 'No probe requests heard yet - nothing to answer' (pharos_karma.c:395-397). k_karma_display ignores v.headline entirely — it is written to JSON at line 157 and nowhere else — and feeds the face pk_band_advice(v.band), which for PK_BAND_NORMAL is a positive claim about the room. k_karma_row then paints 'never announced: 0' green. None of PK_NOTE_NO_PROBES, PK_NOTE_THIN_DWELL or PK_NOTE_TABLE_FULL reaches any surface.

**Evidence.**
```c
    snprintf(o->advice, sizeof(o->advice), "%s", pk_band_advice(v.band));
...
    case 1:
        snprintf(out->left, sizeof(out->left), "never announced");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.unannounced);
        /* The gap is the finding: a legitimate multi-SSID access point
         * beacons every name it answers for. */
        out->tone = v.unannounced ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD; return true;
```

**Trigger.** Run Karma in a room where the access points beacon but no phone probes (a locked-screen room, or any short rotation slice). Reproduced by /private/tmp/pharos_probe/rows_probe.c: 60 beacons from one AP, zero probe requests, camped dwell —
  KARMA no-probes: band=NORMAL score=0/94 notes=0x02
    engine headline it discarded: "No probe requests heard yet - nothing to answer"
    advice the face shows instead: "Radios here answer only for networks they also announce. That is what an honest access point looks like."
    names answered             | 0           | NEUTRAL
    never announced            | 0           | GOOD
    answered promptly          | 0           | DIM
    responder                  | 00:00:00    | DIM

**Impact.** The lens states as fact that the radios here are honest, at a ceiling of 94, having tested nothing. That is an absence-based claim presented as positive evidence and it is the 'never say you are safe' rule broken directly. The green tick on 'never announced: 0' is the same zero meaning 'no gap found' and 'no test performed'. The 'responder: 00:00:00' row compounds it by displaying a zeroed MAC as if it were an identified radio.

**Fix.** Use v.headline for o->advice (as harvest, squall, sentinel and vigil already do) and fall back to pk_band_advice only when the headline is empty. Make row 1's tone DIM rather than GOOD while PK_NOTE_NO_PROBES is set, and add a row that says the note out loud — 'probe requests heard' / '%u' — since that count is the licence for every other number on the page. Show '-' rather than 00:00:00 in row 3 when v.answered_ssids is zero.

### [high] dishonesty — Twin: before it has heard a single group, the face reads "0 / CONSISTENT / looks like part of the same deployment"
`components/pharos_lens_census/lens_census.c:846`

**Wrong.** twin_tick memsets a local `worst` each pass and only writes it from pt_evaluate when a group has two or more members (`if (n < 2) { continue; }`, line 287). In a room of singleton SSIDs, or before any beacon arrives, s_twin_worst stays all zero — and PT_BAND_CONSISTENT is enum value 0, so pt_band_name returns 'CONSISTENT' and pt_band_advice returns a positive statement. k_twin_display sets has_score unconditionally, so the face shows a real 0..0 gauge rather than the '--' Census uses for the same situation. Separately, none of PT_NOTE_SINGLE, PT_NOTE_THIN, PT_NOTE_NO_PROFILE, PT_NOTE_ROAMING, PT_NOTE_OPEN_MEMBER or PT_NOTE_LOCAL_MAC is referenced by any lens file — pharos_twin.h says of the site baseline 'which is why its absence is disclosed on every verdict', and that disclosure never reaches the glass.

**Evidence.**
```c
static bool k_twin_display(struct pharos_lens_display *o)
{
    pt_verdict_t v = s_twin_worst;
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pt_band_name(v.band));
    snprintf(o->detail, sizeof(o->detail), "\"%s\"  ceil %u", s_twin_ssid, v.ceiling);
    snprintf(o->advice, sizeof(o->advice), "%s", pt_band_advice(v.band));
    o->score = v.score; o->ceiling = v.ceiling; o->has_score = true;
```

**Trigger.** Start Twin anywhere where no SSID is carried by two or more BSSIDs — a home, or the first second of any session. Reproduced by /private/tmp/pharos_probe/rows_probe.c rendering a zeroed pt_verdict_t through the real pt_band_name/pt_band_advice:
  big="0" band="CONSISTENT" detail="\"\"  ceil 0"
  advice="Every radio carrying this name looks like part of the same deployment. Many BSSIDs on one SSID "

**Impact.** A confidence ceiling of 0 is the engine saying it has earned nothing, and the face draws it as a clean verdict with a reassuring sentence about a network it has not identified. The detail line renders as an empty pair of quotes. Census, sitting in the same file, gets this right ('--' / 'listening' / 'no networks heard yet' with has_score false); Twin does not. The missing PT_NOTE_NO_PROFILE disclosure means an operator is never told that the single biggest accuracy lever — adopting a site baseline — has not been pulled.

**Fix.** Give k_twin_display the same guard Census has: when no group has been evaluated (track a `bool s_twin_have` set in twin_tick alongside s_twin_worst) print '--' / 'listening' / 'no repeated network names yet' with has_score false. Add rows for the notes — at minimum 'site baseline' / 'none' (PHAROS_TONE_LIMIT) when PT_NOTE_NO_PROFILE is set, and 'graded on few beacons' when PT_NOTE_THIN is — and take s_twin_worst under s_lock in the display as k_twin_row already does.

### [high] ux — Twin, Karma, Mirage and Census write advice the LIVE face cannot carry, and the part cut off is the instruction
`components/pharos_ui/pharos_hud.c:1501`

**Wrong.** struct pharos_lens_display::advice is 96 bytes and the face renders it as two lines capped at 47 characters each (split_two at pharos_hud.c:1501), so about 94 characters reach the glass. set_text_fit_r's own comment states the rule: 'Truncating is a last resort, not a licence - an engine whose advice needs cutting should shorten its advice.' Four lenses pipe band-advice strings of 95-126 characters straight into o->advice, and because the imperative is written last in every one of them, it is the imperative that is lost.

**Evidence.**
```c
        char a1[48], a2[48];
        const unsigned cap = ps_capacity(PS_TYPE_LABEL, PS_Y_ACTION, PR_SAFE_R);
        split_two(d->advice, cap < 47u ? cap : 47u, a1, sizeof a1, a2, sizeof a2);

and, for example, in lens_census.c:
    snprintf(o->advice, sizeof(o->advice), "%s", pt_band_advice(v.band));
```

**Trigger.** Open Twin, Karma, Mirage or Census in any state. Reproduced by /private/tmp/pharos_probe/advice_probe.c, linking the real engines and measuring against the 94-character budget:
  twin      TWIN LIKELY   122 chars -> CUT, loses 28: "...t join it. Preserve the log."
  twin      CONSISTENT    121 chars -> CUT, loses 27: "... is roaming, not an attack."
  karma     SUSPICIOUS    126 chars -> CUT, loses 32: "...ouncement was not simply missed."
  karma     KARMA LIKELY  121 chars -> CUT, loses 27: "...erve the log and locate it."
  mirage    FLOOD LIKELY  122 chars -> CUT, loses 28: "... A beacon flood. Locate it."
  mirage    SUSPICIOUS    119 chars -> CUT, loses 25: "...hat is what a flood does."
  census    (every grade)  99 chars -> CUT, loses 5: "...sure."
(all four twin bands, all four karma bands, three mirage bands and all seven census grades overflow)

**Impact.** On the highest-severity reading each of these lenses can produce, the operator is shown the diagnosis and not the action. 'Do not join it. Preserve the log.' and 'preserve the log and locate it.' are the only parts of those sentences that say what to do, and neither reaches the screen. The CONSISTENT case loses 'Many BSSIDs on one SSID is roaming, not an attack' — the disclaimer that stops a roaming estate being read as a finding.

**Fix.** Shorten the offending returns in pharos_twin.c, pharos_karma.c, pharos_flood.c and pharos_census.c to 94 characters or fewer, leading with the imperative, and move the reasoning into the per-band detail rows or the report where there is room. Then add a host test alongside test_style.c that asserts strlen() <= 94 for every string returned by pt_band_advice, pk_band_advice, pf_band_advice, pw_band_advice, pc_grade_advice and pac_band_hint, so a new band cannot silently reintroduce it.

### [high] ux — Footprint and Squall have rows whose tone can never be anything but WARN or BAD, so a quiet room is drawn as an alarm
`components/pharos_lens_footprint/lens_footprint.c:374`

**Wrong.** Three of Footprint's six rows have a tone that no input can change: row 0 is BAD above 75 and WARN otherwise (never GOOD, never DIM), row 2 is a bare `PHAROS_TONE_WARN`, and row 4 is a bare `PHAROS_TONE_BAD`. Row 4's left text is po_report_t::tell_name, which po_assess always fills — with 'no dominant tell' when nothing was heard — so the fallback `: "no single tell"` is dead code and the row renders the absence of a finding in red. Squall's row 0 has the same shape (`out->tone = PHAROS_TONE_WARN;` at lens_squall.c:220) and prints 'ch 0' before any channel has been graded. This is exactly the failure PHAROS_TONE_LIMIT's comment in pharos_lens.h names: 'a warning that is always present is a warning nobody reads'.

**Evidence.**
```c
    case 2:
        snprintf(out->left, sizeof(out->left), "stealth gap");
        snprintf(out->right, sizeof(out->right), "%u", r.stealth_gap);
        out->tone = PHAROS_TONE_WARN; return true;
    case 3: ...
    case 4:
        snprintf(out->left, sizeof(out->left), "%.25s",
                 r.tell_name ? r.tell_name : "no single tell");
        snprintf(out->right, sizeof(out->right), "loudest");
        out->tone = PHAROS_TONE_BAD; return true;

and the engine that makes the fallback unreachable:
    out->tell_name = "";
    ...
    out->tell_name = po_tell_name(out->dominant_tell);
...
    case PO_TELL_NONE:
    default:               return "no dominant tell";
```

**Trigger.** Start Footprint in a silent room (its display path for this is `frames < 20u`, 'listening to this room') and open the detail page. The rows read: 'camped defender 0' amber, 'hopping defender 0' neutral, 'stealth gap 0' amber, 'families lit 0' dim, 'no dominant tell / loudest' RED, 'hoppers miss it / no' green. For Squall, start it before any channel has completed a dwell: 'worst channel / ch 0' amber.

**Impact.** Footprint's page is amber and red every time it is opened, including when it has heard nothing at all, and a red row that says 'no dominant tell' is a contradiction on its face. Every alarm colour spent on a permanent condition is an alarm colour the operator learns to skip, which is the cost pharos_lens.h introduced PHAROS_TONE_LIMIT to avoid.

**Fix.** Give rows 0 and 2 real ladders keyed on the score (GOOD/DIM below 40, WARN 40-74, BAD 75+; stealth_gap DIM at 0). Make row 4 conditional: when r.dominant_tell == PO_TELL_NONE show 'no dominant tell' / '-' at PHAROS_TONE_DIM, and only paint BAD when a tell was actually identified. Do the same for lens_squall.c:220, and show '-' rather than 'ch 0' while v.n_graded is zero. While in Footprint, also show po_report_t::guidance ('what made it loud, stated plainly') — it and po_report_t::headline have no reader anywhere in the tree, which is the one thing a red-team mirror exists to say.

### [high] dishonesty — Census says "no networks heard yet" about networks it heard, and "worst of N" counts networks it never graded
`components/pharos_lens_census/lens_census.c:333`

**Wrong.** The variable is named n_graded but is assigned the full table size whenever any single access point is graded. Two consequences follow from the one line. If every AP in the table is still PC_GRADE_UNGRADED — which pc_grade returns for any AP heard fewer than three times (pharos_census.c:38-43) — n_graded is 0 and the face prints 'no networks heard yet' about a table full of networks. And once one AP is graded, the band reads 'worst of <total>' even though the other nineteen were never assessed.

**Evidence.**
```c
    const pc_verdict_t v = have_graded ? s_grades[worst] : (pc_verdict_t){ 0 };
    const unsigned n_graded = have_graded ? n : 0u;
    xSemaphoreGive(s_lock);
    n = n_graded;

    if (!n) {
        snprintf(o->big, sizeof(o->big), "--");
        snprintf(o->band, sizeof(o->band), "listening");
        snprintf(o->detail, sizeof(o->detail), "no networks heard yet");
        o->has_score = false;
        return true;
    }
    ...
    snprintf(o->band, sizeof(o->band), "worst of %u", n);
```

**Trigger.** Start Census. Its plan is a 350 ms dwell across the sweep, and pc_grade refuses to grade below three beacons, so the first several seconds have a populated table with zero graded entries — the face says 'no networks heard yet' while the detail page underneath already lists them. Then, as soon as one AP crosses three beacons in a room of twenty, the face says 'worst of 20' on the strength of one graded network.

**Impact.** Two different ambiguous statements from one line. 'no networks heard yet' is contradicted by the lens's own row list on the next screen, which is the kind of small internal contradiction that costs a tool its credibility. 'worst of 20' overstates the sample the verdict rests on, which is precisely the dwell-scaling the honesty rules require in the other direction.

**Fix.** Count the graded entries in the existing loop (`unsigned n_graded = 0; ... if (g != PC_GRADE_UNGRADED) n_graded++;`) and use that number for 'worst of %u'. Separately gate the '--' branch on s_n_aps rather than on n_graded, so a table with entries but no grades reports 'N heard, none graded yet' instead of claiming silence.

### [high] ux — Probe: a room of well-behaved phones reads "0 / --", and every per-device explanation the engine wrote is discarded
`components/pharos_lens_probe/lens_probe.c:179`

**Wrong.** k_probe_display picks the worst device with a strictly-greater comparison against a memset verdict. pp_grade_device gives a device that named nothing an exposure of 0 with a real grade (PP_GRADE_A_PLUS, or PP_GRADE_B when it also never randomises — pharos_probe.c:314-320), so no such device can ever displace `worst`, and the band falls back to pp_grade_name(PP_GRADE_UNGRADED), which is '--' and means 'too few probes heard to say anything'. Separately, pp_grade_advice() has no caller anywhere in the tree, pp_verdict_t::headline is never read by a lens, and PP_NOTE_RELINKED — 'randomisation defeated in this session', named in this file's own opening comment as one of the three things the lens exists to show — reaches the JSON report and no row. The lens has no row_expand, so a grade letter on a device row cannot be interrogated at all.

**Evidence.**
```c
        if (v.exposure > worst.exposure) {
            worst = v;
        }
    }
    ...
    snprintf(o->big, sizeof(o->big), "%u", worst.exposure);
    snprintf(o->band, sizeof(o->band), "%s", pp_grade_name(worst.grade));

and the engine's own text, with no reader:
        out->headline = (out->notes & PP_NOTE_NO_RANDOM)
                            ? "Names nothing, but always uses the same address"
                            : "Asks for nothing by name - this is what good looks like";
```

**Trigger.** Run Probe in a room where every phone sends four or more wildcard probes and no directed ones — modern iOS and Android default behaviour. Every device grades A+ at exposure 0, `worst` is never assigned, and the face shows big='0' band='--' advice='Nothing named yet.' A single device graded B (names nothing but never randomises — a trackable device, a real finding) produces the identical '0 / --'.

**Impact.** The same '0 / --' means 'nobody has probed yet', 'every phone here is exemplary' and 'there is a permanently trackable device here'. The lens was written to be used in awareness sessions, where the A+ case is the one worth showing people, and it is indistinguishable from the device having heard nothing. And the per-device grade in the row list is a claim nobody can open — the project's stated objection to grades elsewhere.

**Fix.** Track the worst device by index with a have-flag rather than by comparing against a zeroed verdict, so a graded exposure-0 device wins over nothing. Add a row_expand for the device rows that shows pp_grade_advice(&v) / v.headline plus one line per note (RELINKED, NO_RANDOM, THIN, UNIQUE_NAME) — PP_NOTE_RELINKED in particular, since 'this device's randomisation was defeated' is the strongest thing this lens can prove.

### [high] ux — Aegis: "3 stages raised" and no surface says which three
`components/pharos_lens_aegis/lens_aegis.c:131`

**Wrong.** pa_verdict_t::stage_mask ('bit per pa_stage_t that has been raised') is written by pa_evaluate at pharos_aegis.c:125, emitted to JSON at lens_aegis.c:91, and read by no row and no display line. The five rows are counts and one age. pa_stage_meaning() — a full set of plain-English stage explanations — has no production caller at all; its only reference in the tree is test_aegis.c:179. The band advice for PA_BAND_ELEVATED tells the operator to 'Open the lens named below and read its own verdict', and nothing below names a lens.

**Evidence.**
```c
    case 0:
        snprintf(out->left, sizeof(out->left), "stages raised");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_raised);
        out->tone = v.n_raised ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD; return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "still current");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_live);

and the field with no reader:
    uint8_t stage_mask;    /* bit per pa_stage_t that has been raised  */
```

**Trigger.** Let the rotation run until three stages have been raised, then open Aegis and its detail page. It reads 'stages raised 3 / still current 1 / worst peak 80 / how long ago 4m ago / earned-allowed 80/96'. Only the live face's detail line names one stage (the worst); the other two are unnameable from the device.

**Impact.** Aegis exists because 'an attack is a SEQUENCE, not a reading' and 'nothing that looks at one lens at a time can see that' — and the page that holds the sequence cannot list it. An operator who reads 'three stages raised' has to guess which lenses to go back to, which is the one question the correlator is uniquely able to answer. The header's 'act on this' advice compounds it by telling them to open a lens the page never names.

**Fix.** Add one row per bit set in v.stage_mask: left pa_stage_name(i), right the stage's peak and whether it is still live, tone by peak. Head each with pa_stage_meaning(i) in a row_expand, which is the page an operator who does not know what IMPERSONATE means will be standing on. Row 0's tone should be DIM rather than GOOD when n_raised is zero, for the same reason as the Sentinel and Karma findings — nothing raised is not the same as nothing happening, and pa_band_advice(PA_BAND_CLEAR) already says so in words the face never shows (k_aegis_display uses v.headline, not pa_band_advice).

### [high] ux — Ring: the refused toggle is reported only to the serial log, and the row expansion invites the press that will be refused
`components/pharos_lens_ring/lens_ring.c:148`

**Wrong.** k_ring_edit returns true — which the UI reads as 'something changed' — even when pharos_ui_ring_toggle refuses because this is the last armed watch. The comment says 'Say so rather than letting the press look broken', but the only saying-so is ESP_LOGI, which goes to USB. Nothing on the glass changes. Worse, k_ring_expand's sub 3 tells the operator to do exactly the thing that will be refused, because it derives its text from `armed` alone with no knowledge of whether this is the last one.

**Evidence.**
```c
    if (!pharos_ui_ring_toggle(i)) {
        /* Refused: this is the last armed watch, and a ring watching nothing
         * would be a state reached through the ring with no obvious way back.
         * Say so rather than letting the press look broken. */
        ESP_LOGI(TAG, "ring: %u is the last watch armed; leaving it on", i);
    }
    return true;

and the expansion that recommends it:
    case 3:
        snprintf(out->left, sizeof(out->left), "press again to");
        snprintf(out->right, sizeof(out->right), "%s",
                 armed ? "switch off" : "switch on");
```

**Trigger.** Hold anywhere on the home ring to open Ring, switch every watch off until one remains, open that last row: it says 'press again to / switch off'. Press the centre. The row does not change, no toast appears, and the only explanation goes out the USB port to a laptop nobody is holding.

**Impact.** The one control on this page that can refuse is the one that refuses silently. The operator's model becomes 'the ring page is unreliable', which is expensive on the page that decides what the device watches — and the expansion actively told them the press would work.

**Fix.** Have k_ring_edit call pharos_hud_toast("last watch stays on") on refusal (the UI already does this for 'radio locked' and 'would not start' at pharos_ui.c:1198 and 1208), and return false so the UI does not report a change that did not happen. In k_ring_expand sub 3, when this is the only armed watch, show 'the last one' / 'stays on' at PHAROS_TONE_LIMIT instead of inviting the press.

### [high] ux — Locate's rows print 0 dBm and a trend word when there is no target, and spend verdict amber on a permanent property of the instrument
`components/pharos_lens_locate/lens_locate.c:230`

**Wrong.** k_locate_row reads `have` at line 204 and consults it only in case 0. Cases 1 through 7 render the zeroed verdict unconditionally: channel 0, the trend word for whatever pl_evaluate_at left (PL_TREND_STEADY with no samples), and three separate rows of '0 dBm' — which on this scale is the strongest signal the instrument can report. Case 6 paints '0%' confidence amber. Case 8 is a permanent statement about the instrument ('distance / not shown') drawn in PHAROS_TONE_WARN, the verdict colour, when PHAROS_TONE_LIMIT exists in this codebase for exactly that and is used for the same kind of row in lens_rival.c:445, lens_vigil.c:393 and lens_system.c:386. Case 2 paints the trend amber whenever locked, which contradicts this lens's own display comment that 'getting nearer is the goal, not a finding'.

**Evidence.**
```c
    case 3:
        snprintf(out->left, sizeof(out->left), "signal now");
        snprintf(out->right, sizeof(out->right), "%d dBm", (int)v.rssi_now);
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    ...
    case 8:
        /* Said out loud, because it is the single most misread thing here. */
        snprintf(out->left, sizeof(out->left), "distance");
        snprintf(out->right, sizeof(out->right), "not shown");
        out->tone = PHAROS_TONE_WARN;
        return true;
```

**Trigger.** Start Locate without handing it a target (its live face correctly says 'NO TARGET'), then press the bottom strip to open the detail page. It reads: target none / channel 0 / trend STEADY / signal now 0 dBm / smoothed 0 dBm / best seen 0 dBm / confidence 0% amber / samples 0 / distance not shown amber.

**Impact.** Three rows claim a 0 dBm signal for a transmitter that was never selected, and 0 dBm is the top of the scale rather than the bottom of it. 'trend STEADY' is a judgement about nothing. And the amber on 'distance / not shown' is the case pharos_lens.h's PHAROS_TONE_LIMIT comment was written about verbatim — a condition that can never change, taking a colour that is supposed to mean 'worth knowing about the air'.

**Fix.** Return false from k_locate_row for every index above 0 while s_has_target is false, so the page is one honest row instead of eight invented ones. Change case 8's tone to PHAROS_TONE_LIMIT. Change case 2's tone to track the trend's meaning (BAD for GONE QUIET, GOOD for HERE/WARMER, DIM otherwise) rather than to be amber whenever locked. Add a row for v.silent_us — how long since the target last spoke — which the engine computes and nothing shows.

### [high] dead-feature — struct pharos_lens_display::raw_score is filled by four lenses and read by nothing
`components/pharos_core/include/pharos_lens.h:48`

**Wrong.** The display contract documents raw_score as 'what the evidence earned BEFORE the caps', and lens_rival.c:351, lens_ward.c:491, lens_watch.c:392 and lens_whisper.c:229 all fill it. pharos_hud_live reads big, band, detail, advice, why, score, ceiling, has_score, has_alert, alert, families, fam_label, history, has_history and simulated — and never raw_score. There is no reference to raw_score anywhere under components/pharos_ui/ or main/.

**Evidence.**
```c
    uint8_t score;    /* 0..100, drives the gauge and its colour    */
    uint8_t raw_score;/* what the evidence earned BEFORE the caps   */
    uint8_t ceiling;  /* the most this observation could have earned*/

and the only place the ceiling and score are drawn:
    if (d->has_score) {
        if (d->ceiling > 0 && d->ceiling < 100) {
            snprintf(num, sizeof num, "%u / %u", d->score, d->ceiling);
        } else {
            snprintf(num, sizeof num, "%u", d->score);
```

**Trigger.** Run Watch with a verdict whose raw score is cut by the ceiling — 85 raw at a ceiling of 45. The face shows '45 / 45'. The fact that the evidence earned 85 and a thin sweep took 40 of it away reaches the glass only through Watch's own row 14 ('earned / allowed'), and not at all for Rival, Ward or Whisper, which have no equivalent row.

**Impact.** The distance between what was earned and what was allowed is the single most important disclosure in a confidence-ceiling system, and the display contract that promises to carry it drops it on the floor. It also means the fix proposed in docs/lens-audit.md's squall section — 'raw_score is computed, has a display slot, and never reaches the glass' — would be a no-op if implemented as written: filling the slot changes nothing until the HUD reads it.

**Fix.** Either render it — when raw_score > score, append the earned figure next to the '%u / %u' at pharos_hud.c:1456 or put it in the why line — or delete the field and the four assignments, so a maintained field always has a reader. Rendering is the better half: it is the disclosure the honesty rules exist to make.

### [medium] ux — Sentinel names no network, Roster tells the operator to tap a control that is inert, and Footprint's drill marks nothing on the detail page
`components/pharos_lens_sentinel/lens_sentinel.c:336`

**Wrong.** Three smaller surface gaps of the same shape. (a) Sentinel's file comment says "'Six findings' is not actionable; 'one network downgraded its security' is" — and then its last row is exactly 'findings total / %u'. ps_verdict_t::findings[] carries a BSSID, SSID, severity and before/after grade for each, is written to the JSON report at lens_sentinel.c:246-258, and reaches no row; the lens has no row_expand. Its live-face advice when un-adopted is 'no baseline yet - console: sentinel adopt', and pharos_lens_sentinel_adopt's only caller is console_glue.c:122, so the lens has no control for the action it instructs. (b) Roster's advice says 'tap for the list', but Roster declares no on_select, and a centre tap in VIEW_LIVE on a lens without one silently does nothing (pharos_ui.c:1186-1191, no toast); the list is on the bottom strip. (c) k_footprint_display sets o->simulated for a drill and the HUD honours it on the live face only (pharos_hud.c:1467) — paint_detail never receives the display struct, and k_footprint_row emits no drill marker, so a drill's rows are indistinguishable from measurements of the room.

**Evidence.**
```c
    case 6:
        snprintf(out->left, sizeof(out->left), "findings total");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_findings);
        out->tone = PHAROS_TONE_DIM; return true;

    snprintf(o->advice, sizeof(o->advice), "%s",
             s_baseline.adopted ? v.headline : "no baseline yet - console: sentinel adopt");

and in lens_roster.c:
        snprintf(o->advice, sizeof(o->advice), "%.40s - tap for the list",
                 worst.vendor ? worst.vendor : rd_class_name(worst.klass));
```

**Trigger.** (a) Adopt a baseline over USB, change a network's security, open Sentinel's detail page: it says 'security dropped 1' and there is no way to learn which network. With no baseline, the face tells you to run a console command the device offers no way to run. (b) Open Roster with an exposed device in view; the advice reads '<vendor> - tap for the list'; press the 190x190 centre zone — nothing happens. (c) Press Footprint's centre to start a drill, then open the detail page: 'camped defender 88 / stealth gap 40 / forged identity loudest' with nothing saying it is synthetic.

**Impact.** (a) The lens states its own principle and then breaks it — a count you cannot open is the claim-without-evidence this project refuses everywhere else — and its only remedy is unreachable from the glass. (b) The most prominent touch target on a 466 px round screen is the one the advice appears to mean and the one that does nothing. (c) pharos_lens.h calls a drill mistaken for an incident 'the worst failure this project can have'; the live face guards against it and the detail page does not.

**Fix.** (a) Add a row_expand to Sentinel that walks v.findings[] — ps_change_name(fi->change), the SSID, and 'was %u now %u' — and make the adopt reachable with a row_edit on a 'baseline' row (Twin needs the same for pharos_lens_twin_adopt_profile, which is not reachable from the glass either). (b) Change Roster's advice to name the real control, e.g. '%.40s - open the list below', or give Roster an on_select that sets the detail view. (c) Have k_footprint_row emit row 0 as 'DRILL - not this room' / pr_scenario_name(sc) at PHAROS_TONE_LIMIT whenever s_drill is set, and shift the other rows down.


## parsers

**Coverage.** Read in full: components/pharos_radio/pharos_radio.c (786 lines), components/pharos_engine/pharos_dot11.c (346 lines), components/pharos_core/include/pharos_event.h (243 lines). Also read in full as the callee of the suspect call site: components/pharos_engine/pharos_wps.c (~240 lines) and components/pharos_engine/include/pharos_dot11.h. Checked every consumer of the non-NUL-terminated pharos_ev_dot11_t::ssid (lens_ward.c:133-138, lens_roster.c:102-106, lens_probe.c:88-92, lens_watch.c:404) - all clamp against ssid_len, so that is sound by design, not a finding. Checked existing host coverage: test/host/test_pharos.c:40-75 and :340-375 (find_ie/find_ie_from/rsn truncation sweep), test/host/test_harvest.c:85-142 (EAPOL truncation sweep), test/host/test_wps.c:120-190 (pwps_parse overrun + byte-by-byte truncation sweep). Grepped every use of `blen` in the promiscuous callback (radio.c lines 283, 308, 324, 328, 354, 389, 391, 419, 431) - line 354 is the only one that does raw arithmetic instead of passing the offset to a bounds-checking walker. Built an ASan probe at /private/tmp/pharos_probe/probe.c against the real components/pharos_engine/pharos_wps.c and reproduced the overflow.

### [high] vulnerability — size_t underflow in the promiscuous callback hands pwps_parse a 65528-byte length and a pointer past the frame (remote OOB read from a 28-byte beacon)
`components/pharos_radio/pharos_radio.c:354`

**Wrong.** The management-frame block is entered on `(size_t)len > 24u`, so `blen` can be as small as 1. Every other element walk in that block passes its start offset into a walker that guards it (`pharos_dot11_find_ie_from` and `pharos_dot11_ie_count` both begin `if (!body || len < start) return ...`), so a body shorter than the 12-byte fixed-parameter block stops them cleanly. The WPS call is the one that does the arithmetic itself: `blen - 12u` is unsigned, so for blen in 1..11 it wraps to a huge size_t, the truncation to uint16_t turns it into 65525..65535, and `body + 12u` already points past the end of the driver's RX buffer. pwps_parse then walks up to ~64 KB of whatever follows the Wi-Fi RX buffer in internal RAM. There is no `blen >= 12` guard anywhere on the path: the only preceding conditions are `has_fixed`, `!already` (always true for a BSSID not yet in s_wps_seen) and `s_wps_n < 24u`.

**Evidence.**
```c
    if (ev.u.dot11.type == PHAROS_FT_MGMT && (size_t)len > 24u) {
        const uint8_t *body = payload + 24;
        const size_t blen = (size_t)len - 24u;
...
            if (!already && s_wps_n < 24u) {
                pwps_info_t wi;
                if (pwps_parse(body + 12u, (uint16_t)(blen - 12u), &wi) &&
                    wi.present) {
```

**Trigger.** Transmit a single runt beacon on any channel the scanner is dwelling on, from a previously unseen source address. Frame bytes (28 total, which is what rx_ctrl.sig_len reports for a 24-byte MAC header plus FCS):
  80 00                                 frame control: type=MGMT(0), subtype=BEACON(8)
  00 00                                 duration
  ff ff ff ff ff ff                     addr1 broadcast
  de ad be ef 00 01                     addr2 (any address not yet in s_wps_seen)
  de ad be ef 00 01                     addr3
  00 00                                 seq/frag
  <4 bytes FCS>
That gives len=28, blen=4, and `(uint16_t)(blen - 12u)` == 65528 with the read starting at payload+36, i.e. 8 bytes past the end of a 28-byte frame. Any beacon or probe-response with sig_len in 25..35 does it. Reproduced verbatim (same expression, same pwps_parse) under AddressSanitizer at /private/tmp/pharos_probe/probe.c:
  blen = 4
  (uint16_t)(blen - 12u) = 65528
  ==39561==ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 1
      #0 pwps_parse pharos_wps.c:163
  0x603000001d74 is located 8 bytes after 28-byte region
Build line: cc -std=c11 -g -fsanitize=address -Icomponents/pharos_engine/include -Icomponents/pharos_core/include probe.c components/pharos_engine/pharos_wps.c -o probe

**Impact.** An out-of-bounds read of up to ~64 KB past the Wi-Fi driver's RX buffer, from inside the promiscuous callback, triggered by one frame any attacker can transmit. On ESP32-S3 the practical outcome is a LoadProhibited / InstrFetchProhibited panic and reboot the moment the walk crosses an unmapped page - a one-frame remote denial of service against a detector whose whole job is to keep listening while someone is attacking the air, and it is exactly the frame an attacker would send while doing something else. Secondarily, if the walk happens to find a 221 element with the 00:50:F2:04 OUI in the memory past the buffer, pwps_parse_attrs will copy adjacent RAM through copy_printable into pharos_ev_wps_t::vendor/model, which is then drawn on the glass and written into reports - adjacent-memory disclosure, and a fabricated access-point model attributed to a real BSSID.

**Fix.** Guard the call the same way the walkers guard themselves - the fixed-parameter block must actually be present before you can skip it:

            if (!already && s_wps_n < 24u && blen >= 12u) {
                pwps_info_t wi;
                if (pwps_parse(body + 12u, (uint16_t)(blen - 12u), &wi) &&

Cheaper still and harder to regress: hoist `const size_t ie_off = has_fixed ? 12u : 0u;` next to `blen` at line 283-284 and wrap the whole `if (has_fixed)` block in `if (has_fixed && blen >= 12u)`, so no code inside it can ever do unguarded `blen - 12`. Either way the runt beacon then falls out with no WPS event, which is the correct answer: a 28-byte beacon carries no elements to read.

The defect is in the ESP-IDF glue, which is not host-compiled, so a host test cannot reach it directly. The regression that CAN be written belongs in test/host/test_wps.c next to the existing overrun cases: assert that pwps_parse refuses a length it cannot possibly have been given legitimately, and/or factor the body/offset computation out of promisc_cb into a small pure helper in pharos_dot11.c (e.g. `bool pharos_dot11_body(size_t len, size_t fixed, size_t *out_off, size_t *out_len)`) that the host suite can drive across every len in 24..64.


## uiloop — the UI state machine in components/pharos_ui/pharos_ui.c (navigation, lens lifecycle, watchtower rotation, UI↔analytics concurrency)

**Coverage.** Read components/pharos_ui/pharos_ui.c in full, top to bottom (2208 lines, md5 33a729518772bf1da455072567d753c0 — note another agent added the pharos_batt_mode_t API to this file and to pharos_ui.h WHILE I was reading; all line numbers and quotes below are against that current on-disk state, re-verified after the change). Supporting reads: components/pharos_ui/pharos_hud.c (touch dispatch 534-630, zones 1072-1110, hud_rebuild 1122-1135, home paint/hit 1290-1325, battery 1560-1620, stub block 1841-1875 — ~450 lines), components/pharos_ui/include/pharos_hud.h, pharos_ui.h, pharos_style.h (radii), pharos_round.h/.c (full), components/pharos_engine/include/pharos_tower.h (full) and pharos_tower.c (ptw_turn 177-260, ptw_set_armed 262-300, ptw_summarise 332-410), components/pharos_lens_ring/lens_ring.c (full, 251 lines), components/pharos_lens_system/lens_system.c (rows 224-340, row_edit 470-560, expand 560-600), components/pharos_core/pharos_lens.h + pharos_lens.c (registry bound), components/pharos_lens_watch/lens_watch.c (locking contract only), main/console_glue.c (glue_activate/deactivate 74-90, cli_ring 541-575), components/pharos_engine/pharos_console.c (cmd_stop 376-383), test/host/Makefile (pharos_ui.c is NOT in the host build — only pharos_theme/style/dial/round are, so nothing in test/host exercises this state machine; test_tower.c and test_ring.c cover the engine and the label geometry underneath it, not the view logic). Two probes compiled and run under /private/tmp/pharos_probe against the real pharos_tower.c and pharos_round.c; both reproduced.

### [high] bug — paint_home zeroes worst_score for "NOT WATCHING" and the very next line puts it back
`components/pharos_ui/pharos_ui.c:669`

**Wrong.** The PAUSED-IS-NOT-QUIET block deliberately sets h.worst_score = 0 so a stopped device does not draw a live-looking severity arc. The unconditional assignment on the next line overwrites it with the last reporting watch's score. h.worst_score drives the rim arc (pharos_hud.c:1296 set_arc_value(s_h_ring, h->worst_score)), so the face reads "NOT WATCHING" in amber while the rim still shows the score of a watch that has stopped listening. This is the exact comfortable-lie the surrounding comment says it is defending against, and the defence is a dead store. Note ptw_summarise sets worst_index even for an all-quiet ring (w->state > out->worst with worst == PTW_UNKNOWN), so worst_index >= 0 is the normal case, not the exceptional one.

**Evidence.**
```c
    if (!s_tower_on && s_tower.n) {
        h.headline = "NOT WATCHING";
        h.worst_state = 2u; /* amber: this is a thing to notice, not an alarm */
        h.worst_score = 0;
    }
    h.worst_score = (sum.worst_index >= 0) ? s_tower.w[sum.worst_index].score : 0;
```

**Trigger.** Reproduced on the host against the real pharos_tower.c (/private/tmp/pharos_probe/probe.c): arm wifi.watch/wifi.karma/wifi.census, ptw_turn once, ptw_report("wifi.watch", PTW_NOTED, score 41, ceiling 60), then run the two quoted statements with tower_on = 0. Output: "A) paused face: headline=NOT WATCHING worst_state=2 worst_score=41". On the device: boot, let the ring report once, then pause the rotation (hold on home -> Ring page, or `lens sys.census` on the console) and look at the rim arc.

**Impact.** The home face contradicts itself while paused: the words say nothing is being watched, the rim arc says 41/100 of something. An absence-based claim ("we are not listening") is drawn alongside stale positive evidence rendered as if current.

**Fix.** Move the unconditional assignment above the !s_tower_on block, or guard it: `if (s_tower_on) h.worst_score = (sum.worst_index >= 0) ? s_tower.w[sum.worst_index].score : 0;` — keeping the zero the paused branch already sets.

### [high] bug — The Ring page's "rotation" row hands the page away in the same tick it is pressed
`components/pharos_ui/pharos_ui.c:1653`

**Wrong.** Two halves of one gap: the rotation state and the view/lens lifecycle are never reconciled.

Turning it ON: lens_switch() pauses the tower whenever a lens is chosen by hand, so the Ring page ALWAYS opens with row 0 reading "PAUSED". Pressing that row calls pharos_ui_ring_set_running(true) from row_apply(); tower_rotate() then runs later in the SAME UI-loop iteration, ptw_turn reports changed=true immediately (handover_us is stale from before the pause, so slice_done is true on the first call), and s_tower_pending is switched to a watch. s_view is left at VIEW_DETAIL, so paint_detail() now renders that watch's rows under the header of the settings page the operator is standing on. The settings page is not closed, it is silently re-pointed at a different lens, and the next row the operator taps edits that lens instead.

Turning it OFF: pharos_ui_ring_set_running(false) calls lens_halt(), which deactivates the active lens — which, on this page, IS sys.ring. s_view stays VIEW_DETAIL, pharos_lens_active() becomes NULL, and paint_detail(NULL) falls through every `active && active->row` guard to render an empty page with an empty header.

**Evidence.**
```c
void pharos_ui_ring_set_running(bool on)
{
    s_tower_on = on;
    if (!on) {
        lens_halt();
    }
    ESP_LOGI(TAG, "watchtower %s", on ? "running" : "paused");
}

/* and the caller, components/pharos_lens_ring/lens_ring.c:138 */
static bool k_ring_edit(unsigned index)
{
    if (index == RING_ROW_RUN) {
        pharos_ui_ring_set_running(!pharos_ui_ring_running());
        return true;
    }

/* and the loop order, pharos_ui.c:2135-2160 */
        nav_apply();
        row_apply();
        home_apply();
        request_apply();
        tower_rotate();
        if (s_tower_pending) {
```

**Trigger.** Reproduced on the host against the real pharos_tower.c (/private/tmp/pharos_probe/probe.c, part B): with the tower armed and paused for 40 s, setting tower_on=1 and calling ptw_turn once returns changed=1, next=wifi.karma — i.e. the switch happens on the first call after the toggle, inside the same iteration as row_apply(). On the device: from the home ring, press and hold anywhere -> the Ring page opens with "rotation  PAUSED" -> tap that top row once. The page stays up but its rows become KARMA's (or whichever watch is next), and the "rotation" row is gone.

**Impact.** The one control the code added specifically so the pause would be "visible and reversible" (lens_ring.c:15-18) is neither: pressing it destroys the page it lives on, so the operator cannot see the result and cannot press it again to undo. The reverse press blanks the page instead.

**Fix.** Make the Ring page survive the rotation it controls. Cheapest correct form: have the rotation's switch leave the view alone only when the operator is not inside a lens page — i.e. in the `if (s_tower_pending)` block, if s_view is VIEW_DETAIL or VIEW_OPENED and the active lens is the one being replaced, drop back to VIEW_HOME so the operator lands somewhere that matches what is running. Better: give sys.ring a `.caps`-free exemption so a hand-opened SYSTEM lens suppresses tower_rotate's switch entirely while its page is up, and make set_running(false) not call lens_halt() when the active lens is the one asking.

### [high] dishonesty — Console `stop` prints "stopped" but the watchtower restarts a lens on the next tick
`components/pharos_ui/pharos_ui.c:761`

**Wrong.** request_apply()'s stop branch halts the lens and drops to VIEW_BROWSE, but never clears s_tower_on. The lens branch immediately below it does exactly that, with a comment explaining why omitting it made `lens` "effectively unusable" — the same reasoning applies verbatim to `stop` and was not applied. With the rotation running (the boot default), tower_rotate() re-activates a watch within one 50 ms tick, while cmd_stop has already printed "stopped" to the operator. The screen is additionally left in VIEW_BROWSE showing the browse card for s_cursor, which is not the lens that is now holding the radio.

**Evidence.**
```c
    if (s_req_stop) {
        s_req_stop = false;
        lens_halt();
        s_view = VIEW_BROWSE;
        paint_browse();
        return;
    }

/* ... the very next branch, which DOES do it: */
    if (lens_switch(id)) {
        /* CHOOSING A LENS BY HAND STOPS THE ROTATION.
         *
         * Same rule as tapping a dot on the ring ... The console path was
         * missing it, which made `lens` effectively unusable while the
         * watchtower was running */
        s_tower_on = false;
        s_tower_pending = NULL;

/* and components/pharos_engine/pharos_console.c:376 */
static void cmd_stop(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    if (ops->deactivate) {
        ops->deactivate();
    }
    pc_println(out, "stopped");
}
```

**Trigger.** Boot the device with a clean fence (s_tower_on = s_fence_ok && s_tower.n > 0, pharos_ui.c:2137). On the USB console type `stop`; it answers "stopped". Immediately type `tower`: the dump reads "watchtower: running" with a cursor on an armed watch, and `status` names a live lens. The radio never stopped.

**Impact.** A receive-only monitor telling the operator the radio is stopped when it is not is the one class of statement this project refuses everywhere else. It also makes `stop` useless as the way to quiesce the device before measuring power or before a lens-level test.

**Fix.** Add `s_tower_on = false; s_tower_pending = NULL;` to the stop branch, matching the lens branch below it, and land on VIEW_HOME rather than VIEW_BROWSE so the face says NOT WATCHING (which would then be true) instead of showing a browse card for a lens nobody selected.

### [high] dead-feature — The watchtower acknowledge gesture is unreachable in the middle of the ring — the CORE hit test swallows it
`components/pharos_ui/pharos_ui.c:1074`

**Wrong.** nav_apply()'s VIEW_HOME / PHAROS_NAV_SELECT branch is the ONLY caller of ptw_acknowledge() in the firmware, and its comment says the gesture is "a press in the middle of the ring". But pharos_hud.c's nav_event() intercepts the press first: anything with radius < PR_CORE_R (90 px) is routed to s_home_cb(PHAROS_HUD_HOME_CORE), and home_apply() handles that by unconditionally opening sys.survey — it never consults the latch. The NAV_SELECT zone is a 190x190 square (mk_zone(scr, 190, 190, 0, 0)), so the circular middle of that square never reaches the acknowledge code; only the four corner slivers do. Two handlers claim the same gesture and the geometry decides, silently, in favour of the one that does not acknowledge.

**Evidence.**
```c
        case PHAROS_NAV_SELECT: {
            /* A press in the middle of the ring, with something remembered,
             * means "I have seen it". Only a person can clear a latched
             * finding - time passing is not somebody having looked. */
            ptw_summary_t ack;
            ptw_summarise(&s_tower, (uint64_t)esp_timer_get_time(), &ack);
            if (ack.latched_index >= 0 && ack.worst < PTW_ELEVATED) {
                ptw_acknowledge(&s_tower);

/* home_apply(), pharos_ui.c:836 - no latch check at all */
    if ((unsigned)want == PHAROS_HUD_HOME_CORE) {
        if (lens_switch("sys.survey")) {

/* pharos_hud.c:577 - the interception */
    if (s_current == PAGE_HOME && s_home_cb) {
            const int hit = pharos_hud_home_hit((int16_t)p.x, (int16_t)p.y);
            if (hit >= 0) { s_home_cb((unsigned)hit); return; }
            if (pr_radius_of((int16_t)p.x, (int16_t)p.y) < PR_CORE_R) {
                s_home_cb(PHAROS_HUD_HOME_CORE); return;
            }
```

**Trigger.** Reproduced numerically against the real pharos_round.c (/private/tmp/pharos_probe/centre.c), mirroring nav_event()'s dispatch over every pixel of the 190x190 centre zone: 25433 px (69.7%) route to HOME_CORE -> sys.survey, 11048 px (30.3%) route to NAV_SELECT, and those reach the acknowledge code only at radius 90..134 from centre — the corners of an invisible square. On the device: let a watch latch a finding (the sub-line reads e.g. "WATCH 4m ago"), then tap the dead centre of the ring. The Survey opens; the latch is still there when you come back. Only a tap in a diagonal corner, ~95 px out from centre and off any dot, clears it.

**Impact.** A latched finding is designed to be clearable only by a person, and the person's control is hidden behind geometry. The face keeps showing an old ALERT indefinitely while the only visible thing the middle does is open the Survey. The bottom-of-file guide also teaches this gesture as "Tap the middle to run the tool you are on" (k_guide step 2), which is a third meaning the middle does not have on HOME.

**Fix.** Decide in one place. Put the latch check inside home_apply()'s PHAROS_HUD_HOME_CORE branch, ahead of the sys.survey switch: if ack.latched_index >= 0 && ack.worst < PTW_ELEVATED, acknowledge and toast; otherwise open the Survey. Then the NAV_SELECT copy in nav_apply can drop its duplicate ack block and just open s_home_sel, and one press in the middle means one thing.

### [high] bug — Battery and charging state only reach the glass from the HOME and GUIDE paints
`components/pharos_ui/pharos_ui.c:502`

**Wrong.** The battery arc and its text are persistent chrome, created on the screen root in pharos_hud_create() (pharos_hud.c:668-676) and therefore visible on every page. But batt_apply() — the only thing that pushes a new reading into them — is called from exactly two places: paint_home() (line 524) and paint_guide() (line 971). The VIEW_LIVE and VIEW_DETAIL/VIEW_OPENED branches of paint(), and paint_browse(), all call pharos_hud_create() and theme_sync() but never batt_apply(). So the moment the operator enters a lens — which is where a monitoring device spends its life — the charge indicator freezes at whatever was last painted on home, and plugging in the charger changes nothing on screen. The newly added "battery on screen" setting (ROW_BATTERY in lens_system.c, which calls pharos_ui_batt_mode_next -> pharos_hud_battery_mode) makes this worse: that row lives on a VIEW_DETAIL page, pharos_hud_battery_mode() only sets s_batt_mode and clears the dirty key, and the repaint it arms cannot fire until the operator navigates all the way back to HOME. Setting it to "always" produces no visible change on the page where you set it.

**Evidence.**
```c
static void batt_apply(void)
{
    if (s_batt_ok) {
        pharos_hud_battery(s_batt.soc_pct, s_batt.charging, s_batt.present);
    }
}

/* paint(), the DETAIL/OPENED branch - line 1888 */
    if (s_view == VIEW_DETAIL || s_view == VIEW_OPENED) {
        if (!pharos_bsp_display_lock(PHAROS_PAINT_LOCK_MS)) {
            s_paint_misses++;
            return;
        }
        s_paints++;
        pharos_hud_create();
        theme_sync();
        paint_detail(active);
        pharos_bsp_display_unlock();
        return;
    }

/* the LIVE branch, line 1910 - same omission */
    pharos_hud_create();
    theme_sync();
```

**Trigger.** Open any lens (tap a dot on the ring), then plug in USB power. The rim arc keeps its old colour and level and the "n%% CHG" text never appears; the device only admits it is charging if you press and hold back to HOME. Second trigger: `lens sys.system` on the console, go to the detail page, tap the "battery on screen" row until it reads "always" — nothing appears on the glass until you leave the page.

**Impact.** The battery indicator answers "is it charging" and "how full" only on a screen the operator is not looking at, and the setting that controls it gives no feedback at the moment it is changed, which reads as a broken control.

**Fix.** Call batt_apply() from every paint that holds the display lock: add it after theme_sync() in the VIEW_DETAIL/VIEW_OPENED branch and in the VIEW_LIVE path of paint(), and add it to paint_browse() after pharos_hud_create(). It is a dirty-checked no-op when the reading has not moved (pharos_hud.c:1587 `if (key == s_batt_last) return;`), so the cost is one comparison per frame.

### [high] bug — theme_sync's hud rebuild wipes the battery chrome and the hud's dirty cache keeps it hidden
`components/pharos_ui/pharos_ui.c:1859`

**Wrong.** theme_sync() calls pharos_hud_rebuild(), which does lv_obj_clean(scr) and re-creates every widget — including s_batt_track / s_batt_fill / s_batt_txt, which pharos_hud_create() leaves hidden (show(..., false), pharos_hud.c:670-676). theme_sync knows the rebuild destroys state and carefully re-establishes the two things it owns (s_nav_pending, s_row_pending), but not the battery. And pharos_hud.c's s_batt_last is a static that the rebuild does not reset, so the next pharos_hud_battery() call with an unchanged percentage returns early at `if (key == s_batt_last) return;` and never re-shows the widgets. The indicator therefore disappears from the glass and stays gone until the SoC changes by a whole percent or the charger is plugged in — potentially many minutes on a steady battery.

**Evidence.**
```c
    if (!first) {
        /* The browse card is painted on view entry rather than per frame, and
         * a theme can only be changed from a lens' detail page, so by the time
         * anyone gets back to BROWSE it has been repainted anyway. */
        pharos_hud_rebuild();
        /* And nothing the teardown raised is a real intent. Tearing the face
         * down and building it again is not a thing a finger did. */
        s_nav_pending = -1;
        s_row_pending = -1;
    }

/* pharos_hud.c:1122 - the rebuild, which resets s_aura_rgb and s_zones_detail
   but not s_batt_last */
void pharos_hud_rebuild(void)
{
    if (!s_built) return;
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;
    const hud_page_t was = s_current;
    lv_obj_clean(scr);
    s_built = false;
    s_aura_rgb = 0xFFFFFFFFu;
    s_zones_detail = -1;
```

**Trigger.** `lens sys.system` on the console (or open Settings from the browser), go to the detail page, tap the "theme" row once to change theme. Then press and hold twice to get back to HOME: the rim battery arc is gone. It stays gone until the charge level moves a percent.

**Impact.** Changing the theme silently removes the charge indicator, which is exactly the kind of loss nobody connects back to the action that caused it. It also compounds finding 5: the two controls that touch the battery display both live on the same settings page.

**Fix.** Two lines, either side. In pharos_hud_rebuild(), add `s_batt_last = -2;` beside the other statics it resets — a rebuilt face has no battery drawn, so the cache must not claim it has. In theme_sync(), call batt_apply() after pharos_hud_rebuild() so the value is back on the glass in the same frame rather than at the next home paint.

### [medium] bug — paint_browse gives up silently on a missed display lock and nothing ever retries it
`components/pharos_ui/pharos_ui.c:744`

**Wrong.** VIEW_BROWSE is the only view painted on entry rather than per frame — paint() returns immediately for it ("the browse card is painted when the cursor moves"). paint_browse() takes the display lock with a 70 ms timeout and, on failure, just returns: it neither retries nor increments s_paint_misses. Entering BROWSE is a full page change, which is precisely the case PHAROS_PAINT_LOCK_MS's own comment says can hold the lock longer than one paint period ("that invalidates the whole 466x466 surface, and the arriving page's headline fades in on top of it"). If that acquire fails, s_view is BROWSE but the glass still shows the home ring or the previous lens face, and there is no code path that will ever repaint it — the side zones now step an invisible cursor and the centre launches whatever lens it happens to be on. Every other view counts its misses so the heartbeat log can diagnose a blank panel; this one cannot, which is the detector-blind-to-its-own-instrumentation shape the project calls out elsewhere.

**Evidence.**
```c
static void paint_browse(void)
{
    if (!s_order_n || !pharos_bsp_display_lock(PHAROS_PAINT_LOCK_MS)) {
        return;
    }
    pharos_hud_create();
    const pharos_lens_t *l = s_order[s_cursor % s_order_n];

/* and paint(), line 1885 */
    if (s_view == VIEW_BROWSE) {
        return; /* the browse card is painted when the cursor moves */
    }
```

**Trigger.** From the home ring, tap the bottom strip (PHAROS_NAV_DETAIL -> s_view = VIEW_BROWSE; paint_browse()) at a moment when LVGL's timer task is compositing a ring transition under radio load — the same 70+ ms hold the PHAROS_PAINT_LOCK_MS comment documents. The screen keeps showing the ring; the device is in BROWSE. Deterministically forceable on the bench by lowering PHAROS_PAINT_LOCK_MS to 1 and tapping the bottom strip.

**Impact.** A recoverable dropped frame everywhere else becomes an unrecoverable wrong-screen state here: the operator is looking at one page and pressing another, and the miss is invisible in the heartbeat counters that exist to catch exactly this.

**Fix.** Count the miss and let the loop retry. Add `s_paint_misses++;` to the failure path, and have paint() repaint BROWSE when the last attempt failed — e.g. a static `s_browse_dirty` set by every entry into VIEW_BROWSE and cleared only by a successful paint_browse(), with paint()'s VIEW_BROWSE branch calling paint_browse() while it is set instead of returning unconditionally.

### [medium] bug — s_row_pending and s_home_tap cross tasks without volatile, while s_nav_pending beside them has it
`components/pharos_ui/pharos_ui.c:251`

**Wrong.** All three intent slots are written from LVGL's task (hud_row_cb, hud_home_cb, on_nav) and read-and-cleared from the UI task on the other core — the file's own comment on s_row_pending says so explicitly, "same as s_nav_pending". But only s_nav_pending is declared volatile. The same is true of s_home_tap (line 190). s_req_stop and s_analytics_run elsewhere in the file are both volatile, so the author's rule is clear and these two were missed. The compiler is free to keep a non-volatile file-static in a register across the loop body; the calls in between (vTaskDelay, ESP_LOGI) make a hoist unlikely today but nothing in the source prevents it, and this is a two-core target with no barriers on these paths.

**Evidence.**
```c
/* A row touched on the glass, 0..ROWS-1 within the page shown, or -1. Filed
 * from LVGL's task and acted on by the UI task, same as s_nav_pending: doing
 * the work in the callback runs it on LVGL's stack and reboots the board. */
static int s_row_pending = -1;
static volatile int s_nav_pending = -1; /* pharos_nav_t, or -1 for none */

/* and line 190 */
static int s_home_tap = -1;         /* a dot touched on the glass, or -1 */
```

**Trigger.** Not reproducible as a runtime failure at the current optimisation level — this is a missing qualifier on a documented cross-task slot, not an observed miss. It becomes reachable under -O2 with LTO if row_apply()/home_apply() are inlined into pharos_ui_run's loop and the compiler decides nothing in the loop can write the static; the symptom would be a row tap or ring-dot tap that never registers until some unrelated call forces a reload.

**Impact.** A dropped touch on the detail page or the home ring with no log line — the class of fault that gets reported as "the glass is unreliable" and cannot be reproduced on demand.

**Fix.** `static volatile int s_row_pending = -1;` and `static volatile int s_home_tap = -1;`, matching s_nav_pending. No other change needed; both are already written-once/read-once int slots.

### [medium] vulnerability — Console ring commands mutate the watchtower on the REPL task with no synchronisation against the UI task
`components/pharos_ui/pharos_ui.c:1620`

**Wrong.** The file's stated doctrine is that the UI task owns the lifecycle and every other task files a request (pharos_ui_request_lens / pharos_ui_request_stop exist for exactly this). The ring-editing API is exempted from it: cli_ring in main/console_glue.c calls pharos_ui_ring_reset(), pharos_ui_ring_toggle() and pharos_ui_ring_cycle_period() directly from the console REPL task, and they write s_tower while the UI task is concurrently reading and writing it in tower_rotate(), paint_home() and ptw_summarise(). The sharpest case is pharos_ui_ring_reset() -> tower_arm_all() -> ptw_reset(&s_tower, 5000), which sets s_tower.n to 0 and then re-arms one watch at a time. ptw_turn() checks `if (!s || !s->n) return -1;` on entry but then does `const unsigned next = (s->cursor + 1u) % s->n;` further down (pharos_tower.c:243); if n drops to 0 between those two points the modulo is a divide by zero, which on ESP32-S3 raises IntegerDivideByZero and panics. The wider case is cosmetic but constant: paint_home() takes `h.label[d] = s_tower.w[i].name` and later indexes s_tower.w[sum.worst_index] using an index computed from a table the console may have rewritten in between.

**Evidence.**
```c
void pharos_ui_ring_reset(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ring_ver");
        ...
    }
    tower_arm_all();
    ESP_LOGI(TAG, "ring: reset to defaults");
}

/* tower_arm_all(), line 315 */
    ptw_reset(&s_tower, 5000);
    for (unsigned i = 0; i < want && s_tower.n < PTW_MAX_WATCHES; i++) {

/* main/console_glue.c:542, on the REPL task */
static int cli_ring(int argc, char **argv)
{
    const unsigned n = pharos_ui_ring_count();
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        pharos_ui_ring_reset();
```

**Trigger.** Type `ring reset` on the USB console while the watchtower is running. The panic window is the few instructions of ptw_turn between its n-check and its `% s->n`, entered once per 50 ms UI tick, so it is rare rather than reliable; the half-built-ring paint is reachable on any `ring reset` that lands during a home repaint.

**Impact.** A reboot of a monitoring device triggered by an operator command, with a backtrace that points at the tower engine rather than at the console that raced it. The engine is pure C and correct; the unsynchronised caller is the defect.

**Fix.** Route these through the same request mechanism as everything else: have cli_ring set a small pending-ring-op slot (reset / toggle i / period i) that the UI task drains in request_apply(), so every write to s_tower happens on the task that reads it. If that is too heavy for the toggle path, at minimum take s_lens_mtx around tower_arm_all()'s ptw_reset-and-rearm and around tower_rotate()'s ptw_turn, which closes the divide-by-zero.

### [high] ux — The k_ring table's own comments state counts that no longer match the table
`components/pharos_ui/pharos_ui.c:347`

**Wrong.** This file treats comments as the specification, and the ring table's block comment now disagrees with the table below it on both numbers. The table holds 16 entries, 11 of them armed by default (counted mechanically from the source). The comment says thirteen are available and ten ship armed, and then cites a spacing measurement — "pd_ring_layout() leaves 21 px between names at ten watches, 12 px at twelve, and 10 px at thirteen" — as the justification for the default, without a figure for either of the counts the code actually produces. The earlier comment in the same block still describes an eight-watch cap and a forty-second lap, which the table also no longer matches. test/host/test_ring.c pins the spacing at every count, so the arithmetic is checked; the prose that explains the choice is not.

**Evidence.**
```c
    /* `on` is whether it ships ARMED. All thirteen are available and one tap
     * away in the Ring lens; ten are on to begin with, because that is what
     * the dial can label comfortably.
     *
     * That number is measured, not chosen by eye: pd_ring_layout() leaves
     * 21 px between names at ten watches, 12 px at twelve, and 10 px at
     * thirteen - which is the point at which two names read as one long word.
     * See test_ring.c, which pins the spacing at every count. Somebody who
     * wants all thirteen can have them, knowing what they are trading. */
    static const struct { const char *id; uint8_t period; bool on; } k_ring[] = {
```

**Trigger.** Mechanical count of the table in the current file: `awk '/k_ring\[\]/,/^    };/' components/pharos_ui/pharos_ui.c` yields 16 entries, 11 marked true and 5 marked false — against "thirteen available" and "ten are on to begin with".

**Impact.** Low operationally — the armed count is also capped by PHAROS_HUD_HOME_MAX (16), which the table exactly reaches, so nothing overflows. But the next person to change this table will trust the measured-spacing justification, which is quoted for counts the ring no longer runs at, and will burn a cycle re-deriving it.

**Fix.** Update the prose to the real numbers (16 registered, 11 armed by default), add the measured spacing at 11 and at 16 from test_ring.c so the justification covers the values actually shipped, and delete the two superseded paragraphs above it that still describe an eight-watch cap.
