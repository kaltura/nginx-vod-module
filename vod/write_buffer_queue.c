#include "write_buffer_queue.h"

#define BUFFER_SIZE (188 * 16 * 32)		// chosen to be a multiple of mpegTS packet size and AES block size

void 
write_buffer_queue_init(write_buffer_queue_t* queue, request_context_t* request_context)
{
	initialize_list_head(&queue->buffers);
	queue->cur_write_buffer = NULL;
	queue->request_context = request_context;
}

u_char*
write_buffer_queue_get_buffer(write_buffer_queue_t* queue, uint32_t size)
{
	buffer_header_t* write_buffer = queue->cur_write_buffer;
	u_char* result;

	// optimization for the common case
	if (write_buffer != NULL && write_buffer->cur_pos + size <= write_buffer->end_pos)
	{
		result = write_buffer->cur_pos;
		write_buffer->cur_pos += size;
		return result;
	}

	if (size > BUFFER_SIZE)
	{
		return NULL;
	}

	if (write_buffer != NULL)
	{
		// buffer is full, try to move to the next buffer
		if (write_buffer->link.next == &queue->buffers)
		{
			write_buffer = NULL;
		}
		else
		{
			write_buffer = (buffer_header_t*)write_buffer->link.next;
		}
		queue->cur_write_buffer = write_buffer;
	}

	if (write_buffer == NULL)
	{
		// allocate a new link
		write_buffer = vod_alloc(queue->request_context->pool, sizeof(*write_buffer));
		if (write_buffer == NULL)
		{
			return NULL;
		}
		write_buffer->start_pos = NULL;
		insert_tail_list(&queue->buffers, &write_buffer->link);
		queue->cur_write_buffer = write_buffer;
	}

	if (write_buffer->start_pos == NULL)
	{
		// allocate a buffer
		write_buffer->start_pos = vod_alloc(queue->request_context->pool, BUFFER_SIZE);
		if (write_buffer->start_pos == NULL)
		{
			return NULL;
		}

		write_buffer->cur_pos = write_buffer->start_pos;
		write_buffer->end_pos = write_buffer->start_pos + BUFFER_SIZE;
	}

	result = write_buffer->cur_pos;
	write_buffer->cur_pos += size;
	return result;
}

vod_status_t
write_buffer_queue_send(write_buffer_queue_t* queue, u_char* ptr)
{
	buffer_header_t* cur_buffer;
	bool_t reuse_buffer;
	vod_status_t rc;

	while (!is_list_empty(&queue->buffers))
	{
		cur_buffer = (buffer_header_t*)queue->buffers.next;
		if (ptr >= cur_buffer->start_pos && ptr < cur_buffer->end_pos)
		{
			break;
		}

		if (cur_buffer->cur_pos <= cur_buffer->start_pos)
		{
			break;
		}

		remove_entry_list(&cur_buffer->link);
		if (cur_buffer == queue->cur_write_buffer)
		{
			queue->cur_write_buffer = NULL;
		}

		rc = queue->write_callback(queue->write_context, cur_buffer->start_pos, cur_buffer->cur_pos - cur_buffer->start_pos, &reuse_buffer);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, queue->request_context->log, 0,
				"write_buffer_queue_send: write_callback failed %i", rc);
			return rc;
		}

		if (!reuse_buffer)
		{
			cur_buffer->start_pos = NULL;
		}
		cur_buffer->cur_pos = cur_buffer->start_pos;
		insert_tail_list(&queue->buffers, &cur_buffer->link);
	}

	return VOD_OK;
}

vod_status_t 
write_buffer_queue_flush(write_buffer_queue_t* queue)
{
	buffer_header_t* cur_buffer;
	bool_t reuse_buffer;
	vod_status_t rc;

	while (!is_list_empty(&queue->buffers))
	{
		cur_buffer = (buffer_header_t*)queue->buffers.next;
		remove_entry_list(&cur_buffer->link);

		if (cur_buffer->cur_pos <= cur_buffer->start_pos)
		{
			continue;
		}

		rc = queue->write_callback(queue->write_context, cur_buffer->start_pos, cur_buffer->cur_pos - cur_buffer->start_pos, &reuse_buffer);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, queue->request_context->log, 0,
				"write_buffer_queue_flush: write_callback failed %i", rc);
			return rc;
		}

		// no reason to reuse the buffer here
	}

	return VOD_OK;
}

