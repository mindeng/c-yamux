
#include <memory.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdlib.h>

#include "session.h"
#include "stream.h"

struct yamux_session* yamux_session_new(struct yamux_config* config, int sock, enum yamux_session_type type)
{
    if (!sock)
        return NULL;

    if (!config)
        *config = YAMUX_DEFAULT_CONFIG;

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

        .ping_fn    = NULL,
        .pong_fn    = NULL,
        .go_away_fn = NULL
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

    free(session->streams);
    free(session         );
}

ssize_t yamux_session_close(struct yamux_session* session, enum yamux_error err)
{
    if (!session)
        return EINVAL;
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
        return EINVAL;

    struct yamux_frame f = (struct yamux_frame){
        .version  = YAMUX_VERSION,
        .type     = yamux_frame_ping,
        .flags    = pong ? yamux_frame_ack : yamux_frame_syn,
        .streamid = YAMUX_STREAMID_SESSION,
        .length   = value
    };

    if (!timespec_get(&session->since_ping, TIME_UTC))
        return EACCES;

    return send(session->sock, &f, sizeof(struct yamux_frame), 0);
}

struct yamux_stream* yamux_new_stream(struct yamux_session* session, yamux_streamid id)
{
    if (!session)
        return NULL;

    if (!id)
    {
        id = session->nextid;
        session->nextid += 2;
    }

    struct yamux_stream* st = NULL;
    struct yamux_session_stream* ss;

    if (session->num_streams != session->cap_streams)
        for (size_t i = 0; i < session->cap_streams; ++i)
        {
            ss = &session->streams[i];

            if (!ss->alive)
            {
                st = ss->stream;
                ss->alive = true;
                goto FOUND;
            }
        }

    if (session->cap_streams == session->config->accept_backlog)
        return NULL;

    ss = &session->streams[session->cap_streams];
    session->cap_streams++;

    ss->alive = true;
    st = ss->stream;

FOUND:;

    struct yamux_stream nst = (struct yamux_stream){
        .id          = id,
        .session     = session,
        .state       = yamux_stream_inited,
        .window_size = YAMUX_DEFAULT_WINDOW,

        .read_fn = NULL,
        .fin_fn  = NULL,
        .rst_fn  = NULL
    };
    *st = nst;

    return st;
}

ssize_t yamux_session_read(struct yamux_session* session)
{
    if (!session || session->closed)
        return EINVAL;

    struct yamux_frame f;

    ssize_t r = recv(session->sock, &f, sizeof(struct yamux_frame), 0);
    decode_frame(&f);
    if (r)
        return r;

    if (f.version != YAMUX_VERSION)
        return ENOTSUP; // can't send a Go Away message, either

    if (!f.streamid)
        switch (f.type)
        {
            case yamux_frame_ping:
                if (f.flags & yamux_frame_syn)
                {
                    yamux_session_ping(session, f.length, true);

                    if (session->ping_fn)
                        session->ping_fn(session);
                }
                else if ((f.flags & yamux_frame_ack) && session->pong_fn)
                {
                    struct timespec now, dt, last = session->since_ping;
                    if (!timespec_get(&now, TIME_UTC))
                        return EACCES;

                    dt.tv_sec = now.tv_sec - last.tv_sec;
                    if (now.tv_nsec < last.tv_nsec)
                    {
                        dt.tv_sec--;
                        dt.tv_nsec = last.tv_nsec - now.tv_nsec;
                    }
                    else
                        dt.tv_nsec = now.tv_nsec - last.tv_nsec;

                    session->pong_fn(session, dt);
                }
                else
                    return EPROTO;
                break;
            case yamux_frame_go_away:
                session->closed = true;
                if (session->go_away_fn)
                    session->go_away_fn(session, (enum yamux_error)f.length);
                break;
            default:
                return EPROTO;
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

                    return 0;
                }
                else if (f.flags & yamux_frame_fin)
                {
                    // local stream didn't initiate FIN
                    if (s->state != yamux_stream_closing)
                        yamux_stream_close(s);

                    s->state = yamux_stream_closed;

                    if (s->fin_fn)
                        s->fin_fn(s);

                    return 0;
                }
                else if (f.flags & yamux_frame_ack)
                {
                    if (s->state != yamux_stream_syn_sent)
                        return EPROTO;

                    s->state = yamux_stream_est;

                    return 0;
                }
                else if (f.flags)
                    return EPROTO;

                return yamux_stream_process(s, &f, session->sock);
            }
        }

        // stream doesn't exist yet
        if (f.flags & yamux_frame_syn)
        {
            struct yamux_stream* st = yamux_new_stream(session, f.streamid);

            if (session->new_stream_fn)
                session->new_stream_fn(session, st);

            st->state = yamux_stream_syn_recv;
        }
        else
            return EPROTO;
    }

    return 0;
}

