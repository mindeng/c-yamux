
#ifndef YAMUX_CONFIG_H
#define YAMUX_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

struct yamux_config
{
    size_t   accept_backlog        ;
    /*bool     keep_alive            ;
    timespec keep_alive_time       ;
    timespec conn_write_timeout    ;*/
    uint32_t max_stream_window_size;
};

#define YAMUX_DEFAULT_WINDOW (0x100*0x400)

#define YAMUX_DEFAULT_CONFIG ((struct yamux_config)\
{\
    .accept_backlog=0x100,\
    /*.keep_alive=true,\
    .keep_alive_time=(struct timespec){.tv_sec=30,.tv_nsec=0},\
    .conn_write_timeout=(struct timespec){.tv_sec=10,.tv_nsec=0},*/\
    .max_stream_window_size=YAMUX_DEFAULT_WINDOW\
})\


#endif

