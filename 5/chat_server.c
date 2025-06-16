#define _POSIX_C_SOURCE 200809L
#include "chat_server.h"
#include "chat.h"
#include <alloca.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

struct qn {
  struct chat_message *m;
  struct qn *n;
};

static void q_put(struct qn **h, struct qn **t, struct chat_message *m) {
  struct qn *n = malloc(sizeof *n);
  n->m = m;
  n->n = NULL;
  if (*t)
    (*t)->n = n;
  else
    *h = n;
  *t = n;
}

static struct chat_message *q_pop(struct qn **h, struct qn **t) {
  struct qn *n = *h;
  if (!n)
    return NULL;
  *h = n->n;
  if (!*h)
    *t = NULL;
  struct chat_message *m = n->m;
  free(n);
  return m;
}

struct peer {
  int sock;
  char *in;
  size_t icap, ilen;
  char *out;
  size_t ocap, opos, olen;
#if NEED_AUTHOR
  char *name;
  bool has_name;
#endif
  struct peer *next;
};

static struct peer *peer_new(int s) {
  struct peer *p = calloc(1, sizeof *p);
  p->sock = s;
#if NEED_AUTHOR
  p->has_name = false;
#endif
  return p;
}

static void peer_del(struct peer *p) {
  if (p->sock >= 0)
    close(p->sock);
  free(p->in);
  free(p->out);
#if NEED_AUTHOR
  free(p->name);
#endif
  free(p);
}

static void peer_enqueue(struct peer *p, const char *buf, size_t len) {
  if (p->olen + len > p->ocap) {
    size_t c = p->ocap ? p->ocap * 2 : 4096;
    while (c < p->olen + len)
      c *= 2;
    p->out = xrealloc(p->out, c);
    p->ocap = c;
  }
  memcpy(p->out + p->olen, buf, len);
  p->olen += len;
}

struct chat_server {
  int lsock;
  struct peer *peers;
  struct qn *hq, *tq;
  char *sin;
  size_t sin_cap, sin_len;
  int updates;
};

static void chat_server_clear_queue(struct chat_server *s) {
  struct chat_message *m;
  while ((m = chat_server_pop_next(s)))
    chat_message_delete(m);
}

struct chat_server *chat_server_new(void) {
  struct chat_server *s = calloc(1, sizeof *s);
  s->lsock = -1;
  s->updates = 0;
  return s;
}
void chat_server_delete(struct chat_server *s) {
  if (!s)
    return;
  if (s->lsock >= 0)
    close(s->lsock);
  for (struct peer *p = s->peers, *n; p; p = n) {
    n = p->next;
    peer_del(p);
  }
  chat_server_clear_queue(s);
  free(s->sin);
  free(s);
}

int chat_server_listen(struct chat_server *s, uint16_t port) {
  if (s->lsock >= 0)
    return CHAT_ERR_ALREADY_STARTED;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return CHAT_ERR_SYS;
  set_nonblock(fd);
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_port = htons(port),
                          .sin_addr = {.s_addr = htonl(INADDR_ANY)}};

  if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return errno == EADDRINUSE ? CHAT_ERR_PORT_BUSY : CHAT_ERR_SYS;
  }
  if (listen(fd, 128) != 0) {
    close(fd);
    return CHAT_ERR_SYS;
  }
  s->lsock = fd;
  return 0;
}

int chat_server_get_socket(const struct chat_server *s) { return s->lsock; }

int chat_server_get_events(const struct chat_server *s) {
  if (s->lsock < 0)
    return 0;
  int ev = CHAT_EVENT_INPUT;
  for (struct peer *p = s->peers; p; p = p->next) {
    if (p->olen > p->opos) {
      ev |= CHAT_EVENT_OUTPUT;
      break;
    }
  }
  return ev;
}

static void message_store(struct chat_server *s, struct peer *f, const char *d,
                          size_t l) {
  struct chat_message *m = calloc(1, sizeof *m);
  m->data = strndup(d, l);
#if NEED_AUTHOR
  m->author = strdup(f ? f->name : "server");
#endif
  q_put(&s->hq, &s->tq, m);
}

static void broadcast(struct chat_server *s, struct peer *f, const char *d,
                      size_t l) {
  char *wire;
  size_t wl;
#if NEED_AUTHOR
  const char *a = f ? f->name : "server";
  size_t al = strlen(a);
  wl = al + 1 + l + 1;
  wire = malloc(wl);
  memcpy(wire, a, al);
  wire[al] = ' ';
  memcpy(wire + al + 1, d, l);
  wire[wl - 1] = '\n';
#else
  wl = l + 1;
  wire = malloc(wl);
  memcpy(wire, d, l);
  wire[wl - 1] = '\n';
#endif
  for (struct peer *p = s->peers; p; p = p->next) {
    if (p != f)
      peer_enqueue(p, wire, wl);
  }
  free(wire);
}

static void peer_got_line(struct chat_server *s, struct peer *p, const char *b,
                          size_t l) {
#if NEED_AUTHOR
  if (!p->has_name) {
    p->name = strndup(b, l);
    p->has_name = true;
    return;
  }
#endif
  size_t clen;
  char *c = str_trim(b, l, &clen);
  if (clen) {
    message_store(s, p, c, clen);
    broadcast(s, p, c, clen);
  }
  free(c);
}

static void peer_consume_in(struct chat_server *s, struct peer *p) {
  char *b = p->in;
  while (true) {
    char *nl = memchr(b, '\n', p->ilen - (b - p->in));
    if (!nl)
      break;
    size_t ln = nl - b;
    if (ln && b[ln - 1] == '\r')
      --ln;
    peer_got_line(s, p, b, ln);
    b = nl + 1;
  }
  size_t rem = p->in + p->ilen - b;
  if (rem && b != p->in)
    memmove(p->in, b, rem);
  p->ilen = rem;
}

static bool peer_write(struct peer *p) {
  bool prog = false;
  while (p->opos < p->olen) {
    ssize_t n =
        send(p->sock, p->out + p->opos, p->olen - p->opos, MSG_NOSIGNAL);
    if (n > 0) {
      p->opos += n;
      prog = true;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    } else {
      p->opos = p->olen = 0;
      break;
    }
  }
  if (p->opos == p->olen)
    p->opos = p->olen = 0;
  return prog;
}

int chat_server_update(struct chat_server *s, double timeout) {
  if (s->lsock < 0)
    return CHAT_ERR_NOT_STARTED;

  bool prog = false;

  size_t npeer = 0;
  for (struct peer *p = s->peers; p; p = p->next)
    ++npeer;

  struct pollfd *pf = alloca(sizeof(*pf) * (npeer + 1));
  memset(pf, 0, sizeof(*pf) * (npeer + 1));

  pf[0].fd = s->lsock;
  pf[0].events = POLLIN;

  size_t i = 1;
  for (struct peer *p = s->peers; p; p = p->next, ++i) {
    pf[i].fd = p->sock;
    pf[i].events = POLLIN;
    if (p->olen > p->opos)
      pf[i].events |= POLLOUT;
  }

  int ms = timeout < 0 ? -1 : (int)(timeout * 1000);
  if (poll(pf, npeer + 1, ms) < 0)
    return CHAT_ERR_SYS;

  for (;;) {
    int cs = accept(s->lsock, NULL, NULL);
    if (cs < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      return CHAT_ERR_SYS;
    }
    set_nonblock(cs);
    struct peer *p = peer_new(cs);
    p->next = s->peers;
    s->peers = p;
    prog = true;
  }

  struct peer **pp = &s->peers;
  i = 0;
  while (i < npeer && *pp) {
    struct peer *p = *pp;
    struct pollfd *f = &pf[1 + i];

    if (f->revents & (POLLERR | POLLHUP)) {
      *pp = p->next;
      peer_del(p);
      prog = true;
      ++i;
      continue;
    }

    if (f->revents & POLLIN) {
      if (p->icap - p->ilen < 4096) {
        size_t cap = p->icap ? p->icap * 2 : 4096;
        p->in = xrealloc(p->in, cap);
        p->icap = cap;
      }
      ssize_t n = recv(p->sock, p->in + p->ilen, p->icap - p->ilen, 0);
      if (n > 0) {
        p->ilen += n;
        peer_consume_in(s, p);
        prog = true;
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        *pp = p->next;
        peer_del(p);
        prog = true;
        ++i;  
        continue;
      }
    }

    if ((f->revents & POLLOUT) && p->olen > p->opos) {
    prog |= peer_write(p); 
}


    pp = &p->next;
    ++i;
  }

  return prog ? 0 : CHAT_ERR_TIMEOUT;
}

int chat_server_feed(struct chat_server *s, const char *buf, uint32_t sz) {
  if (s->lsock < 0)
    return CHAT_ERR_NOT_STARTED;
  if (s->sin_len + sz > s->sin_cap) {
    size_t c = s->sin_cap ? s->sin_cap : 1024;
    while (c < s->sin_len + sz)
      c *= 2;
    s->sin = xrealloc(s->sin, c);
    s->sin_cap = c;
  }
  memcpy(s->sin + s->sin_len, buf, sz);
  s->sin_len += sz;
  size_t pos = 0;
  while (pos < s->sin_len) {
    char *nl = memchr(s->sin + pos, '\n', s->sin_len - pos);
    if (!nl)
      break;
    size_t l = nl - (s->sin + pos);
    if (l && s->sin[pos + l - 1] == '\r')
      --l;
    size_t clen;
    char *c = str_trim(s->sin + pos, l, &clen);
    if (clen)
      broadcast(s, NULL, c, clen);
    free(c);
    pos = (nl - s->sin) + 1;
  }
  if (pos) {
    memmove(s->sin, s->sin + pos, s->sin_len - pos);
    s->sin_len -= pos;
  }
  if (s->sin_len == 0 && s->sin_cap) {
    free(s->sin);
    s->sin = NULL;
    s->sin_cap = 0;
  }
  return 0;
}

struct chat_message *chat_server_pop_next(struct chat_server *s) {
  return q_pop(&s->hq, &s->tq);
}