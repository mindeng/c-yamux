
#ifndef YAMUX_STREAM_H
#define YAMUX_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "session.h"

// NOTE: 'data' is not guaranteed to be preserved when the read_fn
// handler exists (read: it will be freed).
struct yamux_stream;

typedef void (*yamux_stream_read_fn)(struct yamux_stream* stream, uint32_t data_length, void* data);
typedef void (*yamux_stream_fin_fn )(struct yamux_stream* stream);
typedef void (*yamux_stream_rst_fn )(struct yamux_stream* stream);
typedef void (*yamux_stream_free_fn)(struct yamux_stream* stream);

enum yamux_stream_state
{
    yamux_stream_inited,
    yamux_stream_syn_sent,
    yamux_stream_syn_recv,
    yamux_stream_est,
    yamux_stream_closing,
    yamux_stream_closed
};

struct yamux_stream
{
    struct yamux_session* session;

    yamux_stream_read_fn read_fn;
    yamux_stream_fin_fn  fin_fn ;
    yamux_stream_rst_fn  rst_fn ;
    yamux_stream_free_fn free_fn;

    void* userdata;

    enum yamux_stream_state state;

    yamux_streamid id;

    uint32_t window_size;
};

// does not init the stream
struct yamux_stream* yamux_stream_new(struct yamux_session* session, yamux_streamid id, void* userdata);

// not obligatory, SYN is sent by yamux_stream_write when the stream
// isn't initialised anyway
ssize_t yamux_stream_init (struct yamux_stream* stream);

// doesn't free the stream
// uses FIN
ssize_t yamux_stream_close(struct yamux_stream* stream);
// uses RST
ssize_t yamux_stream_reset(struct yamux_stream* stream);

void yamux_stream_free(struct yamux_stream* stream);

ssize_t yamux_stream_window_update(struct yamux_stream* stream, int32_t delta);
ssize_t yamux_stream_write(struct yamux_stream* stream, uint32_t data_length, void* data);

ssize_t yamux_stream_process(struct yamux_stream* stream, struct yamux_frame* frame, int sock);

#endif

