/*
 * AioContext wait support
 *
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef QEMU_AIO_WAIT_H
#define QEMU_AIO_WAIT_H

#include "block/aio.h"
#include "qemu/coroutine.h"

/**
 * AioWait:
 *
 * An object that facilitates synchronous waiting on a condition.  The main
 * loop can wait on an operation running in an IOThread as follows:
 *
 *   AioWait *wait = ...;
 *   AioContext *ctx = ...;
 *   MyWork work = { .done = false };
 *   schedule_my_work_in_iothread(ctx, &work);
 *   AIO_WAIT_WHILE(wait, ctx, !work.done);
 *
 * The IOThread must call aio_wait_kick() to notify the main loop when
 * work.done changes:
 *
 *   static void do_work(...)
 *   {
 *       ...
 *       work.done = true;
 *       aio_wait_kick(wait);
 *   }
 */
typedef struct {
    /* Number of waiting AIO_WAIT_WHILE() callers. Accessed with atomic ops. */
    unsigned num_waiters;

    CoQueue wait_queue;
} AioWait;

void aio_wait_init(AioWait *wait);

/**
 * AIO_WAIT_WHILE:
 * @wait: the aio wait object
 * @ctx: the aio context, or NULL if multiple aio contexts (for which the
 *       caller does not hold a lock) are involved in the polling condition.
 * @cond: wait while this conditional expression is true
 *
 * Wait while a condition is true.  Use this to implement synchronous
 * operations that require event loop activity.
 *
 * The caller must be sure that something calls aio_wait_kick() when the value
 * of @cond might have changed.
 *
 * The caller's thread must be the IOThread that owns @ctx or the main loop
 * thread (with @ctx acquired exactly once).  This function cannot be used to
 * wait on conditions between two IOThreads since that could lead to deadlock,
 * go via the main loop instead.
 */
#define AIO_WAIT_WHILE(wait, ctx, cond) ({                         \
    bool waited_ = false;                                          \
    AioWait *wait_ = (wait);                                       \
    AioContext *ctx_ = (ctx);                                      \
    if (qemu_in_coroutine()) {                                     \
        while ((cond)) {                                           \
            atomic_inc(&wait_->num_waiters);                       \
            qemu_co_queue_wait(&wait_->wait_queue, NULL);          \
            atomic_dec(&wait_->num_waiters);                       \
        }                                                          \
    } else if (ctx_ && in_aio_context_home_thread(ctx_)) {         \
        while ((cond)) {                                           \
            aio_poll(ctx_, true);                                  \
            waited_ = true;                                        \
        }                                                          \
    } else {                                                       \
        assert(qemu_get_current_aio_context() ==                   \
               qemu_get_aio_context());                            \
        /* Increment wait_->num_waiters before evaluating cond. */ \
        atomic_inc(&wait_->num_waiters);                           \
        while ((cond)) {                                           \
            if (ctx_) {                                            \
                aio_context_release(ctx_);                         \
            }                                                      \
            aio_poll(qemu_get_aio_context(), true);                \
            if (ctx_) {                                            \
                aio_context_acquire(ctx_);                         \
            }                                                      \
            waited_ = true;                                        \
        }                                                          \
        atomic_dec(&wait_->num_waiters);                           \
    }                                                              \
    waited_; })

/**
 * aio_wait_kick:
 * @wait: the aio wait object that should re-evaluate its condition
 *
 * Wake up the main thread if it is waiting on AIO_WAIT_WHILE().  During
 * synchronous operations performed in an IOThread, the main thread lets the
 * IOThread's event loop run, waiting for the operation to complete.  A
 * aio_wait_kick() call will wake up the main thread.
 */
void aio_wait_kick(AioWait *wait);

/**
 * aio_wait_bh_oneshot:
 * @ctx: the aio context
 * @cb: the BH callback function
 * @opaque: user data for the BH callback function
 *
 * Run a BH in @ctx and wait for it to complete.
 *
 * Must be called from the main loop thread with @ctx acquired exactly once.
 * Note that main loop event processing may occur.
 */
void aio_wait_bh_oneshot(AioContext *ctx, QEMUBHFunc *cb, void *opaque);

#endif /* QEMU_AIO_WAIT */
