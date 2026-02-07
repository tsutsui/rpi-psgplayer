/*
 * p6psg.c
 *  PC-6001 PSGドライバ用演奏データ読み出し
 */

#include "p6psg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P6PSG_LAST_ERROR_MAXLEN 256

typedef struct p6psg {
    uint8_t *buf;
    size_t size;
    char last_error[P6PSG_LAST_ERROR_MAXLEN];
} p6psg_t;

/* オブジェクト生成 */
p6psg_t *
p6psg_create(void)
{
    p6psg_t *psg = malloc(sizeof(*psg));
    if (psg == NULL)
        return NULL;

    memset(psg, 0, sizeof(*psg));

    return psg;
}

/* オブジェクト破棄 */
void
p6psg_destroy(p6psg_t *psg)
{
    if (psg == NULL)
        return;

    if (psg->buf != NULL)
        free(psg->buf);

    free(psg);
}

/* 演奏データオブジェクト読み込みおよびパース */
int
p6psg_load(p6psg_t *psg, const char *path, p6psg_channel_dataset_t *channels)
{
    FILE *p6psgfile = NULL;
    uint8_t *buf = NULL;
    size_t p6size = 0;
    p6psg_channel_dataset_t channels_tmp;

    if (psg == NULL)
        return 0;

    if (path == NULL || channels == NULL) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "invalid argument");
        return 0;
    }
    psg->last_error[0] = '\0';

    if (psg->buf != NULL) {
        /* 再ロード許可仕様 */
        free(psg->buf);
        psg->buf = NULL;
    }
    psg->size = 0;

    /* open and read P6 PSG data file */
    p6psgfile = fopen(path, "rb");
    if (p6psgfile == NULL) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "fopen: %s", strerror(errno));
        goto fail;
    }

    if (fseek(p6psgfile, 0, SEEK_END) != 0) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "fseek(SEEK_END): %s", strerror(errno));
        goto fail;
    }
    long n = ftell(p6psgfile);
    if (n < 0) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "ftell: %s", strerror(errno));
        goto fail;
    }
    if (fseek(p6psgfile, 0, SEEK_SET) != 0) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "fseek(SEEK_SET): %s", strerror(errno));
        goto fail;
    }
    p6size = (size_t)n;

    if (p6size < (8 + 3)) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "too short");
        goto fail;
    }

    if (p6size >= 0x10000) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "too large");
        goto fail;
    }

    buf = malloc(p6size);
    if (buf == NULL) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "malloc: out of memory");
        goto fail;
    }

    if (fread(buf, 1, p6size, p6psgfile) != p6size) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "fread: %s", strerror(errno));
        goto fail;
    }

    (void)fclose(p6psgfile);
    p6psgfile = NULL;

    /* parse and split P6 PSG data file per channel */
    uint16_t a_addr = ((uint16_t)buf[1] << 8) | buf[0];
    uint16_t b_addr = ((uint16_t)buf[3] << 8) | buf[2];
    uint16_t c_addr = ((uint16_t)buf[5] << 8) | buf[4];
    if (c_addr > p6size || b_addr >= c_addr || a_addr >= b_addr || a_addr < 8) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "invalid address layout");
        goto fail;
    }
    uint16_t a_size = b_addr - a_addr;
    uint16_t b_size = c_addr - b_addr;
    uint16_t c_size = p6size - c_addr;
    if (buf[a_addr + a_size - 1] != 0xff ||
        buf[b_addr + b_size - 1] != 0xff ||
        buf[c_addr + c_size - 1] != 0xff) {
        snprintf(psg->last_error, P6PSG_LAST_ERROR_MAXLEN,
          "invalid data (no end mark)");
        goto fail;
    }
    channels_tmp.ch[P6PSG_CH_A].ptr = &buf[a_addr];
    channels_tmp.ch[P6PSG_CH_A].len = a_size;
    channels_tmp.ch[P6PSG_CH_B].ptr = &buf[b_addr];
    channels_tmp.ch[P6PSG_CH_B].len = b_size;
    channels_tmp.ch[P6PSG_CH_C].ptr = &buf[c_addr];
    channels_tmp.ch[P6PSG_CH_C].len = c_size;

    psg->buf = buf;
    psg->size = p6size;
    buf = NULL;
    *channels = channels_tmp;
    return 1;

 fail:
    if (buf != NULL) {
        free(buf);
    }
    psg->buf = NULL;
    psg->size = 0;
    if (p6psgfile != NULL)
        (void)fclose(p6psgfile);

    return 0;
}

/* エラーメッセージ */
const char *
p6psg_last_error(const p6psg_t *psg)
{
    if (psg == NULL)
        return "(psg is NULL)";

    return psg->last_error;
}
