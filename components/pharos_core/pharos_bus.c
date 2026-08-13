#include "pharos_bus.h"

#include <string.h>

/* Memory ordering argument.
 *
 * Single producer, single consumer, on two cores of an ESP32-S3 sharing
 * coherent SRAM. The producer publishes a slot then advances head; the
 * consumer reads head, then the slot, then advances tail. Two barriers are
 * enough:
 *
 *   producer: write slot ... RELEASE ... head++
 *   consumer: read head ... ACQUIRE ... read slot ... RELEASE ... tail++
 *
 * so the slot store cannot be reordered past the head store, and the slot
 * load cannot be hoisted above the head load.
 */
#if defined(__GNUC__) || defined(__clang__)
#define PHAROS_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)
#define PHAROS_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#else
#define PHAROS_RELEASE() do { } while (0)
#define PHAROS_ACQUIRE() do { } while (0)
#endif

bool pharos_bus_init(pharos_bus_t *bus, pharos_event_t *storage, uint32_t capacity)
{
    if (!bus || !storage || capacity == 0 || (capacity & (capacity - 1)) != 0) {
        return false;
    }
    bus->slots = storage;
    bus->mask = capacity - 1;
    bus->head = 0;
    bus->tail = 0;
    bus->dropped = 0;
    bus->accepted = 0;
    return true;
}

bool pharos_bus_push(pharos_bus_t *bus, const pharos_event_t *ev)
{
    if (!bus || !bus->slots || !ev) {
        return false;
    }
    const uint32_t head = bus->head;
    const uint32_t tail = bus->tail;

    /* Unsigned wraparound subtraction: correct across the uint32 rollover
     * as long as capacity <= 2^31, which the power-of-two check guarantees
     * for any realistic ring. */
    if ((uint32_t)(head - tail) > bus->mask) {
        bus->dropped++;
        return false;
    }

    bus->slots[head & bus->mask] = *ev;
    PHAROS_RELEASE();
    bus->head = head + 1;
    bus->accepted++;
    return true;
}

bool pharos_bus_pop(pharos_bus_t *bus, pharos_event_t *out)
{
    if (!bus || !bus->slots || !out) {
        return false;
    }
    const uint32_t tail = bus->tail;
    const uint32_t head = bus->head;
    if (head == tail) {
        return false;
    }
    PHAROS_ACQUIRE();
    *out = bus->slots[tail & bus->mask];
    PHAROS_RELEASE();
    bus->tail = tail + 1;
    return true;
}

uint32_t pharos_bus_pending(const pharos_bus_t *bus)
{
    return bus ? (uint32_t)(bus->head - bus->tail) : 0;
}

uint32_t pharos_bus_dropped(const pharos_bus_t *bus)
{
    return bus ? bus->dropped : 0;
}

uint32_t pharos_bus_accepted(const pharos_bus_t *bus)
{
    return bus ? bus->accepted : 0;
}

uint16_t pharos_bus_yield_permil(const pharos_bus_t *bus)
{
    if (!bus) {
        return 0;
    }
    const uint32_t offered = bus->accepted + bus->dropped;
    if (offered == 0) {
        return 1000; /* nothing offered yet; do not penalise */
    }
    /* 64-bit intermediate: accepted can plausibly exceed 4.2M in a long
     * session, and accepted * 1000 would overflow 32 bits at ~4.3M. */
    return (uint16_t)(((uint64_t)bus->accepted * 1000u) / offered);
}

void pharos_bus_reset(pharos_bus_t *bus)
{
    if (!bus) {
        return;
    }
    bus->head = 0;
    bus->tail = 0;
    bus->dropped = 0;
    bus->accepted = 0;
}
