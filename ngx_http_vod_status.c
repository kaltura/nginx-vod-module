// includes
#include "ngx_http_vod_status.h"
#include "ngx_http_vod_module.h"
#include "ngx_http_vod_utils.h"
#include "ngx_http_vod_conf.h"
#include "ngx_buffer_cache.h"

// macros
#define DEFINE_STAT(x) { #x, sizeof(#x) - 1, offsetof(ngx_buffer_cache_stats_t, x) }

// typedefs
typedef struct {
	const char* name;
	int name_len;
	int offset;
} ngx_http_vod_stat_def_t;

// constants
static const u_char status_prefix[] = 
	"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\r\n"
	"<vod>\r\n"
	"<version>" NGINX_VOD_VERSION "</version>\r\n"
	"<built>" __DATE__ " " __TIME__ "</built>\r\n";
static const u_char status_postfix[] = "</vod>\r\n";

static u_char xml_content_type[] = "text/xml";

static ngx_http_vod_stat_def_t buffer_cache_stat_defs[] = {
	DEFINE_STAT(store_ok),
	DEFINE_STAT(store_bytes),
	DEFINE_STAT(store_err),
	DEFINE_STAT(fetch_hit),
	DEFINE_STAT(fetch_bytes),
	DEFINE_STAT(fetch_miss),
	DEFINE_STAT(evicted),
	DEFINE_STAT(evicted_bytes),
	DEFINE_STAT(reset),
	DEFINE_STAT(entries),
	DEFINE_STAT(data_size),
	{ NULL, 0, 0 }
};

static u_char*
ngx_http_vod_append_cache_stats(u_char* p, ngx_buffer_cache_stats_t* stats)
{
	ngx_http_vod_stat_def_t* cur_stat;

	for (cur_stat = buffer_cache_stat_defs; cur_stat->name != NULL; cur_stat++)
	{
		// opening tag
		*p++ = '<';
		p = ngx_copy(p, cur_stat->name, cur_stat->name_len);
		*p++ = '>';

		// value
		p = ngx_sprintf(p, "%A", *(ngx_atomic_t*)((u_char*)stats + cur_stat->offset));

		// closing tag
		*p++ = '<';
		*p++ = '/';
		p = ngx_copy(p, cur_stat->name, cur_stat->name_len);
		*p++ = '>';

		// newline
		*p++ = CR;
		*p++ = LF;
	}

	return p;
}

ngx_int_t
ngx_http_vod_status_handler(ngx_http_request_t *r)
{
	ngx_buffer_cache_stats_t stats;
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_stat_def_t* cur_stat;
	ngx_str_t response;
	u_char* p;
	size_t cache_stats_len = 0;
	size_t length;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

	// calculate the buffer size
	for (cur_stat = buffer_cache_stat_defs; cur_stat->name != NULL; cur_stat++)
	{
		cache_stats_len += sizeof("<></>\r\n") - 1 + 2 * cur_stat->name_len + NGX_ATOMIC_T_LEN;
	}

	length = sizeof(status_prefix) - 1;
	if (conf->moov_cache_zone != NULL)
	{
		length += sizeof("<moov_cache>\r\n") + cache_stats_len + sizeof("</moov_cache>\r\n");
	}

	if (conf->path_mapping_cache_zone != NULL)
	{
		length += sizeof("<path_mapping_cache>\r\n") + cache_stats_len + sizeof("</path_mapping_cache>\r\n");
	}
	length += sizeof(status_postfix);

	// allocate the buffer
	response.data = ngx_palloc(r->pool, length);
	if (response.data == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_status_handler: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// populate the buffer
	p = ngx_copy(response.data, status_prefix, sizeof(status_prefix) - 1);

	if (conf->moov_cache_zone != NULL)
	{
		ngx_buffer_cache_get_stats(conf->moov_cache_zone, &stats);

		p = ngx_copy(p, "<moov_cache>\r\n", sizeof("<moov_cache>\r\n") - 1);
		p = ngx_http_vod_append_cache_stats(p, &stats);
		p = ngx_copy(p, "</moov_cache>\r\n", sizeof("</moov_cache>\r\n") - 1);
	}

	if (conf->path_mapping_cache_zone != NULL)
	{
		ngx_buffer_cache_get_stats(conf->path_mapping_cache_zone, &stats);

		p = ngx_copy(p, "<path_mapping_cache>\r\n", sizeof("<path_mapping_cache>\r\n") - 1);
		p = ngx_http_vod_append_cache_stats(p, &stats);
		p = ngx_copy(p, "</path_mapping_cache>\r\n", sizeof("</path_mapping_cache>\r\n") - 1);
	}

	p = ngx_copy(p, status_postfix, sizeof(status_postfix) - 1);
	
	response.len = p - response.data;
	
	if (response.len > length)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_status_handler: response length %uz exceeded allocated length %uz", 
			response.len, length);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	return ngx_http_vod_send_response(r, &response, xml_content_type, sizeof(xml_content_type) - 1);
}
