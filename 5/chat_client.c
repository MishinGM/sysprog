#define _POSIX_C_SOURCE 200809L
#include <alloca.h>
#include "chat.h"
#include "chat_client.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void *xrealloc(void *ptr, size_t sz) {
    void *res = realloc(ptr, sz ? sz : 1);
    if (!res) abort();
    return res;
}

static char *str_trim(const char *s, size_t len, size_t *out_len) {
    const char *beg = s, *end = s + len;
    while (beg < end && isspace((unsigned char)*beg)) ++beg;
    while (end > beg && isspace((unsigned char)end[-1])) --end;
    *out_len = (size_t)(end - beg);
    char *res = malloc(*out_len + 1);
    if (!res) abort();
    memcpy(res, beg, *out_len);
    res[*out_len] = '\0';
    return res;
}

struct mnode { struct chat_message *m; struct mnode *next; };

static void mq_push(struct mnode **head, struct mnode **tail, struct chat_message *m) {
    struct mnode *n = malloc(sizeof *n);
    n->m = m; n->next = NULL;
    if (*tail) (*tail)->next = n;
    else *head = n;
    *tail = n;
}

static struct chat_message *mq_pop(struct mnode **head, struct mnode **tail) {
    struct mnode *n = *head;
    if (!n) return NULL;
    *head = n->next;
    if (!*head) *tail = NULL;
    struct chat_message *m = n->m;
    free(n);
    return m;
}

struct chat_client {
    int sock;
    char   *in;   size_t in_cap, in_len;
    char   *out;  size_t out_cap, out_len, out_pos;
    char   *pend; size_t pend_cap, pend_len;
    struct mnode *q_head, *q_tail;
#if NEED_AUTHOR
    char *name;
#endif
};

struct chat_client *chat_client_new(const char *name) {
    struct chat_client *c = calloc(1, sizeof *c);
    c->sock = -1;
#if NEED_AUTHOR
    c->name = strdup(name);
#endif
    return c;
}

void chat_client_delete(struct chat_client *c) {
    if (!c) return;
    if (c->sock >= 0) close(c->sock);
    free(c->in);
    free(c->out);
    free(c->pend);
#if NEED_AUTHOR
    free(c->name);
#endif
    struct chat_message *m;
    while ((m = mq_pop(&c->q_head, &c->q_tail))) chat_message_delete(m);
    free(c);
}

static int split_addr(const char *addr, char **host, char **port) {
    const char *colon = strrchr(addr, ':');
    if (!colon || colon == addr || colon[1] == '\0')
        return CHAT_ERR_NO_ADDR;
    *host = strndup(addr, colon - addr);
    *port = strdup(colon + 1);
    return 0;
}

int chat_client_connect(struct chat_client *c, const char *addr) {
    if (c->sock >= 0) return CHAT_ERR_ALREADY_STARTED;
    char *host = NULL, *port = NULL;
    if (split_addr(addr, &host, &port) != 0) return CHAT_ERR_NO_ADDR;

    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (getaddrinfo(host, port, &hints, &ai) != 0) {
        free(host); free(port);
        return CHAT_ERR_NO_ADDR;
    }
    free(host); free(port);

    int s = -1;
    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) continue;
        set_nonblock(s);
        if (connect(s, p->ai_addr, p->ai_addrlen) == 0 || errno == EINPROGRESS)
            break;
        close(s);
        s = -1;
    }
    freeaddrinfo(ai);
    if (s < 0) return CHAT_ERR_SYS;

    c->sock = s;

#if NEED_AUTHOR
    size_t nlen = strlen(c->name);
    c->out_cap = nlen + 1;
    c->out     = malloc(c->out_cap);
    memcpy(c->out, c->name, nlen);
    c->out[nlen++] = '\n';
    c->out_len = nlen;
#endif

    return 0;
}

static void out_append(struct chat_client *c, const char *data, size_t len) {
    if (c->out_len + len > c->out_cap) {
        c->out_cap = (c->out_len + len) * 2;
        c->out = xrealloc(c->out, c->out_cap);
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
}

static void produce_msg(struct chat_client *c, const char *line, size_t len) {
    if (len == 0) return;
    struct chat_message *m = calloc(1, sizeof *m);
#if NEED_AUTHOR
    const char *sp = memchr(line, ' ', len);
    if (sp) {
        m->author = strndup(line, sp - line);
        const char *body = sp + 1;
        while (body < line + len && isspace((unsigned char)*body)) ++body;
        m->data = strndup(body, (line + len) - body);
    } else {
        m->author = strdup("?");
        m->data   = strndup(line, len);
    }
#else
    m->data = strndup(line, len);
#endif
    mq_push(&c->q_head, &c->q_tail, m);
}

static void consume_in(struct chat_client *c) {
    char *ptr = c->in;
    while (true) {
        char *nl = memchr(ptr, '\n', c->in_len - (ptr - c->in));
        if (!nl) break;
        size_t line_len = (size_t)(nl - ptr);
        if (line_len && ptr[line_len - 1] == '\r') --line_len;
        produce_msg(c, ptr, line_len);
        ptr = nl + 1;
    }
    size_t rem = c->in + c->in_len - ptr;
    if (rem && ptr != c->in) memmove(c->in, ptr, rem);
    c->in_len = rem;
}

int chat_client_feed(struct chat_client *c, const char *buf, uint32_t sz) {
    if (c->sock < 0) return CHAT_ERR_NOT_STARTED;
    if (c->pend_len + sz > c->pend_cap) {
        c->pend_cap = (c->pend_len + sz) * 2;
        c->pend = xrealloc(c->pend, c->pend_cap);
    }
    memcpy(c->pend + c->pend_len, buf, sz);
    c->pend_len += sz;

    char *ptr = c->pend;
    while (true) {
        char *nl = memchr(ptr, '\n', c->pend_len - (ptr - c->pend));
        if (!nl) break;
        size_t part_len = (size_t)(nl - ptr);
        size_t trim_len = 0;
        char *trim = str_trim(ptr, part_len, &trim_len);
        if (trim_len) {
            out_append(c, trim, trim_len);
            out_append(c, "\n", 1);
        }
        free(trim);
        ptr = nl + 1;
    }
    size_t rem = c->pend + c->pend_len - ptr;
    if (rem && ptr != c->pend) memmove(c->pend, ptr, rem);
    c->pend_len = rem;
    return 0;
}

int chat_client_get_events(const struct chat_client *c) {
    if (c->sock < 0) return 0;
    int ev = CHAT_EVENT_INPUT;
    if (c->out_len > c->out_pos) ev |= CHAT_EVENT_OUTPUT;
    return ev;
}

int chat_client_get_descriptor(const struct chat_client *c) {
    return c->sock;
}

int chat_client_update(struct chat_client *c, double timeout) {
    if (c->sock < 0) return CHAT_ERR_NOT_STARTED;

    bool prog = false;
    if (c->out_pos < c->out_len) {
        ssize_t n = send(c->sock, c->out + c->out_pos,
                         c->out_len - c->out_pos, MSG_NOSIGNAL);
        if (n > 0) {
            c->out_pos += (size_t)n;
            prog = true;
            if (c->out_pos == c->out_len) c->out_len = c->out_pos = 0;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return CHAT_ERR_SYS;
        }
    }
    if (c->in_cap - c->in_len < 4096) {
        c->in_cap = c->in_cap ? c->in_cap * 2 : 4096;
        c->in = xrealloc(c->in, c->in_cap);
    }
    ssize_t r = recv(c->sock, c->in + c->in_len,
                     c->in_cap - c->in_len, 0);
    if (r > 0) {
        c->in_len += (size_t)r;
        consume_in(c);
        prog = true;
    } else if (r == 0) {
        return CHAT_ERR_SYS;
    } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return CHAT_ERR_SYS;
    }
    if (prog || c->q_head) return 0;

    struct pollfd pfd = {
        .fd     = c->sock,
        .events = chat_events_to_poll_events(chat_client_get_events(c))
    };
    int tout = timeout < 0 ? -1 : (int)(timeout * 1000);
    int rc = poll(&pfd, 1, tout);
    if (rc < 0) return CHAT_ERR_SYS;
    if (rc == 0) return CHAT_ERR_TIMEOUT;

    if ((pfd.revents & POLLOUT) && c->out_pos < c->out_len) {
        ssize_t n = send(c->sock, c->out + c->out_pos,
                         c->out_len - c->out_pos, MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return CHAT_ERR_SYS;
        if (n > 0) {
            c->out_pos += (size_t)n;
            if (c->out_pos == c->out_len) c->out_pos = c->out_len = 0;
        }
    }
    if (pfd.revents & POLLIN) {
        if (c->in_cap - c->in_len < 4096) {
            c->in_cap *= 2;
            c->in = xrealloc(c->in, c->in_cap);
        }
        ssize_t n2 = recv(c->sock, c->in + c->in_len,
                          c->in_cap - c->in_len, 0);
        if (n2 == 0) return CHAT_ERR_SYS;
        if (n2 < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return CHAT_ERR_SYS;
        if (n2 > 0) {
            c->in_len += (size_t)n2;
            consume_in(c);
            return 0;
        }
    }
    return CHAT_ERR_TIMEOUT;
}

struct chat_message *chat_client_pop_next(struct chat_client *c) {
    return mq_pop(&c->q_head, &c->q_tail);
}
