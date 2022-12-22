// SPDX-FileCopyrightText: 2022 Oliver Old <oliver.old@outlook.com>
// SPDX-License-Identifier: MIT

#include "events.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

struct _event_t {
    mtx_t mtx;
    cnd_t cnd;
    bool signaled;
    bool is_manual_reset;
};

typedef struct _event_wait_info_t {
    mtx_t mtx;
    cnd_t cnd;
} _event_wait_info_t;

typedef struct _event_waiter_t {
    thrd_t thrd;
    event_t* p_event;
    _event_wait_info_t* p_wait_info;
    bool joinable;
    bool canceled;
    bool done;
} _event_waiter_t;

static int _thrd_status_to_errno(int thrd_status) {
    switch (thrd_status) {
        case thrd_success:
            return 0;
        case thrd_nomem:
            return ENOMEM;
        case thrd_timedout:
            return ETIMEDOUT;
        case thrd_busy:
            return EBUSY;
        default:
            return -1;
    }
}

#define CHECK_THRD_ERR(err) _check_thrd_err(err, __FILE__, __LINE__, __func__)

static void _check_thrd_err(int thrd_status, const char* file, unsigned int line, const char* func) {
    if (thrd_status != thrd_success) {
        char* strerr = strerror(_thrd_status_to_errno(thrd_status));
        fprintf(stderr, "%s:%u: %s: %s\n", file, line, func, strerr);
        abort();
    }
}

static int _event_wait_helper(_event_waiter_t* p_waiter) {
    event_t* p_event = p_waiter->p_event;
    _event_wait_info_t* p_wait_info = p_waiter->p_wait_info;
    bool signaled = false;
    int thrd_status;
    int thrd_status_2;

    if ((thrd_status = mtx_lock(&p_event->mtx)) == thrd_success) {
        if (!(signaled = p_event->signaled)) {
            while ((thrd_status = cnd_wait(&p_event->cnd, &p_event->mtx)) == thrd_success) {
                CHECK_THRD_ERR(mtx_lock(&p_wait_info->mtx));
                bool canceled = p_waiter->canceled;
                CHECK_THRD_ERR(mtx_unlock(&p_wait_info->mtx));

                if (canceled || (signaled = p_event->signaled))
                    break;
            }
        }

        thrd_status_2 = mtx_unlock(&p_event->mtx);
        if (thrd_status == thrd_success)
            thrd_status = thrd_status_2;
    }

    CHECK_THRD_ERR(mtx_lock(&p_wait_info->mtx));

    p_waiter->done = true;

    CHECK_THRD_ERR(cnd_signal(&p_wait_info->cnd));
    CHECK_THRD_ERR(mtx_unlock(&p_wait_info->mtx));

    if (thrd_status != thrd_success)
        return _thrd_status_to_errno(thrd_status);

    if (!signaled)
        return ECANCELED;

    return 0;
}

size_t event_get_size(void) {
    return sizeof(event_t);
}

event_error_t event_init(event_t* p_event, bool is_manual_reset, bool initial_state) {
    if (!p_event)
        return EINVAL;

    int thrd_status;

    if ((thrd_status = mtx_init(&p_event->mtx, mtx_plain)) == thrd_success) {
        if ((thrd_status = cnd_init(&p_event->cnd)) == thrd_success) {
            p_event->signaled = initial_state;
            p_event->is_manual_reset = is_manual_reset;
            return 0;
        }

        mtx_destroy(&p_event->mtx);
    }

    return _thrd_status_to_errno(thrd_status);
}

void event_destroy(event_t* p_event) {
    if (p_event) {
        cnd_destroy(&p_event->cnd);
        mtx_destroy(&p_event->mtx);
    }
}

event_error_t event_signal(event_t* p_event) {
    if (!p_event)
        return EINVAL;

    int thrd_status;
    int thrd_status_2;

    if ((thrd_status = mtx_lock(&p_event->mtx)) == thrd_success) {
        p_event->signaled = true;
        thrd_status = p_event->is_manual_reset ? cnd_broadcast(&p_event->cnd) : cnd_signal(&p_event->cnd);
        thrd_status_2 = mtx_unlock(&p_event->mtx);
        if (thrd_status == thrd_success)
            thrd_status = thrd_status_2;
    }

    return _thrd_status_to_errno(thrd_status);
}

event_error_t event_reset(event_t* p_event) {
    if (!p_event)
        return EINVAL;

    int thrd_status;

    if ((thrd_status = mtx_lock(&p_event->mtx)) == thrd_success) {
        p_event->signaled = false;
        thrd_status = mtx_unlock(&p_event->mtx);
    }

    return _thrd_status_to_errno(thrd_status);
}

event_error_t event_pulse(event_t* p_event) {
    event_error_t err;
    if (!(err = event_signal(p_event)))
        err = event_reset(p_event);
    return err;
}

event_error_t event_wait(event_t* p_event, const struct timespec* p_time) {
    if (!p_event)
        return EINVAL;

    int thrd_status;
    int thrd_status_2;

    if ((thrd_status = mtx_lock(&p_event->mtx)) == thrd_success) {
        do {
            if (p_event->signaled) {
                if (!p_event->is_manual_reset)
                    p_event->signaled = false;
                break;
            }
        } while ((thrd_status = p_time ? cnd_timedwait(&p_event->cnd, &p_event->mtx, p_time) : cnd_wait(&p_event->cnd, &p_event->mtx)) == thrd_success);

        thrd_status_2 = mtx_unlock(&p_event->mtx);
        if (thrd_status == thrd_success)
            thrd_status = thrd_status_2;
    }

    return _thrd_status_to_errno(thrd_status);
}

event_error_t event_wait_multiple(event_t** p_events, size_t c_events, bool wait_all, const struct timespec* p_time, size_t* p_idx_signaled_event) {
    if (p_idx_signaled_event)
        *p_idx_signaled_event = 0;

    if (!c_events)
        return 0;

    if (!p_events || (!wait_all && !p_idx_signaled_event))
        return EINVAL;

    if (c_events == 1)
        return event_wait(*p_events, p_time);

    _event_waiter_t* p_waiters;
    _event_wait_info_t wait_info;
    int err = 0;
    int thrd_status = thrd_success;
    int thrd_status_2;
    bool all_signaled;

    p_waiters = calloc(c_events, sizeof(_event_waiter_t));
    if (!p_waiters)
        return errno;

    if ((thrd_status = mtx_init(&wait_info.mtx, mtx_plain)) != thrd_success)
        goto clean_up_waiters;

    if ((thrd_status = cnd_init(&wait_info.cnd)) != thrd_success)
        goto clean_up_wait_info_mtx;

restart_wait:
    for (size_t i = 0; i < c_events; ++i) {
        _event_waiter_t* p_waiter = &p_waiters[i];
        p_waiter->p_event = p_events[i];
        p_waiter->p_wait_info = &wait_info;
        p_waiter->joinable = true;
        p_waiter->canceled = false;
        p_waiter->done = false;

        if ((thrd_status = thrd_create(&p_waiter->thrd, (thrd_start_t)_event_wait_helper, p_waiter)) != thrd_success) {
            for (size_t j = 0; j < i; ++j)
                thrd_join(p_waiters[j].thrd, NULL);

            goto clean_up_wait_info_cnd;
        }
    }

    CHECK_THRD_ERR(mtx_lock(&wait_info.mtx));

    if (thrd_status != thrd_success)
        goto clean_up_threads;

    do {
        if (wait_all) {
            all_signaled = true;
            for (size_t i = 0; i < c_events; ++i) {
                _event_waiter_t* p_waiter = &p_waiters[i];

                if (p_waiter->done) {
                    if (p_waiter->joinable) {
                        thrd_status = thrd_join(p_waiter->thrd, &err);
                        p_waiter->joinable = false;

                        if (thrd_status != thrd_success) {
                            err = 0;
                            goto clean_up_threads;
                        }

                        if (err)
                            goto clean_up_threads;
                    }
                } else {
                    all_signaled = false;
                }
            }

            if (all_signaled) {
                size_t locked;
                for (locked = 0; locked < c_events; ++locked) {
                    if ((thrd_status = mtx_lock(&p_events[locked]->mtx)) != thrd_success)
                        break;

                    if (!p_events[locked]->signaled) {
                        all_signaled = false;
                        break;
                    }
                }

                thrd_status_2 = thrd_success;
                for (size_t i = 0; i < locked; ++i) {
                    if (all_signaled && !p_events[i]->is_manual_reset)
                        p_events[i]->signaled = false;

                    thrd_status_2 = mtx_unlock(&p_events[i]->mtx);
                }

                if (thrd_status == thrd_success)
                    thrd_status = thrd_status_2;

                goto clean_up_threads;
            }
        } else {
            for (size_t i = 0; i < c_events; ++i) {
                _event_waiter_t* p_waiter = &p_waiters[i];

                if (p_waiter->done) {
                    if (p_waiter->joinable) {
                        thrd_status = thrd_join(p_waiter->thrd, &err);
                        p_waiter->joinable = false;

                        if (thrd_status != thrd_success) {
                            err = 0;
                            goto clean_up_threads;
                        }

                        if (err)
                            goto clean_up_threads;
                    }

                    *p_idx_signaled_event = i;

                    if (!p_events[i]->is_manual_reset && mtx_lock(&p_events[i]->mtx) == thrd_success) {
                        p_events[i]->signaled = false;
                        mtx_unlock(&p_events[i]->mtx);
                    }

                    goto clean_up_threads;
                }
            }
        }
    } while ((thrd_status = p_time ? cnd_timedwait(&wait_info.cnd, &wait_info.mtx, p_time) : cnd_wait(&wait_info.cnd, &wait_info.mtx)) == thrd_success);

clean_up_threads:
    for (size_t i = 0; i < c_events; ++i) {
        _event_waiter_t* p_waiter = &p_waiters[i];

        if (!p_waiter->done) {
            event_t* p_event = p_waiters[i].p_event;

            CHECK_THRD_ERR(mtx_lock(&p_event->mtx));
            p_waiter->canceled = true;
            CHECK_THRD_ERR(cnd_broadcast(&p_event->cnd));
            CHECK_THRD_ERR(mtx_unlock(&p_event->mtx));
        }
    }

    CHECK_THRD_ERR(mtx_unlock(&wait_info.mtx));

    for (size_t i = 0; i < c_events; ++i) {
        _event_waiter_t* p_waiter = &p_waiters[i];

        if (p_waiter->joinable)
            thrd_join(p_waiter->thrd, NULL);
    }

    if (wait_all && !err && thrd_status == thrd_success && !all_signaled)
        goto restart_wait;

clean_up_wait_info_cnd:
    cnd_destroy(&wait_info.cnd);

clean_up_wait_info_mtx:
    mtx_destroy(&wait_info.mtx);

clean_up_waiters:
    free(p_waiters);

    if (err)
        return err;

    return _thrd_status_to_errno(thrd_status);
}
