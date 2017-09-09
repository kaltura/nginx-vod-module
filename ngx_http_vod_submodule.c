#include "ngx_http_vod_dash.h"
#include "ngx_http_vod_hds.h"
#include "ngx_http_vod_hls.h"
#include "ngx_http_vod_mss.h"
#include "ngx_http_vod_thumb.h"
#include "ngx_http_vod_volume_map.h"

const ngx_http_vod_submodule_t* submodules[] = {
	&dash,
	&hds,
	&hls,
	&mss,
#if (NGX_HAVE_LIB_AV_CODEC)
	&thumb,
	&volume_map,
#endif // (NGX_HAVE_LIB_AV_CODEC)
	NULL,
};
