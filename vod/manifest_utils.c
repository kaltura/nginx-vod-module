#include "manifest_utils.h"

// internal flags
#define ADAPTATION_SETS_FLAG_MULTI_AUDIO		(0x1000)
#define ADAPTATION_SETS_FLAG_IGNORE_SUBTITLES	(0x2000)

// typedefs
typedef struct {
	uint32_t codec_id;
	vod_str_t label;
} track_group_key_t;

typedef struct {
	track_group_key_t key;
	media_track_t* head;
	media_track_t* tail;
	uint32_t count;
	vod_queue_t list_node;
	vod_rbtree_node_t rbtree_node;
} track_group_t;

typedef struct {
	vod_rbtree_t rbtree;
	vod_rbtree_node_t sentinel;
	vod_queue_t list;
	uint32_t count;
} track_groups_t;

////// request params formatting functions

static u_char*
manifest_utils_write_bitmask(u_char* p, uint32_t bitmask, u_char letter)
{
	uint32_t i;

	for (i = 0; i < 32; i++)
	{
		if ((bitmask & (1 << i)) == 0)
		{
			continue;
		}

		*p++ = '-';
		*p++ = letter;
		p = vod_sprintf(p, "%uD", i + 1);
	}

	return p;
}

static vod_status_t
manifest_utils_build_request_params_string_per_sequence_tracks(
	request_context_t* request_context, 
	uint32_t segment_index,
	uint32_t sequences_mask,
	uint32_t* sequence_tracks_mask,
	vod_str_t* result)
{
	u_char* p;
	size_t result_size;
	uint32_t* tracks_mask;
	uint32_t i;

	result_size = 0;

	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		result_size += 1 + vod_get_int_print_len(segment_index + 1);
	}

	for (i = 0, tracks_mask = sequence_tracks_mask;
		i < MAX_SEQUENCES;
		i++, tracks_mask += MEDIA_TYPE_COUNT)
	{
		if ((sequences_mask & (1 << i)) == 0)
		{
			continue;
		}

		// sequence
		result_size += sizeof("-f32") - 1;

		// video tracks
		if (tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
		{
			result_size += sizeof("-v0") - 1;
		}
		else
		{
			result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_VIDEO]) * (sizeof("-v32") - 1);
		}

		// audio tracks
		if (tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff)
		{
			result_size += sizeof("-a0") - 1;
		}
		else
		{
			result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_AUDIO]) * (sizeof("-a32") - 1);
		}
	}

	p = vod_alloc(request_context->pool, result_size + 1);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_build_request_params_string_per_sequence_tracks: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result->data = p;

	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		p = vod_sprintf(p, "-%uD", segment_index + 1);
	}

	for (i = 0, tracks_mask = sequence_tracks_mask; 
		i < MAX_SEQUENCES;
		i++, tracks_mask += MEDIA_TYPE_COUNT)
	{
		if ((sequences_mask & (1 << i)) == 0)
		{
			continue;
		}

		// sequence
		p = vod_sprintf(p, "-f%uD", i + 1);

		// video tracks
		switch (tracks_mask[MEDIA_TYPE_VIDEO])
		{
		case 0xffffffff:
			p = vod_copy(p, "-v0", sizeof("-v0") - 1);
			break;

		case 0:
			break;

		default:
			p = manifest_utils_write_bitmask(p, tracks_mask[MEDIA_TYPE_VIDEO], 'v');
			break;
		}

		// audio tracks
		switch (tracks_mask[MEDIA_TYPE_AUDIO])
		{
		case 0xffffffff:
			p = vod_copy(p, "-a0", sizeof("-a0") - 1);
			break;

		case 0:
			break;

		default:
			p = manifest_utils_write_bitmask(p, tracks_mask[MEDIA_TYPE_AUDIO], 'a');
			break;
		}
	}

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"manifest_utils_build_request_params_string_per_sequence_tracks: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

vod_status_t
manifest_utils_build_request_params_string(
	request_context_t* request_context, 
	uint32_t* has_tracks,
	uint32_t segment_index,
	uint32_t sequences_mask,
	uint32_t* sequence_tracks_mask,
	uint32_t* tracks_mask,
	vod_str_t* result)
{
	u_char* p;
	size_t result_size;

	if (sequence_tracks_mask != NULL)
	{
		return manifest_utils_build_request_params_string_per_sequence_tracks(
			request_context,
			segment_index,
			sequences_mask,
			sequence_tracks_mask,
			result);
	}

	result_size = 0;
	
	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		result_size += 1 + vod_get_int_print_len(segment_index + 1);
	}
	
	// sequence mask
	if (sequences_mask != 0xffffffff)
	{
		result_size += vod_get_number_of_set_bits(sequences_mask) * (sizeof("-f32") - 1);
	}

	// video tracks
	if (tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
	{
		result_size += sizeof("-v0") - 1;
	}
	else
	{
		result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_VIDEO]) * (sizeof("-v32") - 1);
	}
	
	// audio tracks
	if (tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff)
	{
		result_size += sizeof("-a0") - 1;
	}
	else
	{
		result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_AUDIO]) * (sizeof("-a32") - 1);
	}
	
	p = vod_alloc(request_context->pool, result_size + 1);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_build_request_params_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result->data = p;

	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		p = vod_sprintf(p, "-%uD", segment_index + 1);
	}
	
	// sequence mask
	if (sequences_mask != 0xffffffff)
	{
		p = manifest_utils_write_bitmask(p, sequences_mask, 'f');
	}

	// video tracks
	if (has_tracks[MEDIA_TYPE_VIDEO])
	{
		if (tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
		{
			p = vod_copy(p, "-v0", sizeof("-v0") - 1);
		}
		else
		{
			p = manifest_utils_write_bitmask(p, tracks_mask[MEDIA_TYPE_VIDEO], 'v');
		}
	}
	
	// audio tracks
	if (has_tracks[MEDIA_TYPE_AUDIO])
	{
		if (tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff)
		{
			p = vod_copy(p, "-a0", sizeof("-a0") - 1);
		}
		else
		{
			p = manifest_utils_write_bitmask(p, tracks_mask[MEDIA_TYPE_AUDIO], 'a');
		}
	}

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"manifest_utils_build_request_params_string: result length %uz exceeded allocated length %uz", 
			result->len, result_size);
		return VOD_UNEXPECTED;
	}
	
	return VOD_OK;
}

u_char*
manifest_utils_append_tracks_spec(
	u_char* p, 
	media_track_t** tracks, 
	uint32_t track_count, 
	bool_t write_sequence_index)
{
	media_sequence_t* cur_sequence;
	media_track_t** last_track_ptr = tracks + track_count;
	media_track_t** cur_track_ptr;
	media_track_t* cur_track;
	uint32_t last_sequence_index;
	bool_t use_sequence_ids = TRUE;
	u_char media_type_letter[] = { 'v', 'a' };		// must match MEDIA_TYPE_xxx in order

	if (write_sequence_index)
	{
		// Note: if there are several different sequences must use sequences indexes instead of ids
		last_sequence_index = INVALID_SEQUENCE_INDEX;
		for (cur_track_ptr = tracks; cur_track_ptr < last_track_ptr; cur_track_ptr++)
		{
			cur_track = *cur_track_ptr;
			if (cur_track == NULL)
			{
				continue;
			}

			cur_sequence = cur_track->file_info.source->sequence;
			if (last_sequence_index == INVALID_SEQUENCE_INDEX)
			{
				last_sequence_index = cur_sequence->index;
			}
			else if (cur_sequence->index != last_sequence_index)
			{
				use_sequence_ids = FALSE;
				break;
			}
		}
	}

	last_sequence_index = INVALID_SEQUENCE_INDEX;
	for (cur_track_ptr = tracks; cur_track_ptr < last_track_ptr; cur_track_ptr++)
	{
		cur_track = *cur_track_ptr;
		if (cur_track == NULL)
		{
			continue;
		}

		if (write_sequence_index)
		{
			cur_sequence = cur_track->file_info.source->sequence;

			if (cur_sequence->index != last_sequence_index)
			{
				last_sequence_index = cur_sequence->index;
				if (cur_sequence->id.len != 0 && cur_sequence->id.len < VOD_INT32_LEN && use_sequence_ids)
				{
					p = vod_sprintf(p, "-s%V", &cur_sequence->id);
				}
				else
				{
					p = vod_sprintf(p, "-f%uD", last_sequence_index + 1);
				}
			}
		}

		if (cur_track->media_info.media_type <= MEDIA_TYPE_AUDIO)
		{
			*p++ = '-';
			*p++ = media_type_letter[cur_track->media_info.media_type];
			p = vod_sprintf(p, "%uD", cur_track->index + 1);
		}
	}

	return p;
}

////// track group functions

static bool_t
track_group_key_init(
	media_track_t* track,
	uint32_t flags,
	track_group_key_t* key)
{
	uint32_t media_type = track->media_info.media_type;

	// label
	switch (media_type)
	{
	case MEDIA_TYPE_VIDEO:
		key->label.len = 0;
		break;

	case MEDIA_TYPE_AUDIO:
		if ((flags & ADAPTATION_SETS_FLAG_MULTI_AUDIO) == 0)
		{
			key->label.len = 0;
			break;
		}

		if (track->media_info.label.len == 0)
		{
			return FALSE;
		}
		key->label = track->media_info.label;
		break;

	case MEDIA_TYPE_SUBTITLE:
		if (track->media_info.label.len == 0 ||
			(flags & ADAPTATION_SETS_FLAG_IGNORE_SUBTITLES) != 0)
		{
			return FALSE;
		}
		key->label = track->media_info.label;
		break;

	default:		// MEDIA_TYPE_NONE
		return FALSE;
	}

	// codec id
	if ((flags & ADAPTATION_SETS_FLAG_MULTI_CODEC) != 0 &&
		media_type != MEDIA_TYPE_SUBTITLE)
	{
		key->codec_id = track->media_info.codec_id;
	}
	else
	{
		key->codec_id = 0;
	}

	return TRUE;
}

static uint32_t
track_group_key_get_hash(track_group_key_t* key)
{
	return key->codec_id + vod_crc32_short(key->label.data, key->label.len);
}

static int
track_group_key_compare(track_group_key_t* key1, track_group_key_t* key2)
{
	if (key1->codec_id != key2->codec_id)
	{
		return key1->codec_id < key2->codec_id ? -1 : 1;
	}

	if (key1->label.len != key2->label.len)
	{
		return key1->label.len < key2->label.len ? -1 : 1;
	}

	if (key1->label.data != key2->label.data)
	{
		return vod_memcmp(key1->label.data, key2->label.data, key1->label.len);
	}

	return 0;
}

static void
track_group_rbtree_insert_value(
	vod_rbtree_node_t *temp,
	vod_rbtree_node_t *node,
	vod_rbtree_node_t *sentinel)
{
	vod_rbtree_node_t **p;
	track_group_t *n, *t;

	for (;;)
	{
		n = vod_container_of(node, track_group_t, rbtree_node);
		t = vod_container_of(temp, track_group_t, rbtree_node);

		if (node->key != temp->key)
		{
			p = (node->key < temp->key) ? &temp->left : &temp->right;
		}
		else
		{
			p = (track_group_key_compare(&n->key, &t->key) < 0)
				? &temp->left : &temp->right;
		}

		if (*p == sentinel)
		{
			break;
		}

		temp = *p;
	}

	*p = node;
	node->parent = temp;
	node->left = sentinel;
	node->right = sentinel;
	vod_rbt_red(node);
}

static track_group_t *
track_group_rbtree_lookup(vod_rbtree_t *rbtree, track_group_key_t* key, uint32_t hash)
{
	vod_rbtree_node_t *node, *sentinel;
	track_group_t *n;
	vod_int_t rc;

	node = rbtree->root;
	sentinel = rbtree->sentinel;

	while (node != sentinel)
	{
		n = vod_container_of(node, track_group_t, rbtree_node);

		if (hash != node->key)
		{
			node = (hash < node->key) ? node->left : node->right;
			continue;
		}

		rc = track_group_key_compare(key, &n->key);
		if (rc < 0)
		{
			node = node->left;
			continue;
		}

		if (rc > 0)
		{
			node = node->right;
			continue;
		}

		return n;
	}

	return NULL;
}

static vod_status_t
track_group_create(
	request_context_t* request_context,
	track_group_key_t* key,
	uint32_t hash,
	media_track_t* track,
	track_groups_t* groups)
{
	track_group_t* group;

	group = vod_alloc(request_context->pool, sizeof(*group));
	if (group == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"track_group_create: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// initialize the group
	group->key = *key;
	group->rbtree_node.key = hash;
	group->count = 1;
	group->head = track;
	group->tail = track;
	track->next = NULL;

	// add to the groups
	vod_queue_insert_tail(&groups->list, &group->list_node);
	vod_rbtree_insert(&groups->rbtree, &group->rbtree_node);
	groups->count++;

	return VOD_OK;
}

static void
track_group_add_track(
	track_group_t* group,
	media_track_t* track,
	uint32_t flags)
{
	// check whether multiple tracks are allowed
	switch (track->media_info.media_type)
	{
	case MEDIA_TYPE_SUBTITLE:
		return;

	case MEDIA_TYPE_AUDIO:
		if (vod_all_flags_set(flags, ADAPTATION_SETS_FLAG_MULTI_AUDIO | ADAPTATION_SETS_FLAG_SINGLE_LANG_TRACK))
		{
			return;
		}
		break;
	}

	// add to existing group
	group->tail->next = track;
	group->tail = track;
	group->count++;
	track->next = NULL;
}

static media_track_t**
track_group_to_adaptation_set(
	track_group_t* group,
	media_track_t** cur_track_ptr,
	adaptation_set_t* result)
{
	media_track_t* cur_track;

	result->first = cur_track_ptr;
	result->count = group->count;
	result->type = group->head->media_info.media_type;

	for (cur_track = group->head; cur_track != NULL; cur_track = cur_track->next)
	{
		*cur_track_ptr++ = cur_track;
	}
	result->last = cur_track_ptr;

	return cur_track_ptr;
}

static void
track_groups_init(
	track_groups_t* result)
{
	track_groups_t* groups;
	uint32_t media_type;

	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		groups = &result[media_type];
		vod_rbtree_init(&groups->rbtree, &groups->sentinel, track_group_rbtree_insert_value);
		vod_queue_init(&groups->list);
		groups->count = 0;
	}
}

static vod_status_t
track_groups_from_media_set(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	uint32_t media_type,
	track_groups_t* result)
{
	media_track_t* last_track;
	media_track_t* cur_track;
	track_groups_t* groups;
	track_group_key_t key = { 0, vod_null_string };
	track_group_t* group;
	vod_status_t rc;
	uint32_t cur_media_type;
	uint32_t hash;

	// initialize the groups objects
	track_groups_init(result);

	last_track = media_set->filtered_tracks + media_set->total_track_count;
	for (cur_track = media_set->filtered_tracks; cur_track < last_track; cur_track++)
	{
		// ignore the track if it doesn't match the requested media type (if there is one)
		cur_media_type = cur_track->media_info.media_type;
		if (media_type != MEDIA_TYPE_NONE && cur_media_type != media_type)
		{
			continue;
		}

		// get the group key
		if (!track_group_key_init(cur_track, flags, &key))
		{
			continue;
		}

		hash = track_group_key_get_hash(&key);

		// look up the group
		groups = &result[cur_media_type];

		group = track_group_rbtree_lookup(&groups->rbtree, &key, hash);
		if (group != NULL)
		{
			track_group_add_track(
				group,
				cur_track,
				flags);
			continue;
		}

		// create a new group
		rc = track_group_create(
			request_context,
			&key,
			hash,
			cur_track,
			groups);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static adaptation_set_t*
track_groups_to_adaptation_sets(
	track_groups_t* groups,
	media_track_t*** cur_track_ptr_arg,
	adaptation_set_t* cur_adaptation_set)
{
	media_track_t** cur_track_ptr = *cur_track_ptr_arg;
	track_group_t* group;
	vod_queue_t* list = &groups->list;
	vod_queue_t* node;

	for (node = vod_queue_head(list); node != list; node = node->next)
	{
		group = vod_container_of(node, track_group_t, list_node);

		cur_track_ptr = track_group_to_adaptation_set(
			group,
			cur_track_ptr,
			cur_adaptation_set);

		cur_adaptation_set++;
	}

	*cur_track_ptr_arg = cur_track_ptr;

	return cur_adaptation_set;
}

////// adaptation sets functions

static bool_t
manifest_utils_is_multi_audio(media_set_t* media_set)
{
	media_track_t* last_track;
	media_track_t* cur_track;
	vod_str_t* label = NULL;

	last_track = media_set->filtered_tracks + media_set->total_track_count;
	for (cur_track = media_set->filtered_tracks; cur_track < last_track; cur_track++)
	{
		if (cur_track->media_info.media_type != MEDIA_TYPE_AUDIO ||
			cur_track->media_info.label.len == 0)
		{
			continue;
		}

		if (label == NULL)
		{
			label = &cur_track->media_info.label;
		}
		else if (!vod_str_equals(cur_track->media_info.label, *label))
		{
			return TRUE;
		}
	}

	return FALSE;
}

static vod_status_t
manifest_utils_get_muxed_adaptation_set(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	vod_str_t* label,
	adaptation_set_t* output)
{
	media_sequence_t* cur_sequence;
	media_track_t** cur_track_ptr;
	media_track_t* audio_track;
	media_track_t* last_track;
	media_track_t* cur_track;
	uint32_t main_media_type;

	// allocate the tracks array
	cur_track_ptr = vod_alloc(request_context->pool, 
		sizeof(output->first[0]) * media_set->total_track_count * MEDIA_TYPE_COUNT);
	if (cur_track_ptr == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_get_muxed_adaptation_set: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	output->type = ADAPTATION_TYPE_MUXED;
	output->first = cur_track_ptr;
	output->count = 0;

	for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
	{
		// find the main media type
		if (cur_sequence->track_count[MEDIA_TYPE_VIDEO] > 0)
		{
			main_media_type = MEDIA_TYPE_VIDEO;
		}
		else if (cur_sequence->track_count[MEDIA_TYPE_AUDIO] > 0)
		{
			if ((flags & ADAPTATION_SETS_FLAG_AVOID_AUDIO_ONLY) != 0 && 
				media_set->track_count[MEDIA_TYPE_VIDEO] > 0)
			{
				continue;
			}

			main_media_type = MEDIA_TYPE_AUDIO;
		}
		else
		{
			continue;
		}

		// find the audio track
		audio_track = cur_sequence->filtered_clips[0].ref_track[MEDIA_TYPE_AUDIO];
		if ((audio_track == NULL || (label != NULL && !vod_str_equals(*label, audio_track->media_info.label))) &&
			media_set->track_count[MEDIA_TYPE_AUDIO] > 0)
		{
			if (cur_sequence->track_count[MEDIA_TYPE_VIDEO] <= 0)
			{
				continue;
			}

			// find some audio track from another sequence to mux with this video
			last_track = media_set->filtered_tracks + media_set->total_track_count;
			for (cur_track = media_set->filtered_tracks; cur_track < last_track; cur_track++)
			{
				if (cur_track->media_info.media_type == MEDIA_TYPE_AUDIO && 
					(label == NULL || vod_str_equals(*label, cur_track->media_info.label)))
				{
					audio_track = cur_track;
					break;
				}
			}
		}

		for (cur_track = cur_sequence->filtered_clips[0].first_track; cur_track < cur_sequence->filtered_clips[0].last_track; cur_track++)
		{
			if (cur_track->media_info.media_type != main_media_type)
			{
				continue;
			}

			// add the track
			if (main_media_type == MEDIA_TYPE_VIDEO)
			{
				cur_track_ptr[MEDIA_TYPE_VIDEO] = cur_track;
				cur_track_ptr[MEDIA_TYPE_AUDIO] = audio_track;
			}
			else
			{
				cur_track_ptr[MEDIA_TYPE_VIDEO] = NULL;
				cur_track_ptr[MEDIA_TYPE_AUDIO] = cur_track;
			}
			cur_track_ptr[MEDIA_TYPE_SUBTITLE] = NULL;

			cur_track_ptr += MEDIA_TYPE_COUNT;

			output->count++;
		}
	}

	output->last = cur_track_ptr;

	return VOD_OK;
}

static vod_status_t
manifest_utils_get_unmuxed_adaptation_sets(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	track_groups_t* groups,
	adaptation_sets_t* output)
{
	adaptation_set_t* cur_adaptation_set;
	adaptation_set_t* adaptation_sets;
	track_group_t* first_audio_group;
	media_track_t** cur_track_ptr;
	vod_status_t rc;
	uint32_t media_type;
	size_t adaptation_sets_count;

	// get the number of adaptation sets
	adaptation_sets_count = groups[MEDIA_TYPE_AUDIO].count + groups[MEDIA_TYPE_SUBTITLE].count;
	output->count[ADAPTATION_TYPE_SUBTITLE] = groups[MEDIA_TYPE_SUBTITLE].count;
	if (groups[MEDIA_TYPE_VIDEO].count > 0 && (flags & ADAPTATION_SETS_FLAG_FORCE_MUXED) != 0)
	{
		output->count[ADAPTATION_TYPE_MUXED] = 1;
		output->count[ADAPTATION_TYPE_VIDEO] = 0;
		output->count[ADAPTATION_TYPE_AUDIO] = groups[MEDIA_TYPE_AUDIO].count - 1;
	}
	else
	{
		adaptation_sets_count += groups[MEDIA_TYPE_VIDEO].count;
		output->count[ADAPTATION_TYPE_MUXED] = 0;
		output->count[ADAPTATION_TYPE_VIDEO] = groups[MEDIA_TYPE_VIDEO].count;
		output->count[ADAPTATION_TYPE_AUDIO] = groups[MEDIA_TYPE_AUDIO].count;
	}

	// allocate the adaptation sets and tracks
	adaptation_sets = vod_alloc(request_context->pool, 
		sizeof(adaptation_sets[0]) * adaptation_sets_count + 
		sizeof(adaptation_sets[0].first[0]) * media_set->total_track_count);
	if (adaptation_sets == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_get_unmuxed_adaptation_sets: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cur_track_ptr = (void*)(adaptation_sets + adaptation_sets_count);
	cur_adaptation_set = adaptation_sets;

	if (output->count[ADAPTATION_TYPE_MUXED] > 0)
	{
		// initialize the muxed adaptation set
		first_audio_group = vod_container_of(
			vod_queue_head(&groups[MEDIA_TYPE_AUDIO].list), track_group_t, list_node);

		output->first_by_type[ADAPTATION_TYPE_MUXED] = cur_adaptation_set;
		rc = manifest_utils_get_muxed_adaptation_set(
			request_context,
			media_set,
			flags,
			&first_audio_group->key.label,
			cur_adaptation_set);
		if (rc != VOD_OK)
		{
			return rc;
		}
		cur_adaptation_set++;
		vod_queue_remove(&first_audio_group->list_node);	// do not output this label separately

		// start from audio (video already added)
		media_type = MEDIA_TYPE_AUDIO;
	}
	else
	{
		// start from video
		media_type = MEDIA_TYPE_VIDEO;
	}

	// initialize all other adaptation sets
	for (; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		output->first_by_type[media_type] = cur_adaptation_set;

		cur_adaptation_set = track_groups_to_adaptation_sets(
			&groups[media_type],
			&cur_track_ptr,
			cur_adaptation_set);
	}

	output->first = adaptation_sets;
	output->last = adaptation_sets + adaptation_sets_count;
	output->total_count = adaptation_sets_count;

	return VOD_OK;
}

vod_status_t
manifest_utils_get_adaptation_sets(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	adaptation_sets_t* output)
{
	track_groups_t groups[MEDIA_TYPE_COUNT];
	media_track_t** cur_track_ptr;
	vod_status_t rc;

	// update flags
	if (manifest_utils_is_multi_audio(media_set))
	{
		flags |= ADAPTATION_SETS_FLAG_MULTI_AUDIO;
		output->multi_audio = TRUE;
	}
	else
	{
		output->multi_audio = FALSE;
	}

	if (media_set->track_count[MEDIA_TYPE_VIDEO] <= 0)
	{
		flags |= ADAPTATION_SETS_FLAG_IGNORE_SUBTITLES;
	}

	if ((flags & ADAPTATION_SETS_FLAG_MULTI_AUDIO) != 0 ||
		vod_no_flag_set(flags, ADAPTATION_SETS_FLAG_MUXED | ADAPTATION_SETS_FLAG_FORCE_MUXED))
	{
		// if multi audio or not muxed, output unmuxed
		rc = track_groups_from_media_set(
			request_context,
			media_set,
			flags,
			MEDIA_TYPE_NONE,
			groups);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (vod_all_flags_set(flags, ADAPTATION_SETS_FLAG_MULTI_AUDIO | ADAPTATION_SETS_FLAG_DEFAULT_LANG_LAST))
		{
			vod_queue_t* first = vod_queue_head(&groups[MEDIA_TYPE_AUDIO].list);
			vod_queue_remove(first);
			vod_queue_insert_tail(&groups[MEDIA_TYPE_AUDIO].list, first);
		}

		rc = manifest_utils_get_unmuxed_adaptation_sets(
			request_context,
			media_set,
			flags,
			groups,
			output);
	}
	else
	{
		// get subtitle track groups
		if (media_set->track_count[MEDIA_TYPE_SUBTITLE] > 0 &&
			media_set->track_count[MEDIA_TYPE_VIDEO] > 0)		// ignore subtitles if there is no video
		{
			rc = track_groups_from_media_set(
				request_context,
				media_set,
				flags,
				MEDIA_TYPE_SUBTITLE,
				groups);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
		else
		{
			// cannot generate muxed media set if there are only subtitles
			if (media_set->track_count[MEDIA_TYPE_VIDEO] + media_set->track_count[MEDIA_TYPE_AUDIO] <= 0)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"manifest_utils_get_adaptation_sets: no audio/video tracks");
				return VOD_BAD_REQUEST;
			}

			groups[MEDIA_TYPE_SUBTITLE].count = 0;
		}

		// initialize the output
		output->count[ADAPTATION_TYPE_MUXED] = 1;
		output->count[ADAPTATION_TYPE_AUDIO] = 0;
		output->count[ADAPTATION_TYPE_VIDEO] = 0;
		output->count[ADAPTATION_TYPE_SUBTITLE] = groups[MEDIA_TYPE_SUBTITLE].count;
		output->total_count = 1 + output->count[ADAPTATION_TYPE_SUBTITLE];

		output->first = vod_alloc(request_context->pool, output->total_count * sizeof(output->first[0]));
		if (output->first == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"manifest_utils_get_adaptation_sets: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}

		output->last = output->first + output->total_count;
		output->first_by_type[ADAPTATION_TYPE_MUXED] = output->first;

		// get the muxed set
		rc = manifest_utils_get_muxed_adaptation_set(
			request_context,
			media_set,
			flags,
			NULL,
			output->first);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// get the subtitle sets
		if (groups[MEDIA_TYPE_SUBTITLE].count > 0)
		{
			output->first_by_type[ADAPTATION_TYPE_SUBTITLE] = output->first + 1;

			cur_track_ptr = output->first->last;

			track_groups_to_adaptation_sets(
				&groups[ADAPTATION_TYPE_SUBTITLE],
				&cur_track_ptr,
				output->first_by_type[ADAPTATION_TYPE_SUBTITLE]);
		}
	}

	return VOD_OK;
}
