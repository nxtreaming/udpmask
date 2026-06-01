#ifndef _incl_TRANSFORM_H
#define _incl_TRANSFORM_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MASK_UNIT       uint32_t
#define MASK_LEN        ((int) sizeof(MASK_UNIT))
#define MASK_MAXCT      ((unsigned int) 100000)

struct um_transform {
    unsigned char   mask[MASK_LEN];
    unsigned int    mask_ct;
};

static inline void transformbuf(unsigned char *buf, size_t buflen,
                                const unsigned char *mask)
{
    MASK_UNIT mask_word;
    size_t i = 0;

    memcpy(&mask_word, mask, sizeof(mask_word));

    for (; buflen - i >= sizeof(mask_word); i += sizeof(mask_word)) {
        MASK_UNIT word;
        memcpy(&word, buf + i, sizeof(word));
        word ^= mask_word;
        memcpy(buf + i, &word, sizeof(word));
    }

    for (; i < buflen; i++) {
        buf[i] ^= mask[i % MASK_LEN];
    }
}


#define genmask(mask, n)                                        \
    do {                                                        \
        for (size_t i = 0; i < n; i++) {                        \
            for (int j = 0; j < 10; j++) {                      \
                mask[i] = (unsigned char) (rand() % 256);       \
                if (mask[i] != 0) break;                        \
            }                                                   \
        }                                                       \
    } while (0)                                                 \

void check_gen_mask(struct um_transform *);
size_t maskbuf(struct um_transform *, unsigned char *, size_t);
size_t unmaskbuf(struct um_transform *, unsigned char *, size_t);
size_t masknoop(struct um_transform *, unsigned char *, size_t);

typedef size_t (*buf_func)(struct um_transform *, unsigned char *, size_t);

#endif /* _incl_TRANSFORM_H */
