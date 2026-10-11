#ifndef __CPIX_H__
#define __CPIX_H__

#include "common.h"
#include "media_set.h"


typedef struct cpix_data_s cpix_data_t;


vod_status_t cpix_parse(
	request_context_t* request_context,
	vod_str_t* source,
	cpix_data_t** res);

vod_status_t cpix_init_drm_info(
	request_context_t* request_context,
	media_set_t* media_set,
	cpix_data_t* cpix);

#endif // __CPIX_H__
