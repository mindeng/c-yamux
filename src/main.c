
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#include "yamux.h"

int main(int argc, char* argv[]) {
    int sock;

    // init sock
    if ((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
    {
        printf("socket() failed with %i\n", -sock);

        goto END;
    }

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("192.168.0.212");
    addr.sin_port        = htons(1337);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0)
    {
        int e = errno;
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

    if (yamux_stream_init(strm))
    {
        printf("yamux_stream_init() failed\n");

        goto KILL_STRM;
    }

    char str[] = "hello\n";
    if (yamux_stream_write(strm, 6, str))
    {
        printf("yamux_stream_write() failed\n");

        goto KILL_STRM;
    }

    for (;;) {
        if (yamux_session_read(sess))
        {
            printf("yamux_session_read() failed\n");
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

