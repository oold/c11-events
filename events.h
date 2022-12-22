// SPDX-FileCopyrightText: 2022 Oliver Old <oliver.old@outlook.com>
// SPDX-License-Identifier: MIT

#include <stddef.h>
#include <stdbool.h>
#include <time.h>

typedef struct _event_t event_t;
typedef int event_error_t;

// Get size of event_t.
size_t event_get_size(void);

// Initialize an event_t.
// The event resets after it was waited on unless 'is_manual_reset' is true.
event_error_t event_init(event_t* p_event, bool is_manual_reset, bool initial_state);
// Destroy the event_t.
void event_destroy(event_t* p_event);

// Set event_t to signaled.
event_error_t event_signal(event_t* p_event);
// Reset event_t to unsignaled.
event_error_t event_reset(event_t* p_event);
// Set event_t to signaled, then reset event_t to unsignaled.
event_error_t event_pulse(event_t* p_event);

// Wait on one event_t.
// Wait until '*p_time' if 'p_time' is not null, else wait indefinitely. Returns ETIMEDOUT if time expired.
event_error_t event_wait(event_t* p_event, const struct timespec* p_time);
// Wait on multiple event_t.
// 'p_events' is a pointer to an array of event_t*. 'c_events' is the amount of event_t*.
// Waits for one signaled event or for all events to become signaled if 'wait_all' is true.
// Wait until '*p_time' if 'p_time' is not null, else wait indefinitely. Returns ETIMEDOUT if time expired.
// 'p_idx_signaled_event' is a *required* out pointer for the index of the signaled event if 'wait_all' is false.
event_error_t event_wait_multiple(event_t** p_events, size_t c_events, bool wait_all, const struct timespec* p_time, size_t* p_idx_signaled_event);
