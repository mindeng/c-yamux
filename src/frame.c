
#include <stdbool.h>

#include "frame.h"

enum eness
{
    unk,
    little,
    big
};

static enum eness eness = unk;

static void set_eness()
{
    uint16_t x = 1;

    if (*(char*)&x == 1)
        eness = little;
    else
        eness = big;
}

static void do16(uint16_t* pv)
{
    if (eness == big)
        return;

    uint16_t v = *pv;
    *pv = (uint16_t)((
          ((v & 0xFF00) >> 8)
        | ((v & 0x00FF) << 8)
        ) & 0xFFFF);
}
static void do32(uint32_t* pv)
{
    if (eness == big)
        return;

    uint32_t v = *pv;
    *pv =
          ((v & 0xFF000000) >> 24)
        | ((v & 0x00FF0000) >>  8)
        | ((v & 0x0000FF00) <<  8)
        | ((v & 0x000000FF) << 24)
        ;
}

void encode_frame(struct yamux_frame* frame)
{
    if (eness == unk)
        set_eness();

    do16(&frame->flags   );
    do32(&frame->streamid);
    do32(&frame->length  );
}
void decode_frame(struct yamux_frame* frame)
{
    if (eness == unk)
        set_eness();

    do16(&frame->flags   );
    do32(&frame->streamid);
    do32(&frame->length  );
}

