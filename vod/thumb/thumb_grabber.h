#ifndef __THUMB_GRABBER_H__
#define __THUMB_GRABBER_H__

// includes
#include "../media_format.h"

// functions
void thumb_grabber_process_init(vod_log_t* log);

vod_status_t thumb_grabber_init_state(
	request_context_t* request_context,
	media_track_t* track,
	uint64_t requested_time,
	bool_t accurate,
	write_callback_t write_callback,
	void* write_context,
	void** result);

vod_status_t thumb_grabber_process(void* context);

#endif //__THUMB_GRABBER_H__
