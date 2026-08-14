# Hardware reference — Waveshare ESP32-S3-Touch-AMOLED-1.75C

The board Pharos targets, the pin map it uses, and — most importantly — every
constant that is **not yet verified against the schematic**. An unverified pin
that looks confident costs somebody a bring-up session; one marked `VERIFY`
costs nobody anything, so this page is honest about the difference.

## The board

| | |
|--|--|
| MCU | ESP32-S3R8, dual Xtensa LX7 @ 240 MHz, 512 KB SRAM |
| PSRAM | 8 MB octal, stacked |
| Flash | 16 MB |
| Display | 1.75″ AMOLED, **466×466**, driver **CO5300**, QSPI |
| Touch | **CST9217** capacitive, I²C |
| IMU | **QMI8658** 6-axis (accel + gyro), I²C |
| Audio | **ES8311** codec + **ES7210** echo/ADC, dual-mic array, I²S |
| PMU | **AXP2101** |
| Battery | 3.7 V Li, MX1.25 header |
| USB | Type-C |
| Buttons | BOOT + PWR (PWR via AXP2101) |

Sources: Waveshare wiki & "Resources and Documents", the schematic PDF, the
managed component `waveshare/esp32_s3_touch_amoled_1_75c` (`^3.0.0`), and the
board's ESPHome definition (used to cross-check the buses).

## Pin map

> [!IMPORTANT]
> **The Waveshare managed component owns the pin map, and Pharos uses it.**
> `managed_components/waveshare__esp32_s3_touch_amoled_1_75c/include/bsp/esp32_s3_touch_amoled_1_75c.h`
> defines `BSP_I2C_SDA/SCL`, the QSPI display pins, the touch lines and the I2S
> pins for this exact board. Pharos must never define anything in the `BSP_*`
> namespace — an earlier version of `board_pins.h` did, and it collided the
> moment the real BSP was enabled (`error: 'BSP_I2C_SCL' redefined`). Two
> sources of truth for a pin number is one too many.
>
> [`components/pharos_bsp/include/board_pins.h`](../components/pharos_bsp/include/board_pins.h)
> now holds only the board facts the vendor header does not answer, in the
> `PHAROS_BOARD_*` namespace, plus this table for humans to read.

The table below is a **reference**, cross-checked against the ESPHome board
definition and the vendor header.

### Confirmed

| Signal | GPIO | Signal | GPIO |
|--------|-----:|--------|-----:|
| I²C SDA | 15 | I²C SCL | 14 |
| QSPI CS | 12 | QSPI CLK | 38 |
| QSPI D0 | 4 | QSPI D1 | 5 |
| QSPI D2 | 6 | QSPI D3 | 7 |
| LCD reset | 39 | Touch INT | 11 |
| Touch reset | 40 | I²S MCLK | 42 |
| I²S BCLK | 9 | I²S WS | 45 |
| I²S DOUT (codec) | 8 | I²S DIN (mic) | 10 |
| Speaker enable | 46 | AXP2101 addr | 0x34 |

### Open `VERIFY` items

| Item | Assumed | Why it is unverified |
|------|---------|----------------------|
| Touch I²C address | 0x5A | CST9217 typical; confirm on the bus scan. |
| IMU I²C address | 0x6B | QMI8658 is 0x6A/0x6B by SA0 strap. |
| Codec / ADC addresses | 0x18 / 0x40 | ES8311 / ES7210 typical; confirm. |
| LCD TE line | unused | Tearing-effect pin may not be routed. |
| PMU IRQ line | none | May not be broken out to a GPIO. |
| microSD slot | **absent** | The 1.75C wiki summary does not clearly expose one; evidence storage defaults to a flash partition. |
| Discrete RTC | **absent** | No discrete RTC named; AXP2101 + ESP32-S3 RTC assumed, a PCF85063 probed at boot if present. |
| Battery capacity | 500 mAh | MX1.25 pack varies; the power planner treats it as an estimate. |

## Storage

Because a microSD slot is not confirmed, evidence defaults to a flash `evidence`
partition (see [`partitions.csv`](../partitions.csv)) — 12 MB, SPIFFS, kept
separate from the app and NVS so a settings reset never touches evidence and an
evidence wipe never risks the firmware. If a board revision routes SD, set
`BSP_HAS_SD` and the SD pins in `board_pins.h`.

## Radio reality

The ESP32-S3 radio is **2.4 GHz only**. It cannot hear 5 GHz or 6 GHz, where
most modern office and home traffic now lives. This is not a firmware limitation
Pharos can lift; it is the silicon. Every report and the Spectrum lens state it
plainly, and no verdict is allowed to imply coverage the antenna does not have.

One receiver, one antenna: the device hears one channel at a time and hops to
cover the band. That single fact is the origin of the entire confidence-ceiling
model in [DESIGN.md](DESIGN.md) §4.

## Bring-up checklist (milestone M1)

1. `idf.py set-target esp32s3 && idf.py build flash monitor`.
2. Confirm the boot banner lists all lenses and `transmit fence clean`.
3. I²C scan: confirm touch, IMU, PMU (and RTC if present) addresses; update the
   `VERIFY` addresses in `board_pins.h`.
4. Confirm the CO5300 comes up via the Waveshare BSP (`bsp_display_start`).
5. Confirm AXP2101 battery telemetry; wire `pharos_bsp_battery` to real values
   and drop the `estimated` flag once the baseline current is measured.
6. Re-run `tools/check_tx_fence.sh build/pharos.elf` to audit the linked image.

Each confirmed item moves a row from **`VERIFY`** to **Confirmed** above — please
update this table and `board_pins.h` together in the same commit.
