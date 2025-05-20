#include <errno.h>
#include <memory.h>
#include <pthread.h> // 引入 pthread 库
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

  // 初始化互斥锁和条件变量
  if (pthread_mutex_init(&st->mutex, NULL) != 0) {
    fprintf(stderr, "Error initializing mutex\n");
    free(st);
    ss->alive = false;      // Revert alive state if mutex init fails
    session->cap_streams--; // Revert cap_streams if mutex init fails
    return NULL;
  }
  if (pthread_cond_init(&st->cond, NULL) != 0) {
    fprintf(stderr, "Error initializing condition variable\n");
    pthread_mutex_destroy(&st->mutex); // Clean up mutex if cond init fails
    free(st);
    ss->alive = false;      // Revert alive state if cond init fails
    session->cap_streams--; // Revert cap_streams if cond init fails
    return NULL;
  }

  return st;
}

ssize_t yamux_stream_init(struct yamux_stream *stream) {
  if (!stream || stream->session->closed) {
    return -EINVAL;
  }

  pthread_mutex_lock(&stream->mutex);
  if (stream->state != yamux_stream_inited) {
    pthread_mutex_unlock(&stream->mutex);
    return -EINVAL;
  }

  struct yamux_frame f = (struct yamux_frame){.version = YAMUX_VERSION,
                                              .type = yamux_frame_window_update,
                                              .flags = yamux_frame_syn,
                                              .streamid = stream->id,
                                              .length = 0};

  stream->state = yamux_stream_syn_sent;
  pthread_mutex_unlock(&stream->mutex);

  encode_frame(&f);
  return send(stream->session->sock, &f, sizeof(struct yamux_frame), 0);
}

ssize_t yamux_stream_close(struct yamux_stream *stream) {
  if (!stream || stream->session->closed)
    return -EINVAL;

  pthread_mutex_lock(&stream->mutex);
  if (stream->state != yamux_stream_est) {
    pthread_mutex_unlock(&stream->mutex);
    return -EINVAL;
  }

  struct yamux_frame f = (struct yamux_frame){.version = YAMUX_VERSION,
                                              .type = yamux_frame_window_update,
                                              .flags = yamux_frame_fin,
                                              .streamid = stream->id,
                                              .length = 0};

  stream->state = yamux_stream_closing;
  pthread_mutex_unlock(&stream->mutex);

  encode_frame(&f);
  return send(stream->session->sock, &f, sizeof(struct yamux_frame), 0);
}

ssize_t yamux_stream_reset(struct yamux_stream *stream) {
  if (!stream || stream->session->closed)
    return -EINVAL;

  pthread_mutex_lock(&stream->mutex);
  struct yamux_frame f = (struct yamux_frame){.version = YAMUX_VERSION,
                                              .type = yamux_frame_window_update,
                                              .flags = yamux_frame_rst,
                                              .streamid = stream->id,
                                              .length = 0};

  stream->state = yamux_stream_closed;
  pthread_mutex_unlock(&stream->mutex);

  encode_frame(&f);
  return send(stream->session->sock, &f, sizeof(struct yamux_frame), 0);
}

static enum yamux_frame_flags get_flags(struct yamux_stream *stream) {
  // 此函数在调用方加锁，因此不需要内部加锁
  enum yamux_frame_flags flags = 0;
  switch (stream->state) {
  case yamux_stream_inited:
    stream->state = yamux_stream_syn_sent;
    flags = yamux_frame_syn;
    break;
  case yamux_stream_syn_recv:
    stream->state = yamux_stream_est;
    flags = yamux_frame_ack;
    break;
  default:
    flags = 0;
    break;
  }
  return flags;
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
  ssize_t total_sent_data = 0; // 记录实际发送的数据长度

  while (data < data_end) {
    pthread_mutex_lock(&stream->mutex);
    uint32_t current_window_size = stream->window_size;
    pthread_mutex_unlock(&stream->mutex);

    if (current_window_size <= 0) {
      // 窗口大小不足，返回已发送的数据量，调用方应等待
      return total_sent_data;
    }

    uint32_t dr = (uint32_t)(data_end - data);
    uint32_t adv = MIN(dr, current_window_size);

    pthread_mutex_lock(
        &stream->mutex); // 保护 get_flags 对 stream->state 的修改
    struct yamux_frame f = (struct yamux_frame){.version = YAMUX_VERSION,
                                                .type = yamux_frame_data,
                                                .flags = get_flags(stream),
                                                .streamid = stream->id,
                                                .length = adv};
    pthread_mutex_unlock(&stream->mutex);

    char sendd[adv + sizeof(struct yamux_frame)]; // VLA used here

    encode_frame(&f);
    memcpy(sendd, &f, sizeof(struct yamux_frame));
    memcpy(sendd + sizeof(struct yamux_frame), data, (size_t)adv);

    ssize_t res = send(sock, sendd, adv + sizeof(struct yamux_frame), 0);
    if (res > 0) {
      // 只有在成功发送数据后才更新窗口大小
      pthread_mutex_lock(&stream->mutex);
      // res 是发送的总字节数 (帧头 + 数据)，我们只从 window_size 中减去数据部分
      stream->window_size -= adv;
      pthread_mutex_unlock(&stream->mutex);

      total_sent_data += adv;
      data += adv;
    } else {
      // 发送错误或部分发送，返回已发送的数据量或错误
      return total_sent_data > 0 ? total_sent_data : res;
    }
  }

  return total_sent_data;
}

void yamux_stream_free(struct yamux_stream *stream) {
  if (!stream)
    return;

  if (stream->free_fn)
    stream->free_fn(stream);

  // 销毁互斥锁和条件变量
  pthread_mutex_destroy(&stream->mutex);
  pthread_cond_destroy(&stream->cond);

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
    char buf[f.length]; // VLA used here

    ssize_t res = recv(sock, buf, (size_t)f.length, 0);

    if (res != (ssize_t)f.length)
      return -1; // Error or partial read

    // read_fn 不修改 stream 状态，无需加锁
    if (stream->read_fn)
      stream->read_fn(stream, f.length, buf);

    return res;
  }
  case yamux_frame_window_update: {
    pthread_mutex_lock(&stream->mutex);
    uint32_t old_window_size = stream->window_size; // 记录旧的窗口大小
    uint64_t nws =
        (uint64_t)((int64_t)stream->window_size + (int64_t)(int32_t)f.length);
    nws &= 0xFFFFFFFFLL;
    stream->window_size = (uint32_t)nws;
    /* printf("new window_size: %lld\n", nws); */

    // 如果窗口从0变为非0，则发出信号
    if (old_window_size == 0 && stream->window_size > 0) {
      pthread_cond_signal(&stream->cond);
    }
    pthread_mutex_unlock(&stream->mutex);
    break;
  }
  default:
    return -EPROTO;
  }

  return 0;
}

// 当 stream->window_size 为 0 时，等待其增长
ssize_t yamux_stream_wait_for_window(struct yamux_stream *stream) {
  if (!stream) {
    return -EINVAL;
  }

  pthread_mutex_lock(&stream->mutex);
  // 使用 while 循环处理虚假唤醒 (spurious wakeups)
  while (stream->window_size <= 0) {
    // 如果流已经关闭，则不再等待
    if (stream->state == yamux_stream_closed ||
        stream->state == yamux_stream_closing) {
      pthread_mutex_unlock(&stream->mutex);
      return -EPIPE; // Broken pipe or stream closed
    }
    pthread_cond_wait(&stream->cond, &stream->mutex);
  }
  pthread_mutex_unlock(&stream->mutex);

  return 0; // 窗口大小已大于 0
}
