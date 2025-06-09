#include "chat.h"
#include <poll.h>
#include <stdlib.h>

void chat_message_delete(struct chat_message *msg) {
  if (!msg)
    return;
#if NEED_AUTHOR
  free((void *)msg->author);
#endif
  free(msg->data);
  free(msg);
}

int chat_events_to_poll_events(int mask) {
  int res = 0;
  if (mask & CHAT_EVENT_INPUT)
    res |= POLLIN;
  if (mask & CHAT_EVENT_OUTPUT)
    res |= POLLOUT;
  return res;
}
