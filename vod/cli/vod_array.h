#ifndef __VOD_ARRAY_H__
#define __VOD_ARRAY_H__

#define vod_array_init(array, pool, n, size) vod_array_init_impl(array, n, size) 

typedef struct {
    void        *elts;
    unsigned   nelts;
    size_t       size;
    unsigned   nalloc;
} vod_array_t;

int vod_array_init_impl(vod_array_t *array, unsigned n, size_t size);
void *vod_array_push(vod_array_t *a);
void vod_array_destroy(vod_array_t *a);

#endif // __VOD_ARRAY_H__
