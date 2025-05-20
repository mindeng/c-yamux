
#include <errno.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "frame.h"
#include "stream.h"

#define MIN(x, y) (y ^ ((x ^ y) & -(x < y)))
#define MAX(x, y) (x ^ ((x ^ y) & -(x < y)))

struct yamux_stream *yamux_stream_new(struct yamux_session *session,
                                      yamux_streamid id, void *userdata) {
  if (!session)
    return NULL;

  if (!id) {
    id = session->nextid;
    session->nextid += 2;
  }

  struct yamux_stream *st = NULL;
  struct yamux_session_stream *ss;

  if (session->num_streams != session->cap_streams)
    for (size_t i = 0; i < session->cap_streams; ++i) {
      ss = &session->streams[i];

      if (!ss->alive) {
        st = ss->stream;
        ss->alive = true;
        goto FOUND;
      }
    }

  if (session->cap_streams == session->config->accept_backlog)
    return NULL;

  ss = &session->streams[session->cap_streams];

  if (ss->alive)
    return NULL;

  session->cap_streams++;

  ss->alive = true;
  st = ss->stream = malloc(sizeof(struct yamux_stream));

FOUND:;

  struct yamux_stream nst =
      (struct yamux_stream){.id = id,
                            .session = session,
                            .state = yamux_stream_inited,
                            .window_size = YAMUX_DEFAULT_WINDOW,

                            .read_fn = NULL,
                            .fin_fn = NULL,
                            .rst_fn = NULL,

                            .userdata = userdata};
  *st = nst;

  return st;
}

ssize_t yamux_stream_init(struct yamux_stream *stream) {
  if (!stream || stream->state != yamux_stream_inited ||
      stream->session->closed) {
    return -EINVAL;
  }

  struct yamux_frame f = (struct yamux_frame){.version = YAMUX_VERSION,
                                              .type = yamux_frame_window_update,
                                              .flags = yamux_frame_syn,
                                              .streamid = stream->id,
                                              .length = 0};

  stream->state = yamux_stream_syn_sent;

  encode_frame(&f);
  return send(stream->session->sock, &f, sizeof(struct yamux_frame), 0);
}
ssize_t yamux_stream_close(struct yamux_stream *stream) {
  if (!stream || stream->state != yamux_stream_est || stream->session->closed)
    return -EINVAL;

  struct yamux_frame f = (struct yamux_frame){.version = YAMUX_VERSION,
                                              .type = yamux_frame_window_update,
                                              .flags = yamux_frame_fin,
                                              .streamid = stream->id,
                                              .length = 0};

  stream->state = yamux_stream_closing;

  encode_frame(&f);
  return send(stream->session->sock, &f, sizeof(struct yamux_frame), 0);
}
ssize_t yamux_stream_reset(struct yamux_stream *stream) {
  if (!stream || stream->session->closed)
    return -EINVAL;

  struct yamux_frame f = (struct yamux_frame){.version = YAMUX_VERSION,
                                              .type = yamux_frame_window_update,
                                              .flags = yamux_frame_rst,
                                              .streamid = stream->id,
                                              .length = 0};

  stream->state = yamux_stream_closed;

  encode_frame(&f);
  return send(stream->session->sock, &f, sizeof(struct yamux_frame), 0);
}

static enum yamux_frame_flags get_flags(struct yamux_stream *stream) {
  switch (stream->state) {
  case yamux_stream_inited:
    stream->state = yamux_stream_syn_sent;
    return yamux_frame_syn;
  case yamux_stream_syn_recv:
    stream->state = yamux_stream_est;
    return yamux_frame_ack;
  default:
    return 0;
  }
}

ssize_t yamux_stream_window_update(struct yamux_stream *stream, int32_t delta) {
  if (!stream || stream->state == yamux_stream_closed ||
      stream->state == yamux_stream_closing || stream->session->closed)
    return -EINVAL;

  struct yamux_session *s = stream->session;

  int sock = s->sock;

  struct yamux_frame f = (struct yamux_frame){.version = YAMUX_VERSION,
                                              .type = yamux_frame_window_update,
                                              .flags = get_flags(stream),
                                              .streamid = stream->id,
                                              .length = (uint32_t)delta};
  encode_frame(&f);

  return send(sock, &f, sizeof(struct yamux_frame), 0);
}

ssize_t yamux_stream_write(struct yamux_stream *stream, uint32_t data_length,
                           void *data_) {
  if (!((size_t)stream | (size_t)data_) ||
      stream->state == yamux_stream_closed ||
      stream->state == yamux_stream_closing || stream->session->closed)
    return -EINVAL;

  char *data = (char *)data_;

  struct yamux_session *s = stream->session;

  int sock = s->sock;

  char *data_end = data + data_length;

  uint32_t ws = stream->window_size;
  if (ws <= 0) {
    return 0;
  }
  yamux_streamid id = stream->id;
  char sendd[ws + sizeof(struct yamux_frame)];

  while (data < data_end) {
    uint32_t dr = (uint32_t)(data_end - data), adv = MIN(dr, ws);

    struct yamux_frame f = (struct yamux_frame){.version = YAMUX_VERSION,
                                                .type = yamux_frame_data,
                                                .flags = get_flags(stream),
                                                .streamid = id,
                                                .length = adv};

    encode_frame(&f);
    memcpy(sendd, &f, sizeof(struct yamux_frame));
    memcpy(sendd + sizeof(struct yamux_frame), data, (size_t)adv);

    ssize_t res = send(sock, sendd, adv + sizeof(struct yamux_frame), 0);
    if (res > 0) {
      stream->window_size -= adv;
      return res;
    } else {
      return res;
    }

    data += adv;
  }

  return data_end - (char *)data_;
}

void yamux_stream_free(struct yamux_stream *stream) {
  if (!stream)
    return;

  if (stream->free_fn)
    stream->free_fn(stream);

  struct yamux_stream s = *stream;

  for (size_t i = 0; i < s.session->cap_streams; ++i) {
    struct yamux_session_stream *ss = &s.session->streams[i];
    if (ss->alive && ss->stream->id == s.id) {
      ss->alive = false;

      s.session->num_streams--;
      if (i == s.session->cap_streams - 1)
        s.session->cap_streams--;

      break;
    }
  }

  free(stream);
}

ssize_t yamux_stream_process(struct yamux_stream *stream,
                             struct yamux_frame *frame, int sock) {
  struct yamux_frame f = *frame;

  switch (f.type) {
  case yamux_frame_data: {
    char buf[f.length];

    ssize_t res = recv(sock, buf, (size_t)f.length, 0);

    if (res != (ssize_t)f.length)
      return -1;

    if (stream->read_fn)
      stream->read_fn(stream, f.length, buf);

    return res;
  }
  case yamux_frame_window_update: {
    uint64_t nws =
        (uint64_t)((int64_t)stream->window_size + (int64_t)(int32_t)f.length);
    nws &= 0xFFFFFFFFLL;
    stream->window_size = (uint32_t)nws;
    /* printf("new window_size: %lld\n", nws); */
    break;
  }
  default:
    return -EPROTO;
  }

  return 0;
}
