#include "corobus.h"
#include "libcoro.h"
#include "rlist.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static enum coro_bus_error_code gerr = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void) {
    return gerr;
}

void
coro_bus_errno_set(enum coro_bus_error_code e) {
    gerr = e;
}

struct data_vec {
    unsigned *d;
    size_t sz;
    size_t cap;
};

static void
dv_push_many(struct data_vec *v, const unsigned *src, size_t n) {
    if (v->sz + n > v->cap) {
        size_t nc = v->cap ? v->cap : 4;
        while (nc < v->sz + n)
            nc *= 2;
        v->d = realloc(v->d, nc * sizeof(*v->d));
        v->cap = nc;
    }
    memcpy(v->d + v->sz, src, n * sizeof(*v->d));
    v->sz += n;
}

static void
dv_push(struct data_vec *v, unsigned x) {
    dv_push_many(v, &x, 1);
}

static unsigned
dv_pop(struct data_vec *v) {
    unsigned x = v->d[0];
    v->sz--;
    memmove(v->d, v->d + 1, v->sz * sizeof(*v->d));
    return x;
}

static void
dv_pop_many(struct data_vec *v, unsigned *dst, size_t n) {
    memcpy(dst, v->d, n * sizeof(*dst));
    v->sz -= n;
    memmove(v->d, v->d + n, v->sz * sizeof(*v->d));
}

struct wq_item {
    struct rlist link;
    struct coro *coro;
};

struct wqueue {
    struct rlist list;
};


struct channel {
    size_t limit;
    struct data_vec vec;
    struct wqueue senders;
    struct wqueue receivers;
};

struct coro_bus {
    struct channel **chs;
    int count;
    int cap;
};

static struct channel*
bus_get(struct coro_bus *b, int i) {
    if (i < 0 || i >= b->count) {
        gerr = CORO_BUS_ERR_NO_CHANNEL;
        return NULL;
    }
    struct channel *c = b->chs[i];
    if (!c) {
        gerr = CORO_BUS_ERR_NO_CHANNEL;
        return NULL;
    }
    return c;
}

static void
wq_wakeup_one(struct wqueue *w) {
    if (!rlist_empty(&w->list)) {
        struct wq_item *it = rlist_first_entry(&w->list, struct wq_item, link);
        coro_wakeup(it->coro);
    }
}

static void
wq_wakeup_all(struct wqueue *w) {
    struct rlist *cur = w->list.next;
    while (cur != &w->list) {
        struct wq_item *it = rlist_entry(cur, struct wq_item, link);
        coro_wakeup(it->coro);
        cur = cur->next;
    }
}

static void
chan_destroy(struct coro_bus *b, int i) {
    struct channel *c = b->chs[i];
    if (!c) return;
    wq_wakeup_all(&c->senders);
    wq_wakeup_all(&c->receivers);
    free(c->vec.d);
    free(c);
    b->chs[i] = NULL;
}

struct coro_bus*
coro_bus_new(void) {
    return calloc(1, sizeof(struct coro_bus));
}

void
coro_bus_delete(struct coro_bus *b) {
    for (int i = 0; i < b->count; i++) {
        chan_destroy(b, i);
    }
    free(b->chs);
    free(b);
}

int
coro_bus_channel_open(struct coro_bus *b, size_t lim) {
    for (int i = 0; i < b->count; i++) {
        if (!b->chs[i]) {
            struct channel *c = calloc(1, sizeof(*c));
            c->limit = lim;
            rlist_create(&c->senders.list);
            rlist_create(&c->receivers.list);
            b->chs[i] = c;
            return i;
        }
    }
    if (b->count == b->cap) {
        int nc = b->cap ? b->cap * 2 : 4;
        b->chs = realloc(b->chs, sizeof(*b->chs) * nc);
        for (int k = b->cap; k < nc; k++)
            b->chs[k] = NULL;
        b->cap = nc;
    }
    struct channel *c = calloc(1, sizeof(*c));
    c->limit = lim;
    rlist_create(&c->senders.list);
    rlist_create(&c->receivers.list);
    b->chs[b->count] = c;
    return b->count++;
}

void
coro_bus_channel_close(struct coro_bus *b, int i) {
    struct channel *c = bus_get(b, i);
    if (!c) return;
    chan_destroy(b, i);
}

static int
try_send_impl(struct channel *c, unsigned x) {
    if (c->vec.sz >= c->limit) {
        gerr = CORO_BUS_ERR_WOULD_BLOCK;
        return -1;
    }
    dv_push(&c->vec, x);
    wq_wakeup_one(&c->receivers);
    return 0;
}

int
coro_bus_try_send(struct coro_bus *b, int i, unsigned x) {
    struct channel *c = bus_get(b, i);
    if (!c) return -1;
    return try_send_impl(c, x);
}

int
coro_bus_send(struct coro_bus *b, int i, unsigned x) {
    while (1) {
        struct channel *c = bus_get(b, i);
        if (!c) return -1;
        if (!try_send_impl(c, x))
            return 0;
        if (gerr != CORO_BUS_ERR_WOULD_BLOCK)
            return -1;
        struct wq_item w;
        w.coro = coro_this();
        rlist_add_tail_entry(&c->senders.list, &w, link);
        coro_suspend();
        rlist_del_entry(&w, link);
    }
}

static int
try_recv_impl(struct channel *c, unsigned *val) {
    if (!c->vec.sz) {
        gerr = CORO_BUS_ERR_WOULD_BLOCK;
        return -1;
    }
    *val = dv_pop(&c->vec);
    wq_wakeup_one(&c->senders);
    return 0;
}

int
coro_bus_try_recv(struct coro_bus *b, int i, unsigned *val) {
    struct channel *c = bus_get(b, i);
    if (!c) return -1;
    return try_recv_impl(c, val);
}

int
coro_bus_recv(struct coro_bus *b, int i, unsigned *val) {
    while (1) {
        struct channel *c = bus_get(b, i);
        if (!c) return -1;
        if (!try_recv_impl(c, val))
            return 0;
        if (gerr != CORO_BUS_ERR_WOULD_BLOCK)
            return -1;
        struct wq_item w;
        w.coro = coro_this();
        rlist_add_tail_entry(&c->receivers.list, &w, link);
        coro_suspend();
        rlist_del_entry(&w, link);
    }
}

#if NEED_BROADCAST
int
coro_bus_try_broadcast(struct coro_bus *b, unsigned x) {
    int cnum = 0;
    for (int i = 0; i < b->count; i++)
        if (b->chs[i])
            cnum++;
    if (!cnum) {
        gerr = CORO_BUS_ERR_NO_CHANNEL;
        return -1;
    }
    for (int i = 0; i < b->count; i++) {
        struct channel *c = b->chs[i];
        if (!c)
            continue;
        if (c->vec.sz >= c->limit) {
            gerr = CORO_BUS_ERR_WOULD_BLOCK;
            return -1;
        }
    }
    for (int i = 0; i < b->count; i++) {
        struct channel *c = b->chs[i];
        if (!c)
            continue;
        dv_push(&c->vec, x);
        wq_wakeup_one(&c->receivers);
    }
    return 0;
}

int
coro_bus_broadcast(struct coro_bus *b, unsigned x) {
    while (1) {
        int cnum = 0;
        for (int i = 0; i < b->count; i++)
            if (b->chs[i])
                cnum++;
        if (!cnum) {
            gerr = CORO_BUS_ERR_NO_CHANNEL;
            return -1;
        }
        int full = 0;
        for (int i = 0; i < b->count; i++) {
            struct channel *c = b->chs[i];
            if (c && c->vec.sz >= c->limit)
                full++;
        }
        if (!full) {
            for (int i = 0; i < b->count; i++) {
                struct channel *c = b->chs[i];
                if (!c)
                    continue;
                dv_push(&c->vec, x);
                wq_wakeup_one(&c->receivers);
            }
            return 0;
        }
        struct wq_item *arr = malloc(sizeof(*arr) * full);
        int idx = 0;
        for (int i = 0; i < b->count; i++) {
            struct channel *c = b->chs[i];
            if (c && c->vec.sz >= c->limit) {
                arr[idx].coro = coro_this();
                rlist_add_tail_entry(&c->senders.list, &arr[idx], link);
                idx++;
            }
        }
        coro_suspend();
        for (int i = 0; i < idx; i++) {
            rlist_del_entry(&arr[i], link);
        }
        free(arr);
    }
}
#endif

#if NEED_BATCH
static int
try_send_v_impl(struct channel *c, const unsigned *arr, unsigned n) {
    size_t freec = c->limit - c->vec.sz;
    if (!freec) {
        gerr = CORO_BUS_ERR_WOULD_BLOCK;
        return -1;
    }
    unsigned r = (freec < n ? freec : n);
    dv_push_many(&c->vec, arr, r);
    wq_wakeup_one(&c->receivers);
    return r;
}

int
coro_bus_try_send_v(struct coro_bus *b, int i, const unsigned *arr, unsigned n) {
    struct channel *c = bus_get(b, i);
    if (!c) return -1;
    return try_send_v_impl(c, arr, n);
}

int
coro_bus_send_v(struct coro_bus *b, int i, const unsigned *arr, unsigned n) {
    while (1) {
        struct channel *c = bus_get(b, i);
        if (!c) return -1;
        int rc = try_send_v_impl(c, arr, n);
        if (rc >= 0)
            return rc;
        if (gerr != CORO_BUS_ERR_WOULD_BLOCK)
            return -1;
        struct wq_item w;
        w.coro = coro_this();
        rlist_add_tail_entry(&c->senders.list, &w, link);
        coro_suspend();
        rlist_del_entry(&w, link);
    }
}

static int
try_recv_v_impl(struct channel *c, unsigned *arr, unsigned cap) {
    if (!c->vec.sz) {
        gerr = CORO_BUS_ERR_WOULD_BLOCK;
        return -1;
    }
    unsigned got = (c->vec.sz < cap ? c->vec.sz : cap);
    dv_pop_many(&c->vec, arr, got);
    wq_wakeup_one(&c->senders);
    return got;
}

int
coro_bus_try_recv_v(struct coro_bus *b, int i, unsigned *arr, unsigned cap) {
    struct channel *c = bus_get(b, i);
    if (!c) return -1;
    return try_recv_v_impl(c, arr, cap);
}

int
coro_bus_recv_v(struct coro_bus *b, int i, unsigned *arr, unsigned cap) {
    while (1) {
        struct channel *c = bus_get(b, i);
        if (!c) return -1;
        int rc = try_recv_v_impl(c, arr, cap);
        if (rc >= 0)
            return rc;
        if (gerr != CORO_BUS_ERR_WOULD_BLOCK)
            return -1;
        struct wq_item w;
        w.coro = coro_this();
        rlist_add_tail_entry(&c->receivers.list, &w, link);
        coro_suspend();
        rlist_del_entry(&w, link);
    }
}
#endif
