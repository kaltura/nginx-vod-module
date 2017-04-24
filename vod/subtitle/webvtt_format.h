#ifndef __WEBVTT_FORMAT_H__
#define __WEBVTT_FORMAT_H__

// includes
#include "../media_format.h"

// globals
extern media_format_t webvtt_format;

// functions
void webvtt_init_process(vod_log_t* log);
void webvtt_exit_process();

#endif //__WEBVTT_FORMAT_H__
