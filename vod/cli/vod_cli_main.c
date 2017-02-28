// gcc -Wall -g -o repack adts_encoder.c buffer_filter.c mp4_parser.c mp4_to_annexb.c mpegts_encoder.c repackTs.c vod_array.c muxer.c read_cache.c
// ./repack /opt/kaltura/app/alpha/web/repack.ts 0 10

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "mp4_parser.h"
#include "read_cache.h"
#include "muxer.h"

static bool_t 
write_file(void* context, const u_char* buffer, uint32_t size)
{
	int* output_fd = (int*)context;
	int rc;
	
	rc = write(*output_fd, buffer, size);
	if (rc < size)
	{
		return FALSE;
	}
	
	return TRUE;
}

static size_t 
read_file(void* context, u_char *buf, size_t size, off_t offset)
{
	int* input_fd = (int*)context;
	
	return pread(*input_fd, buf, size, offset);
}

int 
main(int argc, const char *argv[])
{
	read_cache_state_t read_cache_state;
	hls_muxer_state_t muxer;
	media_track_array_t track_array;
	input_params_t input_params;
	int output_fd;
	int input_fd;

	vod_memzero(&track_array, sizeof(track_array));
	input_params.start_sec = atoi(argv[2]);
	input_params.end_sec = atoi(argv[3]);

	input_fd = open("/web/content/r70v1/entry/data/80/655/0_vriq23ct_0_g0vnoj5i_1.mp4", O_RDONLY);
	if (input_fd == -1)
	{
	}

	output_fd = creat(argv[1], S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (output_fd == -1)
	{
	}
	
	if (!parse_mpeg_file(read_file, &input_fd, &track_array, &input_params))
	{
	}

	if (!read_cache_init(&read_cache_state, read_file, &input_fd))
	{
	}

	if (!hls_muxer_init(&muxer, &track_array, &read_cache_state, write_file, &output_fd))
	{
	}
	
	hls_muxer_process(&muxer);
	
	close(output_fd);
	close(input_fd);

	return 0;
}
