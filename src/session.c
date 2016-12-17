
#include <memory.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "session.h"
#include "stream.h"

static struct yamux_config dcfg = YAMUX_DEFAULT_CONFIG;

struct yamux_session* yamux_session_new(struct yamux_config* config, int sock, enum yamux_session_type type, void* userdata)
{
    if (!sock)
        return NULL;

    if (!config)
        config = &dcfg;

    size_t ab = config->accept_backlog;

    struct yamux_session_stream* streams =
        (struct yamux_session_stream*)malloc(sizeof(struct yamux_session_stream) * ab);

    for (size_t i = 0; i < ab; ++i)
        streams[i].alive = false;

    struct yamux_session s = (struct yamux_session){
        .config = config,
        .type   = type  ,
        .sock   = sock  ,

        .closed = false,

        .nextid = 1 + (type == yamux_session_server),

        .num_streams = 0,
        .cap_streams = 0,
        .streams     = streams,

        .since_ping = {.tv_sec = 0, .tv_nsec = 0 },

        .get_str_ud_fn = NULL,
        .ping_fn       = NULL,
        .pong_fn       = NULL,
        .go_away_fn    = NULL,
        .free_fn       = NULL,

        .userdata = userdata
    };

    struct yamux_session* sess = (struct yamux_session*)malloc(sizeof(struct yamux_session));

    *sess = s;

    return sess;
}
void yamux_session_free(struct yamux_session* session)
{
    if (!session)
        return;

    if (!session->closed)
        yamux_session_close(session, yamux_error_normal);

    if (session->free_fn)
        session->free_fn(session);

    for (size_t i = 0; i < session->cap_streams; ++i)
        if (session->streams[i].alive)
            yamux_stream_free(session->streams[i].stream);

    free(session->streams);
    free(session         );
}

ssize_t yamux_session_close(struct yamux_session* session, enum yamux_error err)
{
    if (!session)
        return -EINVAL;
    if (session->closed)
        return 0;

    struct yamux_frame f = (struct yamux_frame){
        .version  = YAMUX_VERSION,
        .type     = yamux_frame_go_away,
        .flags    = 0,
        .streamid = YAMUX_STREAMID_SESSION,
        .length   = (uint32_t)err
    };

    session->closed = true;

    return send(session->sock, &f, sizeof(struct yamux_frame), 0);
}

ssize_t yamux_session_ping(struct yamux_session* session, uint32_t value, bool pong)
{
    if (!session || session->closed)
        return -EINVAL;

    struct yamux_frame f = (struct yamux_frame){
        .version  = YAMUX_VERSION,
        .type     = yamux_frame_ping,
        .flags    = pong ? yamux_frame_ack : yamux_frame_syn,
        .streamid = YAMUX_STREAMID_SESSION,
        .length   = value
    };

    if (!timespec_get(&session->since_ping, TIME_UTC))
        return -EACCES;

    return send(session->sock, &f, sizeof(struct yamux_frame), 0);
}

ssize_t yamux_session_read(struct yamux_session* session)
{
    if (!session || session->closed)
        return -EINVAL;

    struct yamux_frame f;

    ssize_t r = recv(session->sock, &f, sizeof(struct yamux_frame), 0);
    if (r != sizeof(struct yamux_frame))
        return -1;

    decode_frame(&f);

    //printf("v%X got frame %X %X for stream %u with len %u\n", f.version, f.type, f.flags, f.streamid, f.length);

    if (f.version != YAMUX_VERSION)
        return -ENOTSUP; // can't send a Go Away message, either

    if (!f.streamid)
        switch (f.type)
        {
            case yamux_frame_ping:
                if (f.flags & yamux_frame_syn)
                {
                    yamux_session_ping(session, f.length, true);

                    if (session->ping_fn)
                        session->ping_fn(session, f.length);
                }
                else if ((f.flags & yamux_frame_ack) && session->pong_fn)
                {
                    struct timespec now, dt, last = session->since_ping;
                    if (!timespec_get(&now, TIME_UTC))
                        return -EACCES;

                    dt.tv_sec = now.tv_sec - last.tv_sec;
                    if (now.tv_nsec < last.tv_nsec)
                    {
                        dt.tv_sec--;
                        dt.tv_nsec = last.tv_nsec - now.tv_nsec;
                    }
                    else
                        dt.tv_nsec = now.tv_nsec - last.tv_nsec;

                    session->pong_fn(session, f.length, dt);
                }
                else
                    return -EPROTO;
                break;
            case yamux_frame_go_away:
                session->closed = true;
                if (session->go_away_fn)
                    session->go_away_fn(session, (enum yamux_error)f.length);
                break;
            default:
                return -EPROTO;
        }
    else
    {
        for (size_t i = 0; i < session->cap_streams; ++i)
        {
            struct yamux_session_stream* ss = &session->streams[i];
            struct yamux_stream* s = ss->stream;

            if (!ss->alive || s->state == yamux_stream_closed)
                continue;

            if (s->id == f.streamid)
            {
                if (f.flags & yamux_frame_rst)
                {
                    s->state = yamux_stream_closed;

                    if (s->rst_fn)
                        s->rst_fn(s);
                }
                else if (f.flags & yamux_frame_fin)
                {
                    // local stream didn't initiate FIN
                    if (s->state != yamux_stream_closing)
                        yamux_stream_close(s);

                    s->state = yamux_stream_closed;

                    if (s->fin_fn)
                        s->fin_fn(s);
                }
                else if (f.flags & yamux_frame_ack)
                {
                    if (s->state != yamux_stream_syn_sent)
                        return -EPROTO;

                    s->state = yamux_stream_est;
                }
                else if (f.flags)
                    return -EPROTO;

                ssize_t re = yamux_stream_process(s, &f, session->sock);
                return (re < 0) ? re : (re + r);
            }
        }

        // stream doesn't exist yet
        if (f.flags & yamux_frame_syn)
        {
            void* ud = NULL;

            if (session->get_str_ud_fn)
                ud = session->get_str_ud_fn(session, f.streamid);

            struct yamux_stream* st = yamux_stream_new(session, f.streamid, ud);

            if (session->new_stream_fn)
                session->new_stream_fn(session, st);

            st->state = yamux_stream_syn_recv;
        }
        else
            return -EPROTO;
    }

    return 0;
}

