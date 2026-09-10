/*
 * sensor_ringbuf.h - Shared memory ring buffer for sensor events
 *
 * This implements a lock-free ring buffer for passing sensor events
 * from the host (Android) to the guest (RISC-V) via shared memory.
 *
 * The ring buffer is placed in a memory region that is mapped into
 * both the host and guest address spaces.
 */

#ifndef SENSOR_RINGBUF_H
#define SENSOR_RINGBUF_H

#include <stdint.h>
#include <stdatomic.h>

/* Ring buffer capacity (must be power of 2) */
#define SENSOR_RINGBUF_SIZE 256
#define SENSOR_RINGBUF_MASK (SENSOR_RINGBUF_SIZE - 1)

/* Sensor event structure (packed for ABI stability) */
typedef struct {
    int32_t version;
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    union {
        float data[16];
        struct {
            float x;
            float y;
            float z;
            float pad[13];
        } vector;
    };
    uint32_t flags;
    int32_t reserved1[3];
} __attribute__((packed)) sensor_event_t;

/* Ring buffer header (placed at start of shared memory) */
typedef struct {
    atomic_uint_fast32_t head;      /* Producer index (host writes) */
    atomic_uint_fast32_t tail;      /* Consumer index (guest reads) */
    uint32_t capacity;              /* Buffer capacity */
    uint32_t event_size;            /* Size of each event */
    uint32_t reserved[4];           /* Padding for cache line alignment */
} sensor_ringbuf_header_t;

/* Complete ring buffer structure */
typedef struct {
    sensor_ringbuf_header_t header;
    sensor_event_t events[SENSOR_RINGBUF_SIZE];
} sensor_ringbuf_t;

/*
 * Initialize the ring buffer.
 * @param buf  Pointer to the ring buffer in shared memory
 */
static inline void sensor_ringbuf_init(sensor_ringbuf_t* buf)
{
    buf->header.head = 0;
    buf->header.tail = 0;
    buf->header.capacity = SENSOR_RINGBUF_SIZE;
    buf->header.event_size = sizeof(sensor_event_t);
}

/*
 * Check if the ring buffer is empty.
 * @param buf  Pointer to the ring buffer
 * @return     1 if empty, 0 otherwise
 */
static inline int sensor_ringbuf_empty(sensor_ringbuf_t* buf)
{
    return atomic_load_explicit(&buf->header.head, memory_order_acquire) ==
           atomic_load_explicit(&buf->header.tail, memory_order_acquire);
}

/*
 * Check if the ring buffer is full.
 * @param buf  Pointer to the ring buffer
 * @return     1 if full, 0 otherwise
 */
static inline int sensor_ringbuf_full(sensor_ringbuf_t* buf)
{
    uint32_t head = atomic_load_explicit(&buf->header.head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&buf->header.tail, memory_order_acquire);
    return ((head - tail) & SENSOR_RINGBUF_MASK) == SENSOR_RINGBUF_MASK;
}

/*
 * Get the number of events in the buffer.
 * @param buf  Pointer to the ring buffer
 * @return     Number of events available
 */
static inline uint32_t sensor_ringbuf_count(sensor_ringbuf_t* buf)
{
    uint32_t head = atomic_load_explicit(&buf->header.head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&buf->header.tail, memory_order_acquire);
    return (head - tail) & SENSOR_RINGBUF_MASK;
}

/*
 * Push an event into the ring buffer (host side).
 * @param buf   Pointer to the ring buffer
 * @param event Pointer to the event to push
 * @return      0 on success, -1 if full
 */
static inline int sensor_ringbuf_push(sensor_ringbuf_t* buf, const sensor_event_t* event)
{
    uint32_t head = atomic_load_explicit(&buf->header.head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&buf->header.tail, memory_order_acquire);

    if (((head - tail) & SENSOR_RINGBUF_MASK) == SENSOR_RINGBUF_MASK) {
        return -1; /* Full */
    }

    /* Write event data */
    buf->events[head & SENSOR_RINGBUF_MASK] = *event;

    /* Update head (release ensures event data is visible before head) */
    atomic_store_explicit(&buf->header.head, head + 1, memory_order_release);
    return 0;
}

/*
 * Pop an event from the ring buffer (guest side).
 * @param buf   Pointer to the ring buffer
 * @param event Pointer to store the popped event
 * @return      0 on success, -1 if empty
 */
static inline int sensor_ringbuf_pop(sensor_ringbuf_t* buf, sensor_event_t* event)
{
    uint32_t tail = atomic_load_explicit(&buf->header.tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&buf->header.head, memory_order_acquire);

    if (tail == head) {
        return -1; /* Empty */
    }

    /* Read event data */
    *event = buf->events[tail & SENSOR_RINGBUF_MASK];

    /* Update tail (release ensures event data is read before tail) */
    atomic_store_explicit(&buf->header.tail, tail + 1, memory_order_release);
    return 0;
}

/*
 * Pop multiple events from the ring buffer (guest side).
 * @param buf      Pointer to the ring buffer
 * @param events   Array to store popped events
 * @param max_count Maximum number of events to pop
 * @return         Number of events actually popped
 */
static inline int sensor_ringbuf_pop_batch(sensor_ringbuf_t* buf, sensor_event_t* events, int max_count)
{
    int count = 0;
    while (count < max_count) {
        if (sensor_ringbuf_pop(buf, &events[count]) != 0) {
            break;
        }
        count++;
    }
    return count;
}

#endif /* SENSOR_RINGBUF_H */
