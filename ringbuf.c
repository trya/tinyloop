#include "ringbuf.h"
#include <stdlib.h>
#include <string.h>

struct ringbuf *ringbuf_create(size_t size)
{
    size_t pow2 = 1;
    while (pow2 < size)
        pow2 <<= 1;

    struct ringbuf *rb = calloc(1, sizeof(*rb));
    if (!rb)
        return NULL;

    rb->buf = malloc(pow2);
    if (!rb->buf) {
        free(rb);
        return NULL;
    }

    rb->size = pow2;
    rb->mask = pow2 - 1;
    rb->write_cnt = 0;
    rb->read_cnt = 0;
    pthread_mutex_init(&rb->lock, NULL);
    pthread_cond_init(&rb->cond, NULL);
    return rb;
}

void ringbuf_destroy(struct ringbuf *rb)
{
    if (!rb)
        return;
    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->cond);
    free(rb->buf);
    free(rb);
}

size_t ringbuf_write(struct ringbuf *rb, const void *data, size_t len)
{
    pthread_mutex_lock(&rb->lock);

    size_t avail = (size_t)(rb->write_cnt - rb->read_cnt);
    size_t space = rb->size - avail;
    if (space == 0) {
        pthread_mutex_unlock(&rb->lock);
        return 0;
    }
    if (len > space)
        len = space;

    size_t off = rb->write_cnt & rb->mask;
    size_t first = len;
    if (first > rb->size - off)
        first = rb->size - off;
    memcpy(rb->buf + off, data, first);
    if (first < len)
        memcpy(rb->buf, (const char *)data + first, len - first);
    rb->write_cnt += len;

    pthread_cond_signal(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
    return len;
}

size_t ringbuf_read(struct ringbuf *rb, void *data, size_t len)
{
    pthread_mutex_lock(&rb->lock);

    size_t avail = (size_t)(rb->write_cnt - rb->read_cnt);
    if (avail == 0) {
        pthread_mutex_unlock(&rb->lock);
        return 0;
    }
    if (len > avail)
        len = avail;

    size_t off = rb->read_cnt & rb->mask;
    size_t first = len;
    if (first > rb->size - off)
        first = rb->size - off;
    memcpy(data, rb->buf + off, first);
    if (first < len)
        memcpy((char *)data + first, rb->buf, len - first);
    rb->read_cnt += len;

    pthread_cond_signal(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
    return len;
}

size_t ringbuf_avail(struct ringbuf *rb)
{
    pthread_mutex_lock(&rb->lock);
    size_t avail = (size_t)(rb->write_cnt - rb->read_cnt);
    pthread_mutex_unlock(&rb->lock);
    return avail;
}

size_t ringbuf_space(struct ringbuf *rb)
{
    pthread_mutex_lock(&rb->lock);
    size_t avail = (size_t)(rb->write_cnt - rb->read_cnt);
    pthread_mutex_unlock(&rb->lock);
    return rb->size - avail;
}
