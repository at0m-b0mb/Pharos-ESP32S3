/* Pharos - the ingest bus
 *
 * One single-producer / single-consumer lock-free ring per ingest path. The
 * producer is a radio driver callback that cannot block and cannot take a
 * mutex; the consumer is the analytics task on the other core.
 *
 * Overflow policy is drop-newest *and count*. That count is not a debug
 * statistic - it is evidence. The moment a detector matters most (a flood)
 * is exactly the moment the ring overflows, so every verdict carries the
 * drop count and every engine is expected to widen its uncertainty when
 * frames were lost rather than quietly under-reporting.
 */
#ifndef PHAROS_BUS_H
#define PHAROS_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pharos_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pharos_bus {
    pharos_event_t *slots;
    uint32_t mask; /* capacity - 1; capacity is a power of two */
    /* Accessed from two cores. Declared volatile here and fenced in the
     * implementation; see pharos_bus.c for the ordering argument. */
    volatile uint32_t head; /* producer writes */
    volatile uint32_t tail; /* consumer writes */
    volatile uint32_t dropped;
    volatile uint32_t accepted;
} pharos_bus_t;

/* capacity must be a power of two. storage must hold capacity events. */
bool pharos_bus_init(pharos_bus_t *bus, pharos_event_t *storage, uint32_t capacity);

/* Producer side. Never blocks. Returns false if the ring was full, having
 * already incremented the drop counter. */
bool pharos_bus_push(pharos_bus_t *bus, const pharos_event_t *ev);

/* Consumer side. Returns false when empty. */
bool pharos_bus_pop(pharos_bus_t *bus, pharos_event_t *out);

uint32_t pharos_bus_pending(const pharos_bus_t *bus);
uint32_t pharos_bus_dropped(const pharos_bus_t *bus);
uint32_t pharos_bus_accepted(const pharos_bus_t *bus);

/* Fraction of offered events that made it through, per mille. A detector
 * multiplies its observed rates by 1000/this to estimate true rates, and
 * lowers its confidence ceiling as this falls. */
uint16_t pharos_bus_yield_permil(const pharos_bus_t *bus);

void pharos_bus_reset(pharos_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_BUS_H */
