#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"
#include <ctype.h>

#define ASS_SCRIPT_INFO_HEADER ("[Script Info]")

// utf8 functions
#define CHAR_TYPE u_char
#define METHOD(x) x
#include "webvtt_format_template.h"
#undef CHAR_TYPE
#undef METHOD

typedef struct {
	uint8_t wrap_style;
	uint8_t script_type;
	uint32_t player_res_x;
	uint32_t player_res_y;
} ass_script_info_t;


static vod_status_t
ass_reader_init(
	request_context_t* request_context,
	vod_str_t* buffer,
	size_t initial_read_size,
	size_t max_metadata_size,
	void** ctx)
{
    // RAFIK: here we should read the entire [SCRIPT_INFO] and [V4+ FORMAT] sections, in addition to header of Dialogue
	u_char* p = buffer->data;

    if (buffer->len > 0 &&
        vod_strncmp(p, ASS_SCRIPT_INFO_HEADER, sizeof(ASS_SCRIPT_INFO_HEADER) - 1) != 0)
    {
        vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
            "ass_reader_init failed, len=%d", buffer->len);
        return VOD_NOT_FOUND;
    }
    vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
        "ass_reader_init passed and called subtitle_reader_init");


	return subtitle_reader_init(
		request_context,
		initial_read_size,
		ctx);
}

static vod_status_t
ass_parse(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* source,
	size_t metadata_part_count,
	media_base_metadata_t** result)
{
	return subtitle_parse(
		request_context,
		parse_params,
		source,
		NULL,
		ass_estimate_duration(source),
		metadata_part_count,
		result);
}

static vod_status_t
ass_parse_frames(
	request_context_t* request_context,
	media_base_metadata_t* base,
	media_parse_params_t* parse_params,
	struct segmenter_conf_s* segmenter,
	read_cache_state_t* read_cache_state,
	vod_str_t* frame_data,
	media_format_read_request_t* read_req,
	media_track_array_t* result)
{
	return VOD_OK;
}

media_format_t ass_format = {
	FORMAT_ID_ASS,
	vod_string("ass_or_ssa"),
	ass_reader_init,
	subtitle_reader_read,
	NULL,
	NULL,
	ass_parse,
	ass_parse_frames,
};
