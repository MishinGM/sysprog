#pragma once
#include <stddef.h>
#define NEED_BROADCAST 1
#define NEED_BATCH 1

enum coro_bus_error_code {
	CORO_BUS_ERR_NONE = 0,
	CORO_BUS_ERR_NO_CHANNEL,
	CORO_BUS_ERR_WOULD_BLOCK,
	CORO_BUS_ERR_NOT_IMPLEMENTED,
};

struct coro_bus;
enum coro_bus_error_code coro_bus_errno(void);
void coro_bus_errno_set(enum coro_bus_error_code err);
struct coro_bus *coro_bus_new(void);
void coro_bus_delete(struct coro_bus *bus);
int coro_bus_channel_open(struct coro_bus *bus, size_t size_limit);
void coro_bus_channel_close(struct coro_bus *bus, int channel);
int coro_bus_send(struct coro_bus *bus, int channel, unsigned data);
int coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data);
int coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data);
int coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data);

#if NEED_BROADCAST
int coro_bus_broadcast(struct coro_bus *bus, unsigned data);
int coro_bus_try_broadcast(struct coro_bus *bus, unsigned data);
#endif

#if NEED_BATCH
int coro_bus_send_v(struct coro_bus *bus, int channel,
	const unsigned *data, unsigned count);
int coro_bus_try_send_v(struct coro_bus *bus, int channel,
	const unsigned *data, unsigned count);
int coro_bus_recv_v(struct coro_bus *bus, int channel,
	unsigned *data, unsigned capacity);
int coro_bus_try_recv_v(struct coro_bus *bus, int channel,
	unsigned *data, unsigned capacity);
#endif
