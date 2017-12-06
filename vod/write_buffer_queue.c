#include "write_buffer_queue.h"
#include "buffer_pool.h"

#define BUFFER_SIZE (188 * 16 * 32)		// chosen to be a multiple of mpegTS packet size and AES block size

void 
write_buffer_queue_init(
	write_buffer_queue_t* queue, 
	request_context_t* request_context, 
	write_callback_t write_callback,
	void* write_context,
	bool_t reuse_buffers)
{
	queue->request_context = request_context;
	queue->output_buffer_pool = request_context->output_buffer_pool;
	queue->write_callback = write_callback;
	queue->write_context = write_context;
	queue->reuse_buffers = reuse_buffers;

	vod_queue_init(&queue->buffers);
	queue->cur_write_buffer = NULL;
	queue->last_writer_context = NULL;
	queue->cur_offset = 0;
}

u_char*
write_buffer_queue_get_buffer(write_buffer_queue_t* queue, uint32_t size, void* writer_context)
{
	buffer_header_t* write_buffer = queue->cur_write_buffer;
	size_t buffer_size;
	u_char* result;

	// optimization for the common case
	if (write_buffer != NULL && write_buffer->cur_pos + size <= write_buffer->end_pos)
	{
		result = write_buffer->cur_pos;
		write_buffer->cur_pos += size;
		queue->cur_offset += size;
		queue->last_writer_context = writer_context;
		return result;
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
		vod_queue_insert_tail(&queue->buffers, &write_buffer->link);
		queue->cur_write_buffer = write_buffer;
	}

	if (write_buffer->start_pos == NULL)
	{
		// allocate a buffer
		buffer_size = BUFFER_SIZE;
		write_buffer->start_pos = buffer_pool_alloc(queue->request_context, queue->output_buffer_pool, &buffer_size);
		if (write_buffer->start_pos == NULL)
		{
			return NULL;
		}

		write_buffer->cur_pos = write_buffer->start_pos;
		write_buffer->end_pos = write_buffer->start_pos + buffer_size;
	}
	else
	{
		buffer_size = write_buffer->end_pos - write_buffer->start_pos;
	}

	write_buffer->end_offset = queue->cur_offset + buffer_size;

	if (size > buffer_size)
	{
		return NULL;
	}

	result = write_buffer->cur_pos;
	write_buffer->cur_pos += size;
	queue->cur_offset += size;
	queue->last_writer_context = writer_context;
	return result;
}

vod_status_t
write_buffer_queue_send(write_buffer_queue_t* queue, off_t max_offset)
{
	buffer_header_t* cur_buffer;
	vod_status_t rc;

	while (!vod_queue_empty(&queue->buffers))
	{
		cur_buffer = (buffer_header_t*)vod_queue_head(&queue->buffers);
		if (cur_buffer->cur_pos <= cur_buffer->start_pos)
		{
			break;
		}

		if (cur_buffer->end_offset > max_offset)
		{
			break;
		}

		vod_queue_remove(&cur_buffer->link);
		if (cur_buffer == queue->cur_write_buffer)
		{
			queue->cur_write_buffer = NULL;
		}

		rc = queue->write_callback(queue->write_context, cur_buffer->start_pos, cur_buffer->cur_pos - cur_buffer->start_pos);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, queue->request_context->log, 0,
				"write_buffer_queue_send: write_callback failed %i", rc);
			return rc;
		}

		if (!queue->reuse_buffers)
		{
			cur_buffer->start_pos = NULL;
		}
		cur_buffer->cur_pos = cur_buffer->start_pos;
		vod_queue_insert_tail(&queue->buffers, &cur_buffer->link);
	}

	return VOD_OK;
}

vod_status_t 
write_buffer_queue_flush(write_buffer_queue_t* queue)
{
	buffer_header_t* cur_buffer;
	vod_status_t rc;

	while (!vod_queue_empty(&queue->buffers))
	{
		cur_buffer = (buffer_header_t*)vod_queue_head(&queue->buffers);
		vod_queue_remove(&cur_buffer->link);

		if (cur_buffer->cur_pos <= cur_buffer->start_pos)
		{
			continue;
		}

		rc = queue->write_callback(queue->write_context, cur_buffer->start_pos, cur_buffer->cur_pos - cur_buffer->start_pos);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, queue->request_context->log, 0,
				"write_buffer_queue_flush: write_callback failed %i", rc);
			return rc;
		}

		// no reason to reuse the buffer here
	}

	// done
	return VOD_OK;
}
