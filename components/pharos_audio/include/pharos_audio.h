/* Pharos - the alarm, and the one place this device makes a sound
 *
 * A detector you have to WATCH is half a detector. The whole point of Pharos
 * is that you can put it on a desk, or in a pocket, or on a shelf in a room
 * you are worried about, and get on with something else - and none of that
 * works if the only way it can tell you something is by lighting 466 pixels
 * you are not looking at.
 *
 * So: the ES8311 on this board gets used for exactly one job. When a lens
 * crosses into a band that means something, the device says so out loud.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS DELIBERATELY IS NOT
 *
 * It is not a speaker API. There is no play-this-file, no tone-at-frequency,
 * no volume automation. The vocabulary is five alerts that correspond exactly
 * to the five verdict bands, because every sound this thing makes should be
 * answerable with "what did it find?" - and a device that can be made to emit
 * arbitrary audio in a building you do not own is a different product with a
 * different risk profile.
 *
 * It is also NOT part of the transmit fence's problem. The fence is about
 * radio: frames on the air, attributable to this device, capable of affecting
 * somebody else's network. A speaker cone moves air in the room it is in. The
 * two are not the same promise and this does not weaken that one.
 *
 * ---------------------------------------------------------------------------
 * WHY IT IS LATCHED, NOT LEVEL-TRIGGERED
 *
 * Alerts fire on a band CHANGE, not on a band. A flood that stays at FLOOD
 * LIKELY for four minutes is one event, not two thousand; a device that
 * shrieks continuously gets muted or switched off, and a muted alarm is worse
 * than none because it is trusted. Rising into a band is worth a sound.
 * Sitting in it is worth the screen.
 */
#ifndef PHAROS_AUDIO_H
#define PHAROS_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The alert vocabulary, one per verdict band. Deliberately the same five
 * levels every engine already grades in, so a sound never means something the
 * screen does not. */
typedef enum {
    PHAROS_ALERT_NONE = 0,
    PHAROS_ALERT_NOTICE,   /* ELEVATED   - one soft note, easy to ignore   */
    PHAROS_ALERT_SUSPECT,  /* SUSPICIOUS - two notes, rising               */
    PHAROS_ALERT_ALARM,    /* LIKELY     - urgent, repeated, hard to miss  */
    PHAROS_ALERT_CLEAR,    /* dropped back to quiet - one falling note     */
    PHAROS_ALERT_ACK,      /* a control was accepted; the shortest tick    */
} pharos_alert_t;

/* Bring the codec up. Safe to call when there is no audio hardware, in which
 * case every call below becomes a no-op and this returns false - the firmware
 * must run headless and mute on a bare module. */
bool pharos_audio_init(void);

/* Is there a working codec? Reported by `diag`. */
bool pharos_audio_present(void);

/* Sound one alert. Returns immediately: the tone is rendered on the audio
 * task, because esp_codec_dev_write() blocks for as long as the sound lasts
 * and the caller is usually the UI task, which must not stall a repaint to
 * make a noise. Requests that arrive while one is playing are dropped rather
 * than queued - an alarm backlog is just a longer alarm. */
void pharos_audio_alert(pharos_alert_t which);

/* The operator's mute. Off is remembered across boots, because somebody who
 * silenced this in a meeting should not be surprised by it tomorrow. */
void pharos_audio_set_enabled(bool on);
bool pharos_audio_enabled(void);

/* 0..100. Also persisted. */
void pharos_audio_set_volume(uint8_t pct);
uint8_t pharos_audio_volume(void);

/* Map a 0..4 verdict band onto the alert that band deserves, so every lens
 * gets the same sound for the same severity without each one choosing. */
pharos_alert_t pharos_audio_alert_for_band(uint8_t band);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_AUDIO_H */
