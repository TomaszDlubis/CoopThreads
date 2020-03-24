/*
 * Copyright (c) 2020 Piotr Stolarz
 * Lightweight cooperative threads library
 *
 * Distributed under the 2-clause BSD License (the License)
 * see accompanying file LICENSE for details.
 *
 * This software is distributed WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the License for more information.
 */

#include <alloca.h>
#include <setjmp.h>
#include <stdbool.h>
#include <string.h> /* memset() */
#include "coop_threads.h"

/**
 * Thread states.
 */
typedef enum
{
    EMPTY = 0,  /** Empty context slot on the pool. Id must be 0. */
    HOLE,       /** Thread terminated but its stack still occupies the main
                    stack, where threads stacks are allocated. */
    NEW,        /** Already created thread, not yet started. */
    RUN,        /** Running thread. */
#ifdef CONFIG_OPT_IDLE
    IDLE,       /** Thread is idle. */
#endif
#ifdef CONFIG_OPT_WAIT
    WAIT,       /** Waiting thread. */
#endif
} coop_thrd_state_t;

#ifdef CONFIG_OPT_IDLE
# define _IS_IDLE(_state) ((_state) == IDLE)
#else
# define _IS_IDLE(_state) (0)
#endif

#ifdef CONFIG_OPT_WAIT
# define _IS_WAIT(_state) ((_state) == WAIT)
#else
# define _IS_WAIT(_state) (0)
#endif

/* started thread in an active thread with allocated stack (therefore not NEW) */
#define _IS_STARTED(_state) \
    ((_state) == RUN || _IS_IDLE(_state) || _IS_WAIT(_state))

#define _STATE_NAME(_state) \
    (_IS_WAIT(_state) ? "WAIT" : (_IS_IDLE(_state) ? "IDLE" : \
    ((_state) == RUN ? "RUN" : ((_state) == NEW ? "NEW" : \
    ((_state) == HOLE ? "HOLE" : "EMPTY")))))

/**
 * Thread context.
 */
typedef struct
{
    /** Thread routine. */
    coop_thrd_proc_t proc;

    /** Thread name (may be NULL). */
    const char *name;

    /** Thread stack size. */
    size_t stack_sz;

    /** User passed argument. */
    void *arg;

    /** Thread state. */
    coop_thrd_state_t state;

#ifdef CONFIG_OPT_IDLE
    /** Clock tick the thread is idle up to. */
    coop_tick_t idle_to;
#endif
#ifdef CONFIG_OPT_YIELD_AFTER
    /** Scheduler to thread switch clock tick */
    coop_tick_t switch_tick;
#endif
#ifdef CONFIG_OPT_WAIT
    /** Semaphore id. */
    int sem_id;

    /** Clock tick the thread is waiting up to. */
    coop_tick_t wait_to;

    /** Waiting related flags. */
    struct {
        unsigned char notif: 1; /** Notified flag. */
        unsigned char inf:   1; /** Infinite wait; @c wait_to not applied. */
        unsigned char res:   6; /** Reserved. */
    } wait_flgs;
#endif

    /**
     * Thread stack depth on the main stack. 1 for the first started (deepest)
     * thread. @c coop_sched_ctx_t::depth for latest (most shallow) thread.
     */
    unsigned depth;

    /** Thread execution context. */
    jmp_buf exe_ctx;

    /** Thread entry execution context (used for stack unwinding). */
    jmp_buf entry_ctx;
} coop_thrd_ctx_t;

/**
 * Scheduler context.
 */
typedef struct
{
    /** Scheduler currently processed thread. */
    unsigned cur_thrd;

    /** Number of occupied (non empty) thread slots. */
    unsigned busy_n;

    /** Number of holes (terminated threads occupying the main stack). */
    unsigned hole_n;

#ifdef CONFIG_OPT_IDLE
    /** Number of idle threads. */
    unsigned idle_n;
#endif

    /**
     * Number of threads currently occupying the main stack.
     */
    unsigned depth;

    /** Scheduler execution context. */
    jmp_buf exe_ctx;

    /** Threads pool of contexts. */
    coop_thrd_ctx_t thrds[CONFIG_MAX_THREADS];
} coop_sched_ctx_t;

static coop_sched_ctx_t sched = {};


static void _sched_init(bool force)
{
    static bool inited = false;

    if (!inited || force) {
        inited = true;
        memset(&sched, 0, sizeof(sched));
        sched.cur_thrd = (unsigned)-1;
    }
}

/*
 * NOTE: to reduce stack usage by coop_sched_service() helper routines, these
 * are defined as inline with all their local variables stored in registers.
 */

/**
 * Mark threads whose stacks need to be unwinded.
 *
 * Return terminated thread index at which the unwinded stack shall finish.
 * The new scheduler stack frame will be set at the thread context after the
 * unwinding process completes.
 */
static inline unsigned _mark_unwind_thrds()
{
    register unsigned i, depth, unwnd_thrd = sched.cur_thrd;

    /* mark the terminating (most shallow) thread as EMPTY */
    coop_dbg_log_cb("Thread #%d: RUN -> EMPTY\n", sched.cur_thrd);
    sched.thrds[sched.cur_thrd].state = EMPTY;
    sched.busy_n--;

    /* calculate current main stack depth */
    for (i = depth = 0; i < CONFIG_MAX_THREADS; i++) {
        if (_IS_STARTED(sched.thrds[i].state)) {
            if (depth < sched.thrds[i].depth)
                depth = sched.thrds[i].depth;
        }
    }

    if (depth + 1 < sched.depth)
    {
        /*
         * All holes between the terminating thread and the most shallow
         * started thread are marked as EMPTY to mark stack space occupied
         * by these threads stacks as to be freed.
         */
        for (i = 0; i < CONFIG_MAX_THREADS; i++) {
            if (sched.thrds[i].state == HOLE) {
                if (depth + 1 <= sched.thrds[i].depth)
                {
                    if (depth + 1 == sched.thrds[i].depth) {
                        unwnd_thrd = i;
                    }
                    coop_dbg_log_cb("Thread #%d: HOLE -> EMPTY\n", i);
                    sched.thrds[i].state = EMPTY;
                    sched.busy_n--;
                    sched.hole_n--;
                }
            }
        }
    } else {
        /*
         * No hole between the most shallow started thread and the terminating
         * thread. Unwinded stack will be set to the terminating thread stack.
         */
    }
    sched.depth = depth;

    return unwnd_thrd;
}

#ifdef CONFIG_OPT_IDLE
/**
 * Check conditions and enter the system idle state if necessary.
 */
static inline void _system_idle(void)
{
    register unsigned i = 0;
    register coop_tick_t min_idle = 0, cur_tick = 0;

    /* system is considered idle-ready if all active threads are idle */
    while (sched.idle_n > 0 && (sched.busy_n - sched.hole_n) <= sched.idle_n)
    {
        if (i) {
            coop_dbg_log_cb(
                "System going idle for %lu ticks\n", (unsigned long)min_idle);

            /* subsequent loop pass; system is idle up to nearest wake-up time */
            coop_idle_cb(min_idle);
        }

        min_idle = COOP_MAX_TICK;
        cur_tick = coop_tick_cb();  /* current tick */

        for (i = 0; i < CONFIG_MAX_THREADS; i++)
        {
            if (sched.thrds[i].state != IDLE) {
                continue;
            } else
            if (COOP_IS_TICK_OVER(cur_tick, sched.thrds[i].idle_to))
            {
                /* idle time passed; the idle-loop will be finished */
                coop_dbg_log_cb("Thread #%d IDLE -> RUN (via idle-loop)\n", i);
                sched.thrds[i].state = RUN;
                sched.idle_n--;
            } else
            if ((sched.thrds[i].idle_to - cur_tick) < min_idle) {
                /* calculate nearest wake-up time */
                min_idle = (sched.thrds[i].idle_to - cur_tick);
            }
        }
    }
}
#endif

void coop_sched_service(void)
{
    while (sched.busy_n > 0)
    {
        /*
         * coop_sched_service() routine is called recursively during building
         * stack frames for newly created threads. Each time the recursion
         * occurs the cur_thrd index need to be updated for the next thread to
         * process. For this reason the incrementation takes place at the loop
         * entry stage.
         */
        sched.cur_thrd = (sched.cur_thrd + 1) % CONFIG_MAX_THREADS;

#ifdef CONFIG_OPT_IDLE
        _system_idle();
#endif

        switch (sched.thrds[sched.cur_thrd].state)
        {
        case EMPTY:
        case HOLE:
        default:
            break;

#ifdef CONFIG_OPT_IDLE
        case IDLE:
            if (!COOP_IS_TICK_OVER(
                    coop_tick_cb(), sched.thrds[sched.cur_thrd].idle_to))
            {
                /* the current thread is idle but other threads are running;
                   system can't switch to the idle state in this case */
                break;
            }

            /* idle time passed; continue as in RUN state  */
            coop_dbg_log_cb("Thread #%d IDLE -> RUN (via sched-loop)\n",
                sched.cur_thrd);
            sched.thrds[sched.cur_thrd].state = RUN;
            sched.idle_n--;
            goto run;
#endif

#ifdef CONFIG_OPT_WAIT
        case WAIT:
            if (sched.thrds[sched.cur_thrd].wait_flgs.inf ||
                !COOP_IS_TICK_OVER(
                    coop_tick_cb(), sched.thrds[sched.cur_thrd].wait_to))
            {
                /* not-notified infinite or not yet timed-out waiting thread */
                break;
            }

            /* wait time passed; continue as in RUN state  */
            coop_dbg_log_cb("Thread #%d WAIT -> RUN (timed-out)\n",
                sched.cur_thrd);
            sched.thrds[sched.cur_thrd].state = RUN;
            goto run;
#endif

        case RUN:
#if defined(CONFIG_OPT_IDLE) || defined(CONFIG_OPT_WAIT)
run:
#endif
            /* sched_pos_run: main-running scheduler execution context */
            if (!setjmp(sched.exe_ctx))
            {
                coop_dbg_log_cb("setjmp sched_pos_run; run thread #%d: "
                    "longjmp thrd_pos_[new/run]\n", sched.cur_thrd);

                /* jump to running thread: thrd_pos_new, thrd_pos_run */
#ifdef CONFIG_OPT_YIELD_AFTER
                sched.thrds[sched.cur_thrd].switch_tick = coop_tick_cb();
#endif
                longjmp(sched.thrds[sched.cur_thrd].exe_ctx, 1);
            } else {
                /* return from yielded running thread or restore
                   scheduler stack after thread terminated as a hole */
                coop_dbg_log_cb("Back to scheduler from #%d thread\n",
                    sched.cur_thrd);
            }
            break;

        case NEW:
            /* sched_pos_entry_thrd: save a new thread entry stack state */
            if (!setjmp(sched.thrds[sched.cur_thrd].entry_ctx))
            {
                coop_dbg_log_cb("setjmp sched_pos_entry_thrd; new thread #%d\n",
                    sched.cur_thrd);

                sched.depth++;
                sched.thrds[sched.cur_thrd].depth = sched.depth;

                /* enter the thread routine */
#ifdef CONFIG_OPT_YIELD_AFTER
                sched.thrds[sched.cur_thrd].switch_tick = coop_tick_cb();
#endif
                sched.thrds[sched.cur_thrd].proc(sched.thrds[sched.cur_thrd].arg);

                /*
                 * At this point the current thread is being terminated.
                 * Scheduler stack is set at the thread stack frame, therefore
                 * need to be updated:
                 *
                 * - If the terminating thread is below the current main stack
                 *   depth, the thread constitutes a hole (a terminated thread
                 *   with its stack still occupying the main stack space).
                 *   Scheduler stack frame is restored to its previous position.
                 *   No stack unwind occurs in this case.
                 *
                 * - If the terminating thread stack depth is at the main stack
                 *   depth level (most shallow thread), the stack unwind occurs.
                 *   The scheduler stack frame is set to the position of an
                 *   already terminated thread stack frame (may to be a hole)
                 *   just above a started thread with most shallow stack frame.
                 */
                if (sched.thrds[sched.cur_thrd].depth < sched.depth)
                {
                    coop_dbg_log_cb("Thread #%d: RUN -> HOLE; "
                        "scheduler stack-restore: longjmp sched_pos_run\n",
                        sched.cur_thrd);

                    sched.thrds[sched.cur_thrd].state = HOLE;
                    sched.hole_n++;

                    /* restore previous scheduler stack frame; sched_pos_run jump */
                    longjmp(sched.exe_ctx, 1);
                } else
                {
                    register unsigned unwnd_thrd = _mark_unwind_thrds();

                    coop_dbg_log_cb("Unwind scheduler stack at #%d thread entry "
                        "context: longjmp sched_pos_entry_thrd\n", unwnd_thrd);

                    /* unwind scheduler stack; sched_pos_entry_thrd jump */
                    longjmp(sched.thrds[unwnd_thrd].entry_ctx, 1);
                }
            } else {
                /* return with unwinded stack; new scheduler stack frame
                   from this point */
                coop_dbg_log_cb("Back to scheduler; stack unwinded\n");
            }
            break;
        }
    }

    _sched_init(true);
}

coop_error_t coop_sched_thread(coop_thrd_proc_t proc, const char *name,
    size_t stack_sz, void *arg)
{
    if (!proc) {
        return COOP_INV_ARG;
    } else
    if (sched.busy_n >= CONFIG_MAX_THREADS) {
        return COOP_ERR_LIMIT;
    }

    _sched_init(false);

    for (unsigned i = 0; i < CONFIG_MAX_THREADS; i++) {
        if (sched.thrds[i].state == EMPTY)
        {
            sched.thrds[i].proc = proc;
            sched.thrds[i].name = name;
            sched.thrds[i].stack_sz =
                (!stack_sz ? CONFIG_DEFAULT_STACK_SIZE : stack_sz);
            sched.thrds[i].arg = arg;
            sched.thrds[i].state = NEW;
            sched.thrds[i].depth = 0;
            memset(sched.thrds[i].exe_ctx, 0, sizeof(sched.thrds[i].exe_ctx));
            memset(sched.thrds[i].entry_ctx, 0, sizeof(sched.thrds[i].entry_ctx));

            sched.busy_n++;
            coop_dbg_log_cb("Thread #%d scheduled to run\n", i);
            break;
        }
    }
    return COOP_SUCCESS;
}

const char *coop_thread_name(void)
{
    return sched.thrds[sched.cur_thrd].name;
}

/**
 * @c new_state specifies a state to set before yielding (RUN, IDLE, WAIT).
 */
inline static void _yield(coop_thrd_state_t new_state)
{
    if (sched.thrds[sched.cur_thrd].state == NEW) {
        sched.thrds[sched.cur_thrd].state = new_state;

        /* thrd_pos_new: newly created thread context */
        if (!setjmp(sched.thrds[sched.cur_thrd].exe_ctx))
        {
            coop_dbg_log_cb("setjmp thrd_pos_new; thread #%d: NEW -> %s\n",
                sched.cur_thrd, _STATE_NAME(new_state));

            /* allocate thread stack */
            memset(alloca(sched.thrds[sched.cur_thrd].stack_sz), 0,
                sched.thrds[sched.cur_thrd].stack_sz);

            /* build new thread stack via recurrent scheduler service call */
            coop_sched_service();
        } else {
            /* return from scheduler; first run */
            coop_dbg_log_cb("Back to #%d thread (via thrd_pos_new)\n",
                sched.cur_thrd);
        }
    } else {
#ifdef COOP_DEBUG
        if (new_state != RUN) {
            coop_dbg_log_cb("Thread #%d: RUN -> %s\n",
                sched.cur_thrd, _STATE_NAME(new_state));
        }
#endif
        sched.thrds[sched.cur_thrd].state = new_state;

        /* thrd_pos_run: main-running thread context */
        if (!setjmp(sched.thrds[sched.cur_thrd].exe_ctx))
        {
            coop_dbg_log_cb("setjmp thrd_pos_run; back from #%d thread to "
                "scheduler: longjmp sched_pos_run\n", sched.cur_thrd);

            /* back to scheduler: sched_pos_run jump */
            longjmp(sched.exe_ctx, 1);
        } else {
            /* return from scheduler; regular run */
            coop_dbg_log_cb("Back to #%d thread (via thrd_pos_run)\n",
                sched.cur_thrd);
        }
    }
}

#ifdef CONFIG_OPT_IDLE
void coop_idle(coop_tick_t period)
{
    coop_thrd_state_t new_state = RUN;

    if (period > 0) {
        coop_dbg_log_cb("Thread #%d going idle for %lu ticks\n",
            sched.cur_thrd, (unsigned long)period);

        new_state = IDLE;
        sched.idle_n++;
        sched.thrds[sched.cur_thrd].idle_to = coop_tick_cb() + period;
    }
    _yield(new_state);
}
#else
void coop_yield(void)
{
    _yield(RUN);
}
#endif

#ifdef CONFIG_OPT_YIELD_AFTER
bool coop_yield_after(coop_tick_t after)
{
    if (COOP_IS_TICK_OVER(coop_tick_cb(), after))
    {
        coop_dbg_log_cb("Thread #%d yields after %lu tick\n",
            sched.cur_thrd, (unsigned long)after);

        _yield(RUN);
        return true;
    }
    return false;
}
#endif

#ifdef CONFIG_OPT_WAIT
bool coop_wait(int sem_id, coop_tick_t timeout)
{
    sched.thrds[sched.cur_thrd].sem_id = sem_id;
    sched.thrds[sched.cur_thrd].wait_flgs.notif = 0;
    if (timeout) {
        sched.thrds[sched.cur_thrd].wait_to = coop_tick_cb() + timeout;
        sched.thrds[sched.cur_thrd].wait_flgs.inf = 0;

        coop_dbg_log_cb("Thread #%d waiting with timeout %lu ticks; "
            "sem_id: %d\n", sched.cur_thrd, (unsigned long)timeout, sem_id);
    } else {
        sched.thrds[sched.cur_thrd].wait_to = 0;
        sched.thrds[sched.cur_thrd].wait_flgs.inf = 1;

        coop_dbg_log_cb("Thread #%d waiting infinitely; sem_id: %d\n",
            sched.cur_thrd, sem_id);
    }

    _yield(WAIT);

    if (sched.thrds[sched.cur_thrd].wait_flgs.notif != 0) {
        coop_dbg_log_cb("Thread #%d notified on sem_id: %d\n",
            sched.cur_thrd, sem_id);
        return true;
    } else {
        coop_dbg_log_cb("Thread #%d wait-timeout; sem_id: %d\n",
            sched.cur_thrd, sem_id);
        return false;
    }
}

void coop_notify(int sem_id)
{
    for (unsigned i = 0; i < CONFIG_MAX_THREADS; i++) {
        if (sched.thrds[i].state == WAIT && sched.thrds[i].sem_id == sem_id) {
            coop_dbg_log_cb("Thread #%d WAIT -> RUN "
                "(single notify on sem_id: %d)\n", i, sem_id);

            sched.thrds[i].wait_flgs.notif = 1;
            sched.thrds[i].state = RUN;
            break;
        }
    }
}

void coop_notify_all(int sem_id)
{
    for (unsigned i = 0; i < CONFIG_MAX_THREADS; i++) {
        if (sched.thrds[i].state == WAIT && sched.thrds[i].sem_id == sem_id) {
            coop_dbg_log_cb("Thread #%d WAIT -> RUN "
                "(all notify on sem_id)\n", i, sem_id);

            sched.thrds[i].wait_flgs.notif = 1;
            sched.thrds[i].state = RUN;
        }
    }
}
#endif

#ifdef __TEST__
bool coop_test_is_shallow()
{
    return (sched.depth == sched.thrds[sched.cur_thrd].depth);
}
#endif