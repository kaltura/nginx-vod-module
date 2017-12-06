#include "language_code.h"

#define get_iso639_3_hash_index(code) \
	((((code) >> 10) & 0x1f) - 1)

// globals
static const char* iso639_1_codes[] = {
#define LANG(id, iso639_1, iso639_2b, iso639_3, name, native_name) iso639_1, 
#include "languages_x.h"
#undef LANG
};

static const char* iso639_2b_codes[] = {
#define LANG(id, iso639_1, iso639_2b, iso639_3, name, native_name) iso639_2b, 
#include "languages_x.h"
#undef LANG
};

static const char* iso639_3_codes[] = {
#define LANG(id, iso639_1, iso639_2b, iso639_3, name, native_name) iso639_3, 
#include "languages_x.h"
#undef LANG
};

static vod_str_t native_names[] = {
#define LANG(id, iso639_1, iso639_2b, iso639_3, name, native_name) vod_string(native_name),
#include "languages_x.h"
#undef LANG
};

typedef struct {
	uint16_t offset;
	uint16_t size;
} language_hash_offsets_t;

#include "languages_hash_params.h"

static language_id_t* iso639_3_hash;

vod_status_t
language_code_process_init(vod_pool_t* pool, vod_log_t* log)
{
	const language_hash_offsets_t* hash_offsets;
	uint16_t int_code1;
	uint16_t int_code2;
	uint16_t index;
	unsigned i;
	
	iso639_3_hash = vod_alloc(pool, ISO639_3_HASH_TOTAL_SIZE * sizeof(iso639_3_hash[0]));
	if (iso639_3_hash == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, log, 0,
			"language_code_process_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	
	vod_memzero(iso639_3_hash, ISO639_3_HASH_TOTAL_SIZE * sizeof(iso639_3_hash[0]));
			
	for (i = 1; i < vod_array_entries(iso639_3_codes); i++)
	{
		// iso639-3
		int_code1 = iso639_3_str_to_int(iso639_3_codes[i]);
		hash_offsets = iso639_3_hash_offsets + get_iso639_3_hash_index(int_code1);
		index = hash_offsets->offset + int_code1 % hash_offsets->size;

		if (iso639_3_hash[index] != 0)
		{
			vod_log_error(VOD_LOG_ERR, log, 0,
				"language_code_process_init: hash table collision in index %uD lang %s",
				(uint32_t)index, iso639_3_codes[i]);
			return VOD_UNEXPECTED;
		}
	
		iso639_3_hash[index] = i;

		// iso639-2b
		if (iso639_2b_codes[i] == NULL)
		{
			continue;
		}

		int_code2 = iso639_3_str_to_int(iso639_2b_codes[i]);
		if (int_code2 == int_code1)
		{
			continue;
		}

		hash_offsets = iso639_3_hash_offsets + get_iso639_3_hash_index(int_code2);
		index = hash_offsets->offset + int_code2 % hash_offsets->size;

		if (iso639_3_hash[index] != 0)
		{
			vod_log_error(VOD_LOG_ERR, log, 0,
				"language_code_process_init: hash table collision in index %uD lang %s",
				(uint32_t)index, iso639_2b_codes[i]);
			return VOD_UNEXPECTED;
		}

		iso639_3_hash[index] = i;
	}
	
	return VOD_OK;
}

language_id_t 
lang_parse_iso639_3_code(uint16_t code)
{
	const language_hash_offsets_t* hash_offsets;
	language_id_t result;
	uint16_t hash_index;

	hash_index = get_iso639_3_hash_index(code);
	if (hash_index >= vod_array_entries(iso639_3_hash_offsets))
	{
		return 0;
	}

	hash_offsets = iso639_3_hash_offsets + hash_index;
	result = iso639_3_hash[hash_offsets->offset + code % hash_offsets->size];
	if (result == 0)
	{
		return 0;
	}
	
	if (iso639_3_str_to_int(iso639_3_codes[result]) != code && 
		(iso639_2b_codes[result] == NULL || iso639_3_str_to_int(iso639_2b_codes[result]) != code))
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
lang_get_rfc_5646_name(language_id_t id)
{
	if (iso639_1_codes[id] != NULL)
	{
		return iso639_1_codes[id];
	}
	return iso639_3_codes[id];
}

const char*
lang_get_iso639_3_name(language_id_t id)
{
	return iso639_3_codes[id];
}
