#include "media_set.h"

int64_t
media_set_get_segment_time_millis(request_context_t* request_context , media_set_t* media_set)
{
	media_track_t* cur_track;
	int64_t time_offset;

	time_offset = request_context->time_offset * 1000;

	// try to find a track that has frames, if no track is found, fallback to the first track
	for (cur_track = media_set->filtered_tracks; ; cur_track += media_set->total_track_count)
	{
		if (cur_track >= media_set->filtered_tracks_end)
		{
			cur_track = media_set->filtered_tracks;
			break;
		}

		if (cur_track->frame_count > 0)
		{
			break;
		}
	}

	return cur_track->original_clip_time - time_offset +
		rescale_time(cur_track->first_frame_time_offset, cur_track->media_info.timescale, 1000);
}
