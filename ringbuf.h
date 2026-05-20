#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

struct ringbuf {
    char *buf;
    size_t size;
    size_t mask;
    uint64_t write_cnt;
    uint64_t read_cnt;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

struct ringbuf *ringbuf_create(size_t size);
void ringbuf_destroy(struct ringbuf *rb);
size_t ringbuf_write(struct ringbuf *rb, const void *data, size_t len);
size_t ringbuf_read(struct ringbuf *rb, void *data, size_t len);
size_t ringbuf_avail(struct ringbuf *rb);
size_t ringbuf_space(struct ringbuf *rb);

#endif
