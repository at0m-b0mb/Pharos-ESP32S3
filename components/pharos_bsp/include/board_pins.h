/* Pharos - board pin map for the Waveshare ESP32-S3-Touch-AMOLED-1.75C
 *
 * Source of truth, in order of authority:
 *   1. the board schematic PDF (Waveshare "Resources & Documents"), and
 *   2. the Waveshare managed BSP component
 *      waveshare/esp32_s3_touch_amoled_1_75c (^3.0.0), and
 *   3. the board's ESPHome definition, for cross-checking the buses.
 *
 * Constants confirmed against at least two of those are unmarked. Constants I
 * could infer but not confirm against the schematic are marked VERIFY, and
 * MUST be checked against the schematic before the first bring-up - an
 * unverified pin that looks confident has cost somebody a bring-up session;
 * one marked VERIFY costs nobody anything. See docs/HARDWARE.md for the audit
 * trail and how to confirm each one.
 *
 * Board summary (from the Waveshare wiki, cross-checked):
 *   MCU     ESP32-S3R8, dual LX7 @240MHz, 512KB SRAM
 *   PSRAM   8MB octal, stacked
 *   Flash   16MB
 *   Display 1.75" AMOLED, 466x466, driver CO5300, QSPI
 *   Touch   CST9217, capacitive, I2C
 *   IMU     QMI8658, 6-axis, I2C
 *   Audio   ES8311 codec + ES7210 echo/ADC, dual mic array
 *   PMU     AXP2101
 *   Battery 3.7V Li, MX1.25 header
 */
#ifndef PHAROS_BOARD_PINS_H
#define PHAROS_BOARD_PINS_H

/* ---- shared I2C bus (touch, IMU, PMU, codec control, RTC if fitted) --- */
/* Confirmed against the ESPHome board definition. */
#define BSP_I2C_SDA        15
#define BSP_I2C_SCL        14
#define BSP_I2C_HZ         400000

#define BSP_I2C_ADDR_TOUCH 0x5A /* CST9217   VERIFY (0x5A typical, confirm) */
#define BSP_I2C_ADDR_IMU   0x6B /* QMI8658   VERIFY (0x6A/0x6B by SA0)      */
#define BSP_I2C_ADDR_PMU   0x34 /* AXP2101   confirmed (AXP fixed address)  */
#define BSP_I2C_ADDR_CODEC 0x18 /* ES8311    VERIFY                          */
#define BSP_I2C_ADDR_ADC   0x40 /* ES7210    VERIFY                          */

/* ---- QSPI AMOLED, CO5300 -------------------------------------------- */
/* Confirmed against the ESPHome board definition. */
#define BSP_LCD_QSPI_CS    12
#define BSP_LCD_QSPI_CLK   38
#define BSP_LCD_QSPI_D0     4
#define BSP_LCD_QSPI_D1     5
#define BSP_LCD_QSPI_D2     6
#define BSP_LCD_QSPI_D3     7
#define BSP_LCD_RESET      39
#define BSP_LCD_TE         -1 /* tearing-effect line VERIFY (may be unused) */
#define BSP_LCD_H         466
#define BSP_LCD_V         466

/* ---- capacitive touch, CST9217 -------------------------------------- */
/* Confirmed against the ESPHome board definition. */
#define BSP_TOUCH_INT      11
#define BSP_TOUCH_RESET    40

/* ---- I2S audio (ES8311 out, ES7210 mic in) -------------------------- */
/* Confirmed against the ESPHome board definition. */
#define BSP_I2S_MCLK       42
#define BSP_I2S_BCLK        9
#define BSP_I2S_WS         45
#define BSP_I2S_DOUT        8 /* to codec (playback)   */
#define BSP_I2S_DIN        10 /* from ADC (mic array)  */
#define BSP_SPK_ENABLE     46

/* ---- AXP2101 PMU ----------------------------------------------------- */
#define BSP_PMU_IRQ        -1 /* VERIFY: PMU interrupt line if routed */

/* ---- microSD ---------------------------------------------------------
 * The 1.75C is documented with 16MB flash and does NOT clearly expose a
 * microSD slot in the wiki summary. Evidence storage therefore targets a
 * flash "evidence" partition by default (see partitions.csv); SD is enabled
 * only if a board revision routes it. All four are VERIFY and disabled until
 * confirmed. */
#define BSP_HAS_SD          0
#define BSP_SD_CMD         -1 /* VERIFY */
#define BSP_SD_CLK         -1 /* VERIFY */
#define BSP_SD_D0          -1 /* VERIFY */

/* ---- buttons --------------------------------------------------------- */
#define BSP_BTN_BOOT        0 /* strapping/BOOT, standard on ESP32-S3 */
#define BSP_BTN_PWR        -1 /* PWR handled by AXP2101, not a GPIO   */

/* ---- RTC -------------------------------------------------------------
 * The wiki summary does not name a discrete RTC. The AXP2101 and the ESP32-S3
 * internal RTC cover timekeeping; a PCF85063-class part on the I2C bus is
 * plausible on some revisions. Treated as optional and probed at boot. */
#define BSP_HAS_DISCRETE_RTC 0
#define BSP_I2C_ADDR_RTC   0x51 /* PCF85063 typical, VERIFY presence */

#endif /* PHAROS_BOARD_PINS_H */
