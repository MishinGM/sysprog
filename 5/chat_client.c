#define _POSIX_C_SOURCE 200809L
#include "chat_client.h"
#include "chat.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void *xrealloc(void *p, size_t sz) {
  void *n = realloc(p, sz ? sz : 1);
  if (!n)
    abort();
  return n;
}

static char *str_trim(const char *s, size_t len, size_t *out_len) {
  const char *b = s, *e = s + len;
  while (b < e && isspace((unsigned char)*b))
    ++b;
  while (e > b && isspace((unsigned char)e[-1]))
    --e;
  *out_len = e - b;
  char *r = malloc(*out_len + 1);
  memcpy(r, b, *out_len);
  r[*out_len] = 0;
  return r;
}

struct mnode {
  struct chat_message *m;
  struct mnode *n;
};

static void mq_push(struct mnode **h, struct mnode **t,
                    struct chat_message *m) {
  struct mnode *n = malloc(sizeof *n);
  n->m = m;
  n->n = NULL;
  if (*t)
    (*t)->n = n;
  else
    *h = n;
  *t = n;
}

static struct chat_message *mq_pop(struct mnode **h, struct mnode **t) {
  struct mnode *n = *h;
  if (!n)
    return NULL;
  *h = n->n;
  if (!*h)
    *t = NULL;
  struct chat_message *m = n->m;
  free(n);
  return m;
}

struct chat_client {
  int sock;
  char *in;
  size_t in_cap, in_len;
  char *out;
  size_t out_cap, out_len, out_pos;
  char *pend;
  size_t pend_cap, pend_len;
  struct mnode *hq, *tq;
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
  if (!c)
    return;
  if (c->sock >= 0)
    close(c->sock);
  free(c->in);
  free(c->out);
  free(c->pend);
#if NEED_AUTHOR
  free(c->name);
#endif
  struct chat_message *m;
  while ((m = mq_pop(&c->hq, &c->tq)))
    chat_message_delete(m);
  free(c);
}

static int split_addr(const char *addr, char **h, char **p) {
  const char *c = strrchr(addr, ':');
  if (!c || c == addr || c[1] == '\0')
    return CHAT_ERR_NO_ADDR;
  *h = strndup(addr, c - addr);
  *p = strdup(c + 1);
  return 0;
}

int chat_client_connect(struct chat_client *c, const char *addr) {
  if (c->sock >= 0)
    return CHAT_ERR_ALREADY_STARTED;
  char *host = NULL, *port = NULL;
  if (split_addr(addr, &host, &port) != 0)
    return CHAT_ERR_NO_ADDR;
  struct addrinfo hints = {0}, *ai = NULL;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &ai) != 0) {
    free(host);
    free(port);
    return CHAT_ERR_NO_ADDR;
  }
  free(host);
  free(port);
  int s = -1;
  for (struct addrinfo *p = ai; p; p = p->ai_next) {
    s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (s < 0)
      continue;
    set_nonblock(s);
    if (connect(s, p->ai_addr, p->ai_addrlen) == 0 || errno == EINPROGRESS)
      break;
    close(s);
    s = -1;
  }
  freeaddrinfo(ai);
  if (s < 0)
    return CHAT_ERR_SYS;
  c->sock = s;
#if NEED_AUTHOR
  size_t n = strlen(c->name);
  c->out_cap = n + 1;
  c->out = malloc(c->out_cap);
  memcpy(c->out, c->name, n);
  c->out[n++] = '\n';
  c->out_len = n;
#endif
  return 0;
}

static void out_append(struct chat_client *c, const char *d, size_t l) {
  if (c->out_len + l > c->out_cap) {
    c->out_cap = (c->out_len + l) * 2;
    c->out = xrealloc(c->out, c->out_cap);
  }
  memcpy(c->out + c->out_len, d, l);
  c->out_len += l;
}

static void produce_msg(struct chat_client *c, const char *b, size_t l) {
  if (l == 0)
    return;
  struct chat_message *m = calloc(1, sizeof *m);
#if NEED_AUTHOR
  const char *sp = memchr(b, ' ', l);
  if (sp) {
    m->author = strndup(b, sp - b);
    const char *bd = sp + 1;
    while (bd < b + l && isspace((unsigned char)*bd))
      ++bd;
    m->data = strndup(bd, b + l - bd);
  } else {
    m->author = strdup("?");
    m->data = strndup(b, l);
  }
#else
  m->data = strndup(b, l);
#endif
  mq_push(&c->hq, &c->tq, m);
}

static void consume_in(struct chat_client *c) {
  char *p = c->in;
  while (true) {
    char *nl = memchr(p, '\n', c->in_len - (p - c->in));
    if (!nl)
      break;
    size_t l = nl - p;
    if (l && p[l - 1] == '\r')
      --l;
    produce_msg(c, p, l);
    p = nl + 1;
  }
  size_t r = c->in + c->in_len - p;
  if (r && p != c->in)
    memmove(c->in, p, r);
  c->in_len = r;
}

int chat_client_feed(struct chat_client *c, const char *buf, uint32_t sz) {
  if (c->sock < 0)
    return CHAT_ERR_NOT_STARTED;
  if (c->pend_len + sz > c->pend_cap) {
    c->pend_cap = (c->pend_len + sz) * 2;
    c->pend = xrealloc(c->pend, c->pend_cap);
  }
  memcpy(c->pend + c->pend_len, buf, sz);
  c->pend_len += sz;
  char *p = c->pend;
  while (true) {
    char *nl = memchr(p, '\n', c->pend_len - (p - c->pend));
    if (!nl)
      break;
    size_t l = nl - p;
    size_t tl = 0;
    char *t = str_trim(p, l, &tl);
    if (tl) {
      out_append(c, t, tl);
      out_append(c, "\n", 1);
    }
    free(t);
    p = nl + 1;
  }
  size_t r = c->pend + c->pend_len - p;
  if (r && p != c->pend)
    memmove(c->pend, p, r);
  c->pend_len = r;
  return 0;
}

int chat_client_get_events(const struct chat_client *c) {
  if (c->sock < 0)
    return 0;
  int ev = CHAT_EVENT_INPUT;
  if (c->out_len > c->out_pos)
    ev |= CHAT_EVENT_OUTPUT;
  return ev;
}

int chat_client_get_descriptor(const struct chat_client *c) { return c->sock; }

int chat_client_update(struct chat_client *c, double timeout) {
    if (c->sock < 0)
        return CHAT_ERR_NOT_STARTED;

    struct pollfd pfd = {
        .fd = c->sock,
        .events = POLLIN | (c->out_pos < c->out_len ? POLLOUT : 0),
        .revents = 0
    };
    int ms = timeout < 0 ? -1 : (int)(timeout * 1000);
    int rc = poll(&pfd, 1, ms);
    if (rc < 0)
        return CHAT_ERR_SYS;
    if (rc == 0)
        return CHAT_ERR_TIMEOUT;

    bool prog = false;

   
    if ((pfd.revents & POLLOUT) && c->out_pos < c->out_len) {
        ssize_t n = send(c->sock, c->out + c->out_pos, c->out_len - c->out_pos, MSG_NOSIGNAL);
        if (n > 0) {
            c->out_pos += n;
            if (c->out_pos == c->out_len)
                c->out_pos = c->out_len = 0;
            prog = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK)
            return CHAT_ERR_SYS;
    }

  
    if (pfd.revents & POLLIN) {
        if (c->in_cap - c->in_len < 4096) {
            c->in_cap = c->in_cap ? c->in_cap * 2 : 4096;
            c->in = xrealloc(c->in, c->in_cap);
        }
        ssize_t n = recv(c->sock, c->in + c->in_len, c->in_cap - c->in_len, 0);
        if (n == 0)
            return CHAT_ERR_SYS; 
        if (n > 0) {
            c->in_len += n;
            consume_in(c);
            prog = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK)
            return CHAT_ERR_SYS;
    }

    return prog ? 0 : CHAT_ERR_TIMEOUT;
}


struct chat_message *chat_client_pop_next(struct chat_client *c) {
  return mq_pop(&c->hq, &c->tq);
}