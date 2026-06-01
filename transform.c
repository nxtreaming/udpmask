#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "transform.h"
#include "udpmask.h"

void check_gen_mask(struct um_transform *ctx)
{
    if (ctx->mask_ct++ < MASK_MAXCT) {
        return;
    }

    log_debug("mask used more than %u times ago, update", MASK_MAXCT);

    genmask(ctx->mask, MASK_LEN);
    ctx->mask_ct = 0;
}

size_t maskbuf(struct um_transform *ctx, unsigned char *buf, size_t buflen) {
    if (buflen > UM_BUFFER - MASK_LEN) {
        return 0;
    }

    check_gen_mask(ctx);

    transformbuf(buf, buflen, ctx->mask);
    memcpy(buf + buflen, ctx->mask, MASK_LEN);

    return buflen + MASK_LEN;
}

size_t unmaskbuf(struct um_transform *ctx, unsigned char *buf, size_t buflen) {
    unsigned char rcv_mask[MASK_LEN];
    size_t len;

    if (buflen < MASK_LEN) {
        return 0;
    }

    len = buflen - MASK_LEN;

    memcpy(rcv_mask, buf + len, MASK_LEN);
    transformbuf(buf, len, rcv_mask);

    return len;
}

size_t masknoop(struct um_transform * ctx, unsigned char *buf, size_t buflen) {
    return buflen;
}
