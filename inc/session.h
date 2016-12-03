
#ifndef YAMUX_SESSION_H
#define YAMUX_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "config.h"
#include "frame.h"
#include "stream.h"

enum yamux_session_type
{
    yamux_session_client,
    yamux_session_server
};
enum yamux_error
{
    yamux_error_normal = 0x00,
    yamux_error_protoc = 0x01,
    yamux_error_intern = 0x02
};

struct yamux_session;
struct yamux_stream;

typedef void (*yamux_session_ping_fn      )(struct yamux_session* session                             );
typedef void (*yamux_session_pong_fn      )(struct yamux_session* session, struct timespec dt         );
typedef void (*yamux_session_go_away_fn   )(struct yamux_session* session, enum yamux_error err       );
typedef void (*yamux_session_new_stream_fn)(struct yamux_session* session, struct yamux_stream* stream);

struct yamux_session_stream
{
    struct yamux_stream* stream;
    bool alive;
};
struct yamux_session
{
    struct yamux_config* config;

    size_t num_streams;
    size_t cap_streams;
    struct yamux_session_stream* streams;

    yamux_session_ping_fn       ping_fn      ;
    yamux_session_pong_fn       pong_fn      ;
    yamux_session_go_away_fn    go_away_fn   ;
    yamux_session_new_stream_fn new_stream_fn;

    struct timespec since_ping;

    enum yamux_session_type type;

    int sock;

    yamux_streamid nextid;

    bool closed;
};

struct yamux_session* yamux_session_new (struct yamux_config* config, int sock, enum yamux_session_type type);
// does not close the socket, but does close the session
void                  yamux_session_free(struct yamux_session* session);

// does not free used memory
ssize_t yamux_session_close(struct yamux_session* session, enum yamux_error err);
inline ssize_t yamux_session_go_away(struct yamux_session* session, enum yamux_error err)
{
    return yamux_session_close(session, err);
}

ssize_t yamux_session_ping(struct yamux_session* session, uint32_t value, bool pong);

// defers to stream read handlers
ssize_t yamux_session_read(struct yamux_session* session);

#endif

