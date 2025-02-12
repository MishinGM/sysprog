#include "libcoro.h"
#include "rlist.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <setjmp.h>
#include <signal.h>
#include <errno.h>
#include <string.h>


#ifndef CORO_F_DEFINED
typedef void *(*coro_f)(void *);
#define CORO_F_DEFINED
#endif

#define handle_error() do { \
    printf("Error %s\n", strerror(errno)); \
    exit(-1); \
} while(0)


enum coro_state {
    CORO_STATE_RUNNING,
    CORO_STATE_SUSPENDED,
    CORO_STATE_FINISHED,
};


struct coro {
    enum coro_state state;
    void *ret;
    void *stack;
    void *func_arg;
    coro_f func;
    sigjmp_buf ctx;
    struct coro *joiner;
    struct rlist link;        
    struct rlist active_link;  
};

struct coro_engine {
    struct coro sched;
    struct coro *this;
    struct rlist coros_running_now;
    struct rlist coros_running_next;
    struct rlist coros_pool;
    struct rlist coros_active;  
    size_t coro_count;
    sigjmp_buf start_point;
};


static void
coro_engine_create(struct coro_engine *e) {
    memset(e, 0, sizeof(*e));
    rlist_create(&e->sched.link);
    rlist_create(&e->coros_running_now);
    rlist_create(&e->coros_running_next);
    rlist_create(&e->coros_pool);
    rlist_create(&e->coros_active);
}


static void
coro_engine_resume_next(struct coro_engine *e) {
    if (rlist_empty(&e->coros_running_now))
        return;
    struct coro *to = rlist_shift_entry(&e->coros_running_now, struct coro, link);
    struct coro *from = e->this;
    if (!from) {
        printf("No current coroutine\n");
        exit(-1);
    }
    e->this = NULL;
    if (sigsetjmp(from->ctx, 0) == 0)
        siglongjmp(to->ctx, 1);
    e->this = from;
}


static void
coro_engine_suspend(struct coro_engine *e) {
    struct coro *c = e->this;
    if (!c) {
        printf("Error: no coroutines\n");
        exit(-1);
    }
    c->state = CORO_STATE_SUSPENDED;
    coro_engine_resume_next(e);
}


static void
coro_engine_yield(struct coro_engine *e) {
    struct coro *c = e->this;
    if (!c) {
        printf("Error: no coroutines\n");
        exit(-1);
    }
    rlist_add_tail_entry(&e->coros_running_next, c, link);
    coro_engine_resume_next(e);
}


static void
coro_engine_wakeup(struct coro_engine *e, struct coro *c) {
    if (c->state == CORO_STATE_RUNNING)
        return;
    if (c->state == CORO_STATE_FINISHED)
        return;
    c->state = CORO_STATE_RUNNING;
    rlist_add_tail_entry(&e->coros_running_next, c, link);
}


static void
coro_engine_run(struct coro_engine *e) {
    while (true) {
        rlist_splice_tail(&e->coros_running_now, &e->coros_running_next);
        if (rlist_empty(&e->coros_running_now)) {
          
            if (!rlist_empty(&e->coros_active)) {
                struct rlist *p = e->coros_active.next;
                bool woke = false;
                while (p != &e->coros_active) {
                    struct coro *c = rlist_entry(p, struct coro, active_link);
                    if (c->state == CORO_STATE_SUSPENDED) {
                        coro_engine_wakeup(e, c);
                        woke = true;
                    }
                    p = p->next;
                }
                if (woke)
                    continue;
            }
            break;
        }
        e->this = &e->sched;
        rlist_add_tail_entry(&e->coros_running_now, &e->sched, link);
        coro_engine_resume_next(e);
        e->this = NULL;
    }
}

static void
coro_engine_destroy(struct coro_engine *e) {
    while (!rlist_empty(&e->coros_pool)) {
        struct coro *c = rlist_shift_entry(&e->coros_pool, struct coro, link);
        free(c->stack);
        free(c);
        e->coro_count--;
    }
}


static __thread struct coro_engine *g_eng_new = NULL;


static void
coro_body(int signum) {
    (void)signum;
    struct coro_engine *eng = g_eng_new;
    g_eng_new = NULL;
    struct coro *c = eng->this;
    eng->this = NULL;
    if (sigsetjmp(c->ctx, 0) == 0)
        siglongjmp(eng->start_point, 1);
    eng->this = c;
    while (1) {
        c->ret = c->func(c->func_arg);
        c->func = NULL;
        c->state = CORO_STATE_FINISHED;
        if (c->joiner)
            coro_engine_wakeup(eng, c->joiner);
        coro_engine_resume_next(eng);
    }
}

static struct coro *
coro_engine_spawn_new(struct coro_engine *eng, coro_f f, void *arg) {
    struct coro *c = malloc(sizeof(*c));
    c->state = CORO_STATE_RUNNING;
    c->ret = NULL;
    c->func = f;
    c->func_arg = arg;
    c->joiner = NULL;
    rlist_create(&c->link);
    rlist_create(&c->active_link);
   
    rlist_add_tail_entry(&eng->coros_active, c, active_link);
    
    int st_sz = 1024 * 1024;
    if (st_sz < SIGSTKSZ)
        st_sz = SIGSTKSZ;
    c->stack = malloc(st_sz);
    sigset_t news, olds, suss;
    sigemptyset(&news);
    sigaddset(&news, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &news, &olds) != 0)
        exit(-1);
    struct sigaction sa_new, sa_old;
    sa_new.sa_handler = coro_body;
    sa_new.sa_flags = SA_ONSTACK;
    sigemptyset(&sa_new.sa_mask);
    if (sigaction(SIGUSR2, &sa_new, &sa_old) != 0)
        exit(-1);
    stack_t st_old, st_new;
    st_new.ss_sp = c->stack;
    st_new.ss_size = st_sz;
    st_new.ss_flags = 0;
    if (sigaltstack(&st_new, &st_old) != 0)
        exit(-1);
    g_eng_new = eng;
    struct coro *old = eng->this;
    eng->this = c;
    sigemptyset(&suss);
    if (sigsetjmp(eng->start_point, 1) == 0) {
        raise(SIGUSR2);
        while (eng->this)
            sigsuspend(&suss);
    }
    g_eng_new = NULL;
    eng->this = old;
    if (sigaltstack(NULL, &st_new) != 0)
        exit(-1);
    st_new.ss_flags = SS_DISABLE;
    if (sigaltstack(&st_new, NULL) != 0)
        exit(-1);
    if (!(st_old.ss_flags & SS_DISABLE)) {
        if (sigaltstack(&st_old, NULL) != 0)
            exit(-1);
    }
    if (sigaction(SIGUSR2, &sa_old, NULL) != 0)
        exit(-1);
    if (sigprocmask(SIG_SETMASK, &olds, NULL) != 0)
        exit(-1);
    eng->coro_count++;
    rlist_add_tail_entry(&eng->coros_running_next, c, link);
    return c;
}


static struct coro *
coro_engine_spawn(struct coro_engine *eng, coro_f f, void *arg) {
    if (rlist_empty(&eng->coros_pool))
        return coro_engine_spawn_new(eng, f, arg);
    struct coro *c = rlist_shift_entry(&eng->coros_pool, struct coro, link);

    rlist_add_tail_entry(&eng->coros_active, c, active_link);
    c->state = CORO_STATE_RUNNING;
    c->func = f;
    c->func_arg = arg;
    rlist_add_tail_entry(&eng->coros_running_next, c, link);
    return c;
}

static void *
coro_engine_join(struct coro_engine *eng, struct coro *c) {
    if (!eng->this) {
        eng->this = &eng->sched;
    }
    c->joiner = eng->this;
    while (c->state == CORO_STATE_RUNNING ||
           c->state == CORO_STATE_SUSPENDED) {
        coro_engine_suspend(eng);
    }
    void *r = c->ret;
    c->ret = NULL;
  
    rlist_del_entry(c, active_link);
    rlist_add_entry(&eng->coros_pool, c, link);
    return r;
}


static struct coro_engine g_eng;


void
coro_sched_init(void) {
    coro_engine_create(&g_eng);
}

void
coro_sched_run(void) {
    coro_engine_run(&g_eng);
}

void
coro_sched_destroy(void) {
    coro_engine_destroy(&g_eng);
}

struct coro *
coro_this(void) {
    return g_eng.this;
}

struct coro *
coro_new(coro_f func, void *arg) {
    return coro_engine_spawn(&g_eng, func, arg);
}

void *
coro_join(struct coro *c) {
    return coro_engine_join(&g_eng, c);
}

void
coro_suspend(void) {
    coro_engine_suspend(&g_eng);
}

void
coro_yield(void) {
    coro_engine_yield(&g_eng);
}

void
coro_wakeup(struct coro *c) {
    coro_engine_wakeup(&g_eng, c);
}
