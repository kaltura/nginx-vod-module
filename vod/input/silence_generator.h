#ifndef __SILENCE_GENERATOR_H__
#define __SILENCE_GENERATOR_H__

// includes
#include "../media_clip.h"
#include "../json_parser.h"

// globals
extern media_generator_t silence_generator;

// functions
vod_status_t silence_generator_parse(
	void* ctx,
	vod_json_object_t* element,
	void** result);

#endif // __SILENCE_GENERATOR_H__
