
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#include "yamux.h"

#define SERVER

static void on_read(struct yamux_stream* stream, uint32_t data_len, void* data)
{
    char d[data_len + 1];
    d[data_len] = 0;
    memcpy(d, data, data_len);

    printf("%s", d);
}
static void on_new(struct yamux_session* session, struct yamux_stream* stream)
{
    stream->read_fn = on_read;
}

static struct sockaddr_in addr;

static ssize_t init(int sock)
{
    int err;
#ifndef SERVER
    //printf("connect\n");
    if ((err = connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in))) < 0)
        return err;

    return sock;
#else
    //printf("bind\n");
    if ((err = bind(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in))) < 0)
        return err;

    //printf("listen\n");
    if ((err = listen(sock, 0x80)) < 0)
        return err;

    //printf("accept\n");
    return accept(sock, NULL, NULL);
#endif
}

int main(int argc, char* argv[])
{
    int sock;
    int e;
    ssize_t ee;

    // init sock
    if ((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
    {
        e = errno;
        printf("socket() failed with %i\n", e);

        goto END;
    }

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = htons(1337);

    int s2 = -1;
    ssize_t initr = init(sock);
    if (initr < 0)
    {
        e = errno;
        printf("init failed with %i, errno=%i\n", (int)-initr, e);

        goto FREE_SOCK;
    }
    s2 = (int)initr;

    // init yamux
    struct yamux_session* sess = yamux_session_new(NULL, s2,
#ifndef SERVER
            yamux_session_client
#else
            yamux_session_server
#endif
            );
    if (!sess)
    {
        printf("yamux_session_new() failed\n");

        goto FREE_SOCK;
    }
    sess->new_stream_fn = on_new;

#ifndef SERVER
    struct yamux_stream* strm = yamux_stream_new(sess, 0);
    if (!strm)
    {
        printf("yamux_new_stream() failed\n");

        goto FREE_YAMUX;
    }

    strm->read_fn = on_read;

    if ((ee = yamux_stream_init(strm)) < 0)
    {
        e = errno;
        printf("yamux_stream_init() failed with %i, errno=%i\n", (int)-ee, e);

        goto KILL_STRM;
    }

    char str[] = "hello\n";
    if ((ee = yamux_stream_write(strm, 6, str)) < 0)
    {
        e = errno;
        printf("yamux_stream_write() failed with %i, errno=%i\n", (int)-ee, e);

        goto KILL_STRM;
    }
#endif

    for (;;) {
        if ((ee = yamux_session_read(sess)) < 0)
        {
            e = errno;
            printf("yamux_session_read() failed with %i, errno=%i\n", (int)-ee, e);

            goto KILL_STRM;
        }

#ifndef SERVER
        break;
#endif
    }

KILL_STRM:
#ifndef SERVER
    if (yamux_stream_reset(strm))
        goto FREE_STRM;
FREE_STRM:
    yamux_stream_free(strm);

FREE_YAMUX:
#endif
    yamux_session_free(sess);
FREE_SOCK:
    close(sock);
END:
    return 0;
}

