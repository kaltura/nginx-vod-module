#ifndef __LANGUAGE_CODE_H__
#define __LANGUAGE_CODE_H__

// includes
#include "common.h"

// constants
#define LANG_ISO639_1_LEN (2)
#define LANG_ISO639_2_LEN (3)
#define LANG_MASK_SIZE ((VOD_LANG_COUNT + 7) >> 3)

// macros
#define iso639_2_str_to_int(x) \
	(((((uint16_t)(x)[0]) - 'a' + 1) << 10) | \
	((((uint16_t)(x)[1]) - 'a' + 1) << 5) | \
	((((uint16_t)(x)[2]) - 'a' + 1)))

// typedefs
enum {
#define LANG(id, iso639_1, iso639_2t, iso639_2b, name, native_name) VOD_LANG_##id, 
#include "languages_x.h"
#undef LANG

	VOD_LANG_COUNT
};

typedef uint8_t language_id_t;

// functions
vod_status_t language_code_process_init(vod_pool_t* pool, vod_log_t* log);

language_id_t lang_parse_iso639_2_code(uint16_t code);

void lang_get_native_name(language_id_t id, vod_str_t* result);

const char* lang_get_iso639_1_name(language_id_t id);

const char* lang_get_iso639_2t_name(language_id_t id);

#endif //__LANGUAGE_CODE_H__
