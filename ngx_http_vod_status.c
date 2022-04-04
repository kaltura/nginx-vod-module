// includes
#include "ngx_http_vod_status.h"
#include "ngx_http_vod_module.h"
#include "ngx_http_vod_utils.h"
#include "ngx_http_vod_conf.h"
#include "ngx_perf_counters.h"
#include "ngx_buffer_cache.h"

// macros
#define DEFINE_STAT(x) { { sizeof(#x) - 1, (u_char *) #x }, offsetof(ngx_buffer_cache_stats_t, x) }

// constants
#define PATH_PERF_COUNTERS_OPEN "<performance_counters>\r\n"
#define PATH_PERF_COUNTERS_CLOSE "</performance_counters>\r\n"
#define PERF_COUNTER_FORMAT "<sum>%uA</sum>\r\n<count>%uA</count>\r\n<max>%uA</max>\r\n<max_time>%uA</max_time>\r\n<max_pid>%uA</max_pid>\r\n"

#define PROM_STATUS_PREFIX								\
	"nginx_vod_build_info{version=\"" NGINX_VOD_VERSION "\"} 1\n\n"
#define PROM_VOD_CACHE_METRIC_FORMAT "vod_cache_%V{cache=\"%V\"} %uA\n"
#define PROM_PERF_COUNTER_METRICS						\
	"vod_perf_counter_sum{action=\"%V\"} %uA\n"			\
	"vod_perf_counter_count{action=\"%V\"} %uA\n"		\
	"vod_perf_counter_max{action=\"%V\"} %uA\n"			\
	"vod_perf_counter_max_time{action=\"%V\"} %uA\n"	\
	"vod_perf_counter_max_pid{action=\"%V\"} %uA\n\n"	\

// typedefs
typedef struct {
	int conf_offset;
	ngx_str_t open_tag;
	ngx_str_t close_tag;
} ngx_http_vod_cache_info_t;

typedef struct {
	ngx_str_t name;
	unsigned offset;
} ngx_http_vod_stat_def_t;

// constants
static const u_char status_prefix[] = 
	"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\r\n"
	"<vod>\r\n"
	"<version>" NGINX_VOD_VERSION "</version>\r\n";
static const u_char status_postfix[] = "</vod>\r\n";

static ngx_str_t xml_content_type = ngx_string("text/xml");
static ngx_str_t text_content_type = ngx_string("text/plain");
static ngx_str_t reset_response = ngx_string("OK\r\n");

static ngx_http_vod_stat_def_t buffer_cache_stat_defs[] = {
	DEFINE_STAT(store_ok),
	DEFINE_STAT(store_bytes),
	DEFINE_STAT(store_err),
	DEFINE_STAT(store_exists),
	DEFINE_STAT(fetch_hit),
	DEFINE_STAT(fetch_bytes),
	DEFINE_STAT(fetch_miss),
	DEFINE_STAT(evicted),
	DEFINE_STAT(evicted_bytes),
	DEFINE_STAT(reset),
	DEFINE_STAT(entries),
	DEFINE_STAT(data_size),
	{ ngx_null_string, 0 }
};

static ngx_http_vod_cache_info_t cache_infos[] = {
	{
		offsetof(ngx_http_vod_loc_conf_t, metadata_cache),
		ngx_string("<metadata_cache>\r\n"),
		ngx_string("</metadata_cache>\r\n"),
	},
	{
		offsetof(ngx_http_vod_loc_conf_t, response_cache[CACHE_TYPE_VOD]),
		ngx_string("<response_cache>\r\n"),
		ngx_string("</response_cache>\r\n"),
	},
	{
		offsetof(ngx_http_vod_loc_conf_t, response_cache[CACHE_TYPE_LIVE]),
		ngx_string("<live_response_cache>\r\n"),
		ngx_string("</live_response_cache>\r\n"),
	},
	{
		offsetof(ngx_http_vod_loc_conf_t, mapping_cache[CACHE_TYPE_VOD]),
		ngx_string("<mapping_cache>\r\n"),
		ngx_string("</mapping_cache>\r\n"),
	},
	{
		offsetof(ngx_http_vod_loc_conf_t, mapping_cache[CACHE_TYPE_LIVE]),
		ngx_string("<live_mapping_cache>\r\n"),
		ngx_string("</live_mapping_cache>\r\n"),
	},
	{
		offsetof(ngx_http_vod_loc_conf_t, drm_info_cache),
		ngx_string("<drm_info_cache>\r\n"),
		ngx_string("</drm_info_cache>\r\n"),
	},
};

static u_char*
ngx_http_vod_append_cache_stats(u_char* p, ngx_buffer_cache_stats_t* stats)
{
	ngx_http_vod_stat_def_t* cur_stat;

	for (cur_stat = buffer_cache_stat_defs; cur_stat->name.data != NULL; cur_stat++)
	{
		// opening tag
		*p++ = '<';
		p = ngx_copy(p, cur_stat->name.data, cur_stat->name.len);
		*p++ = '>';

		// value
		p = ngx_sprintf(p, "%uA", *(ngx_atomic_t*)((u_char*)stats + cur_stat->offset));

		// closing tag
		*p++ = '<';
		*p++ = '/';
		p = ngx_copy(p, cur_stat->name.data, cur_stat->name.len);
		*p++ = '>';

		// newline
		*p++ = CR;
		*p++ = LF;
	}

	return p;
}

static ngx_int_t
ngx_http_vod_status_reset(ngx_http_request_t *r)
{
	ngx_http_vod_loc_conf_t *conf;
	ngx_perf_counters_t* perf_counters;
	ngx_buffer_cache_t *cur_cache;
	unsigned i;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);
	perf_counters = ngx_perf_counter_get_state(conf->perf_counters_zone);

	for (i = 0; i < sizeof(cache_infos) / sizeof(cache_infos[0]); i++)
	{
		cur_cache = *(ngx_buffer_cache_t **)((u_char*)conf + cache_infos[i].conf_offset);
		if (cur_cache == NULL)
		{
			continue;
		}

		ngx_buffer_cache_reset_stats(cur_cache);
	}

	if (perf_counters != NULL)
	{
		for (i = 0; i < PC_COUNT; i++)
		{
			perf_counters->counters[i].sum = 0;
			perf_counters->counters[i].count = 0;
			perf_counters->counters[i].max = 0;
			perf_counters->counters[i].max_time = 0;
			perf_counters->counters[i].max_pid = 0;
		}
	}

	return ngx_http_vod_send_response(r, &reset_response, &text_content_type);
}

static ngx_int_t
ngx_http_vod_status_xml_handler(ngx_http_request_t *r)
{
	ngx_buffer_cache_stats_t stats;
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_stat_def_t* cur_stat;
	ngx_perf_counters_t* perf_counters;
	ngx_buffer_cache_t *cur_cache;
	ngx_str_t response;
	u_char* p;
	size_t cache_stats_len = 0;
	size_t result_size;
	unsigned i;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);
	perf_counters = ngx_perf_counter_get_state(conf->perf_counters_zone);

	// calculate the buffer size
	for (cur_stat = buffer_cache_stat_defs; cur_stat->name.data != NULL; cur_stat++)
	{
		cache_stats_len += sizeof("<></>\r\n") - 1 + 2 * cur_stat->name.len + NGX_ATOMIC_T_LEN;
	}

	result_size = sizeof(status_prefix) - 1;
	for (i = 0; i < sizeof(cache_infos) / sizeof(cache_infos[0]); i++)
	{
		cur_cache = *(ngx_buffer_cache_t **)((u_char*)conf + cache_infos[i].conf_offset);
		if (cur_cache == NULL)
		{
			continue;
		}

		result_size += cache_infos[i].open_tag.len + cache_stats_len + cache_infos[i].close_tag.len;
	}

	if (perf_counters != NULL)
	{
		result_size += sizeof(PATH_PERF_COUNTERS_OPEN);
		for (i = 0; i < PC_COUNT; i++)
		{
			result_size += perf_counters_open_tags[i].len + sizeof(PERF_COUNTER_FORMAT) + 5 * NGX_ATOMIC_T_LEN + perf_counters_close_tags[i].len;
		}
		result_size += sizeof(PATH_PERF_COUNTERS_CLOSE);
	}

	result_size += sizeof(status_postfix);

	// allocate the buffer
	response.data = ngx_palloc(r->pool, result_size);
	if (response.data == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_status_xml_handler: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// populate the buffer
	p = ngx_copy(response.data, status_prefix, sizeof(status_prefix) - 1);

	for (i = 0; i < sizeof(cache_infos) / sizeof(cache_infos[0]); i++)
	{
		cur_cache = *(ngx_buffer_cache_t **)((u_char*)conf + cache_infos[i].conf_offset);
		if (cur_cache == NULL)
		{
			continue;
		}

		ngx_buffer_cache_get_stats(cur_cache, &stats);

		p = ngx_copy(p, cache_infos[i].open_tag.data, cache_infos[i].open_tag.len);
		p = ngx_http_vod_append_cache_stats(p, &stats);
		p = ngx_copy(p, cache_infos[i].close_tag.data, cache_infos[i].close_tag.len);
	}

	if (perf_counters != NULL)
	{
		p = ngx_copy(p, PATH_PERF_COUNTERS_OPEN, sizeof(PATH_PERF_COUNTERS_OPEN) - 1);
		for (i = 0; i < PC_COUNT; i++)
		{
			p = ngx_copy(p, perf_counters_open_tags[i].data, perf_counters_open_tags[i].len);
			p = ngx_sprintf(p, PERF_COUNTER_FORMAT, 
				perf_counters->counters[i].sum, 
				perf_counters->counters[i].count, 
				perf_counters->counters[i].max, 
				perf_counters->counters[i].max_time, 
				perf_counters->counters[i].max_pid);
			p = ngx_copy(p, perf_counters_close_tags[i].data, perf_counters_close_tags[i].len);
		}
		p = ngx_copy(p, PATH_PERF_COUNTERS_CLOSE, sizeof(PATH_PERF_COUNTERS_CLOSE) - 1);
	}

	p = ngx_copy(p, status_postfix, sizeof(status_postfix) - 1);
	
	response.len = p - response.data;
	
	if (response.len > result_size)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_status_xml_handler: response length %uz exceeded allocated length %uz",
			response.len, result_size);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	return ngx_http_vod_send_response(r, &response, &xml_content_type);
}

static ngx_int_t
ngx_http_vod_status_prom_handler(ngx_http_request_t *r)
{
	ngx_buffer_cache_stats_t stats;
	ngx_http_vod_stat_def_t* cur_stat;
	ngx_http_vod_loc_conf_t *conf;
	ngx_perf_counters_t* perf_counters;
	ngx_buffer_cache_t *cur_cache;
	ngx_str_t response;
	ngx_str_t cache_name;
	ngx_str_t action;
	unsigned i;
	u_char* p;
	size_t result_size;
	size_t names_len;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);
	perf_counters = ngx_perf_counter_get_state(conf->perf_counters_zone);

	names_len = 0;
	for (cur_stat = buffer_cache_stat_defs; cur_stat->name.data != NULL; cur_stat++)
	{
		names_len += cur_stat->name.len;
	}

	result_size = sizeof(PROM_STATUS_PREFIX) - 1;
	for (i = 0; i < vod_array_entries(cache_infos); i++)
	{
		cur_cache = *(ngx_buffer_cache_t **)((u_char*)conf + cache_infos[i].conf_offset);
		if (cur_cache == NULL)
		{
			continue;
		}

		result_size += (sizeof(PROM_VOD_CACHE_METRIC_FORMAT) - 1 + cache_infos[i].open_tag.len + NGX_ATOMIC_T_LEN) *
			vod_array_entries(buffer_cache_stat_defs) + names_len + sizeof("\n") - 1;
	}

	if (perf_counters != NULL)
	{
		for (i = 0; i < PC_COUNT; i++)
		{
			result_size += sizeof(PROM_PERF_COUNTER_METRICS) - 1 + (perf_counters_open_tags[i].len + NGX_ATOMIC_T_LEN) * 5;
		}
	}

	// allocate the buffer
	p = ngx_palloc(r->pool, result_size);
	if (p == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_status_prom_handler: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	response.data = p;

	p = ngx_copy(p, PROM_STATUS_PREFIX, sizeof(PROM_STATUS_PREFIX) - 1);

	for (i = 0; i < vod_array_entries(cache_infos); i++)
	{
		cur_cache = *(ngx_buffer_cache_t **)((u_char*)conf + cache_infos[i].conf_offset);
		if (cur_cache == NULL)
		{
			continue;
		}

		ngx_buffer_cache_get_stats(cur_cache, &stats);

		cache_name.data = cache_infos[i].open_tag.data + 1;
		cache_name.len = cache_infos[i].open_tag.len - 4;

		for (cur_stat = buffer_cache_stat_defs; cur_stat->name.data != NULL; cur_stat++)
		{
			p = ngx_sprintf(p, PROM_VOD_CACHE_METRIC_FORMAT, &cur_stat->name, &cache_name, *(ngx_atomic_t*)((u_char*)&stats + cur_stat->offset));
		}
		*p++ = '\n';
	}

	if (perf_counters != NULL)
	{
		for (i = 0; i < PC_COUNT; i++)
		{
			action.data = perf_counters_open_tags[i].data + 1;
			action.len = perf_counters_open_tags[i].len - 4;

			p = ngx_sprintf(p, PROM_PERF_COUNTER_METRICS,
				&action, perf_counters->counters[i].sum,
				&action, perf_counters->counters[i].count,
				&action, perf_counters->counters[i].max,
				&action, perf_counters->counters[i].max_time,
				&action, perf_counters->counters[i].max_pid);
		}
	}

	response.len = p - response.data;

	if (response.len > result_size)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_status_prom_handler: response length %uz exceeded allocated length %uz",
			response.len, result_size);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	return ngx_http_vod_send_response(r, &response, &xml_content_type);
}

ngx_int_t
ngx_http_vod_status_handler(ngx_http_request_t *r)
{
	ngx_str_t value;

	if (ngx_http_arg(r, (u_char *) "reset", sizeof("reset") - 1, &value) == NGX_OK &&
		value.len == 1 &&
		value.data[0] == '1')
	{
		return ngx_http_vod_status_reset(r);
	}

	if (ngx_http_arg(r, (u_char *) "format", sizeof("format") - 1, &value) == NGX_OK &&
		value.len == sizeof("prom") - 1 &&
		ngx_strncmp(value.data, "prom", sizeof("prom") - 1) == 0)
	{
		return ngx_http_vod_status_prom_handler(r);
	}

	return ngx_http_vod_status_xml_handler(r);
}
