#include "language_code.h"

// constants
#define ISO639_2_HASH_SIZE (2892)

// globals
static const char* iso639_1_codes[] = {
#define LANG(id, iso639_1, iso639_2t, iso639_2b, name, native_name) iso639_1, 
#include "languages_x.h"
#undef LANG
};

static const char* iso639_2t_codes[] = {
#define LANG(id, iso639_1, iso639_2t, iso639_2b, name, native_name) iso639_2t, 
#include "languages_x.h"
#undef LANG
};

static const char* iso639_2b_codes[] = {
#define LANG(id, iso639_1, iso639_2t, iso639_2b, name, native_name) iso639_2b, 
#include "languages_x.h"
#undef LANG
};

static vod_str_t native_names[] = {
#define LANG(id, iso639_1, iso639_2t, iso639_2b, name, native_name) vod_string(native_name),
#include "languages_x.h"
#undef LANG
};

static language_id_t* iso639_2_hash;

vod_status_t
language_code_process_init(vod_pool_t* pool, vod_log_t* log)
{
	uint16_t index1;
	uint16_t index2;
	unsigned i;
	
	iso639_2_hash = vod_alloc(pool, ISO639_2_HASH_SIZE * sizeof(iso639_2_hash[0]));
	if (iso639_2_hash == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, log, 0,
			"language_code_process_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	
	vod_memzero(iso639_2_hash, ISO639_2_HASH_SIZE * sizeof(iso639_2_hash[0]));
			
	for (i = 1; i < vod_array_entries(iso639_2t_codes); i++)
	{
		// iso639-2t
		index1 = iso639_2_str_to_int(iso639_2t_codes[i]) % ISO639_2_HASH_SIZE;

		if (iso639_2_hash[index1] != 0)
		{
			vod_log_error(VOD_LOG_ERR, log, 0,
				"language_code_process_init: hash table collision in index %uD",
				(uint32_t)index1);
			return VOD_UNEXPECTED;
		}
	
		iso639_2_hash[index1] = i;

		// iso639-2b
		index2 = iso639_2_str_to_int(iso639_2b_codes[i]) % ISO639_2_HASH_SIZE;
		if (index2 == index1)
		{
			continue;
		}

		if (iso639_2_hash[index2] != 0)
		{
			vod_log_error(VOD_LOG_ERR, log, 0,
				"language_code_process_init: hash table collision in index %uD",
				(uint32_t)index2);
			return VOD_UNEXPECTED;
		}

		iso639_2_hash[index2] = i;
	}
	
	return VOD_OK;
}

language_id_t 
lang_parse_iso639_2_code(uint16_t code)
{
	language_id_t result = iso639_2_hash[code % ISO639_2_HASH_SIZE];
	if (result == 0)
	{
		return 0;
	}
	
	if (iso639_2_str_to_int(iso639_2t_codes[result]) != code && 
		iso639_2_str_to_int(iso639_2b_codes[result]) != code)
	{
		return 0;
	}
	
	return result;
}

void
lang_get_native_name(language_id_t id, vod_str_t* result)
{
	*result = native_names[id];
}

const char*
lang_get_iso639_1_name(language_id_t id)
{
	return iso639_1_codes[id];
}

const char*
lang_get_iso639_2t_name(language_id_t id)
{
	return iso639_2t_codes[id];
}
