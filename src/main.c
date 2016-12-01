
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#include "yamux.h"

static void on_read(struct yamux_stream* stream, uint32_t data_len, void* data)
{
    char d[data_len + 1];
    d[data_len] = 0;
    memcpy(d, data, data_len);

    printf("%s", d);
};

int main(int argc, char* argv[]) {
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

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = htons(1337);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0)
    {
        e = errno;
        printf("connect() failed with %i\n", e);

        goto FREE_SOCK;
    }

    // init yamux

    struct yamux_session* sess = yamux_session_new(NULL, sock, yamux_session_client);
    if (!sess)
    {
        printf("yamux_session_new() failed\n");

        goto FREE_SOCK;
    }

    // TODO: make name consistent
    struct yamux_stream* strm = yamux_new_stream(sess, 0);
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

    for (;;) {
        if ((ee = yamux_session_read(sess)) < 0)
        {
            e = errno;
            printf("yamux_session_read() failed with %i, errno=%i\n", (int)-ee, e);
            goto KILL_STRM;
        }

        // TODO: do something
    }

KILL_STRM:
    if (yamux_stream_reset(strm))
        goto FREE_STRM;
FREE_STRM:
    yamux_stream_free(strm);

FREE_YAMUX:
    yamux_session_free(sess);
FREE_SOCK:
    close(sock);
END:
    return 0;
}

