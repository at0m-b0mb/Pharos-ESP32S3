/* Pharos - board facts for the Waveshare ESP32-S3-Touch-AMOLED-1.75C
 *
 * IMPORTANT: this file does NOT define the pin map, and must never define
 * anything in the `BSP_*` namespace.
 *
 * The Waveshare managed component owns that namespace and is the authority:
 *   managed_components/waveshare__esp32_s3_touch_amoled_1_75c/include/bsp/
 *       esp32_s3_touch_amoled_1_75c.h
 * defines BSP_I2C_SDA/SCL, BSP_I2S_*, the QSPI display pins and the touch
 * lines for this exact board. An earlier version of this header duplicated
 * them, which collided with the vendor's definitions the moment the real BSP
 * was enabled ("error: 'BSP_I2C_SCL' redefined"). Two sources of truth for a
 * pin number is one too many - the vendor's wins, and Pharos simply uses it.
 *
 * What lives here instead: the small number of board facts Pharos itself needs
 * that the vendor header does not answer, all in the PHAROS_BOARD_* namespace,
 * plus the reference table below for humans.
 *
 * Board summary (Waveshare wiki, cross-checked against their examples):
 *   MCU     ESP32-S3R8, dual LX7 @240MHz, 512KB SRAM
 *   PSRAM   8 MB octal (needs the cache/XIP settings in sdkconfig.defaults)
 *   Flash   16 MB on the boards we target; some revisions ship 32 MB
 *   Display 1.75" AMOLED, 466x466, driver CO5300, QSPI
 *   Touch   CST9217 capacitive, I2C
 *   IMU     QMI8658 6-axis, I2C
 *   Audio   ES8311 codec + ES7210 ADC, dual mic, I2S
 *   PMU     AXP2101
 *
 * Reference pin map - FOR READING ONLY. Do not #define these; include the
 * vendor header and use its BSP_* macros if you need a pin.
 *   I2C      SDA 15   SCL 14
 *   QSPI     CS 12  CLK 38  D0 4  D1 5  D2 6  D3 7   reset 39
 *   Touch    INT 11   reset 40
 *   I2S      MCLK 42  BCLK 9  WS 45  DOUT 8 (codec)  DIN 10 (mic)  PA_EN 46
 *   PMU      AXP2101 at I2C address 0x34
 */
#ifndef PHAROS_BOARD_PINS_H
#define PHAROS_BOARD_PINS_H

/* Panel geometry. Fixed for this board and used by the round-screen layout. */
#define PHAROS_BOARD_LCD_H 466
#define PHAROS_BOARD_LCD_V 466

/* Optional peripherals Pharos probes for rather than assumes.
 *
 * microSD: the 1.75C is not documented with a card slot, so evidence storage
 * targets the flash `evidence` partition (see partitions.csv). Set this if a
 * board revision routes SD.
 *
 * RTC: no discrete RTC is named for this board; the AXP2101 plus the ESP32-S3
 * internal RTC cover timekeeping. A PCF85063-class part would appear on the
 * I2C bus at 0x51 and is probed for, never assumed. */
#define PHAROS_BOARD_HAS_SD           0
#define PHAROS_BOARD_HAS_DISCRETE_RTC 0

/* Battery pack as commonly fitted to the MX1.25 header. VERIFY against yours;
 * the power planner treats its runtime figure as an estimate regardless. */
#define PHAROS_BOARD_BATTERY_MAH 500

#endif /* PHAROS_BOARD_PINS_H */
