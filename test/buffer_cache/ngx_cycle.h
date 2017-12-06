// Stub implementation of ngx_cycle for testing the buffer cache

#ifndef _NGX_CYCLE_H_INCLUDED_
#define _NGX_CYCLE_H_INCLUDED_

typedef struct ngx_shm_zone_s  ngx_shm_zone_t;

#include <ngx_core.h>

typedef ngx_int_t (*ngx_shm_zone_init_pt) (ngx_shm_zone_t *zone, void *data);

struct ngx_shm_zone_s {
    void                     *data;
    ngx_shm_t                 shm;
    ngx_shm_zone_init_pt      init;
    void                     *tag;
};

extern volatile ngx_cycle_t  *ngx_cycle;

struct ngx_cycle_s {
    ngx_pool_t               *pool;

    ngx_log_t                *log;
    ngx_log_t                 new_log;
};

ngx_shm_zone_t *ngx_shared_memory_add(ngx_conf_t *cf, ngx_str_t *name, size_t size, void *tag);

#endif // _NGX_CYCLE_H_INCLUDED_
