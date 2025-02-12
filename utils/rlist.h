#ifndef RLIST_H
#define RLIST_H

#include <stddef.h>

struct rlist {
    struct rlist *prev;
    struct rlist *next;
};

#define rlist_create(list) do { \
    (list)->next = (list);      \
    (list)->prev = (list);      \
} while (0)

#define rlist_empty(list) ((list)->next == (list))

#define rlist_entry(link, type, member) \
    ((type *)((char *)(link) - offsetof(type, member)))

static inline void rlist_insert(struct rlist *prev, struct rlist *next, struct rlist *item) {
    item->next = next;
    item->prev = prev;
    prev->next = item;
    next->prev = item;
}

static inline void rlist_del(struct rlist *item) {
    struct rlist *prev = item->prev;
    struct rlist *next = item->next;
    prev->next = next;
    next->prev = prev;
}

#define rlist_add_tail_entry(head, item, member) do { \
    rlist_insert((head)->prev, (head), &(item)->member); \
} while (0)

#define rlist_del_entry(item, member) \
    rlist_del(&(item)->member)

#define rlist_first_entry(head, type, member) \
    rlist_entry((head)->next, type, member)

#define rlist_shift_entry(head, type, member) \
({ \
    type *__obj = rlist_empty(head) ? NULL \
        : rlist_entry((head)->next, type, member); \
    if (__obj != NULL) \
        rlist_del(&__obj->member); \
    __obj; \
})

#define rlist_add_entry(head, item, member) \
    rlist_insert((head), (head)->next, &(item)->member)

static inline void rlist_splice_tail(struct rlist *dst, struct rlist *src) {
    if (rlist_empty(src)) return;
    struct rlist *dst_last = dst->prev;
    struct rlist *src_first = src->next;
    struct rlist *src_last = src->prev;
    dst_last->next = src_first;
    src_first->prev = dst_last;
    src_last->next = dst;
    dst->prev = src_last;
    rlist_create(src);
}

#endif
