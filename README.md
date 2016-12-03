# c-yamux

A C port of yamux. Unlike the Go library, it doesn't start a new thread
(or something equivalent to a goroutine) to check for incoming messages
, nor does it automatically ping every so often.

## Usage

For a complete example (with proper error handling), see `src/main.c`.

```c
struct yamux_session* se = yamux_session_new(NULL, sock,
        yamux_session_client);

struct yamux_stream* st = yamux_stream_new(se, 0);

yamux_stream_init(st); // sends the handshake

char str[] = "hello\n";
yamux_stream_write(st, strlen(str), str);

// read incoming messages
for (;;)
{
    yamux_session_read(se);
}
```

## TODO

* Add LGPL file headers

