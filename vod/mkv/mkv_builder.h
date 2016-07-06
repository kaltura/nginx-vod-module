#ifndef __MKV_BUILDER_H__
#define __MKV_BUILDER_H__

// includes
#include "../media_format.h"
#include "../media_set.h"

// typedefs
typedef enum {
	MKV_CLEAR,
	MKV_CLEAR_LEAD,
	MKV_ENCRYPTED,
} mkv_encryption_type_t;

// functions
vod_status_t mkv_build_init_segment(
	request_context_t* request_context,
	media_track_t* track,
	uint64_t track_uid,
	vod_str_t* result);

vod_status_t  mkv_builder_frame_writer_init(
	request_context_t* request_context,
	media_sequence_t* sequence,
	write_callback_t write_callback,
	void* write_context, 
	bool_t reuse_buffers,
	mkv_encryption_type_t encryption_type,
	u_char* iv,
	vod_str_t* response_header,
	size_t* total_fragment_size,
	void** context);

vod_status_t mkv_builder_frame_writer_process(void* context);

#endif //__MKV_BUILDER_H__
