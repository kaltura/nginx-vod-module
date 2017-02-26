#include "sample_aes_aac_filter.h"

#if (VOD_HAVE_OPENSSL_EVP)

#include <openssl/aes.h>
#include "aes_cbc_encrypt.h"

#define SAMPLE_AES_KEY_SIZE (16)
#define CLEAR_LEAD_SIZE (16)
#define ENCRYPTED_BUFFER_SIZE (256)

typedef struct
{
	// fixed input data
	request_context_t* request_context;
	const media_filter_t* next_filter;
	void* next_filter_context;
	u_char iv[AES_BLOCK_SIZE];
	u_char key[SAMPLE_AES_KEY_SIZE];
	EVP_CIPHER_CTX cipher;

	// state
	uint32_t cur_offset;
	uint32_t max_encrypt_offset;
} sample_aes_aac_filter_state_t;

static void
sample_aes_aac_cleanup(sample_aes_aac_filter_state_t* state)
{
	EVP_CIPHER_CTX_cleanup(&state->cipher);
}

vod_status_t
sample_aes_aac_filter_init(
	void** context,
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	u_char* key,
	u_char* iv)
{
	sample_aes_aac_filter_state_t* state;
	vod_pool_cleanup_t *cln;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"sample_aes_aac_filter_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"sample_aes_aac_filter_init: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)sample_aes_aac_cleanup;
	cln->data = state;

	state->request_context = request_context;
	state->next_filter = next_filter;
	state->next_filter_context = next_filter_context;
	vod_memcpy(state->iv, iv, sizeof(state->iv));
	vod_memcpy(state->key, key, sizeof(state->key));

	EVP_CIPHER_CTX_init(&state->cipher);

	*context = state;

	return VOD_OK;
}

vod_status_t
sample_aes_aac_start_frame(void* context, output_frame_t* frame)
{
	sample_aes_aac_filter_state_t* state = (sample_aes_aac_filter_state_t*)context;

	state->cur_offset = 0;
	state->max_encrypt_offset = frame->size - frame->size % AES_BLOCK_SIZE;

	if (state->max_encrypt_offset > CLEAR_LEAD_SIZE)
	{
		// reset the IV (it is ok to call EVP_EncryptInit_ex several times without cleanup)
		if (1 != EVP_EncryptInit_ex(&state->cipher, EVP_aes_128_cbc(), NULL, state->key, state->iv))
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"sample_aes_aac_start_frame: EVP_EncryptInit_ex failed");
			return VOD_ALLOC_FAILED;
		}
	}

	return state->next_filter->start_frame(state->next_filter_context, frame);
}

vod_status_t
sample_aes_aac_write(void* context, const u_char* buffer, uint32_t size)
{
	sample_aes_aac_filter_state_t* state = (sample_aes_aac_filter_state_t*)context;
	uint32_t offset_limit;
	uint32_t end_offset;
	uint32_t cur_size;
	u_char encrypted_buffer[ENCRYPTED_BUFFER_SIZE];
	vod_status_t rc;
	int out_size;

	end_offset = state->cur_offset + size;

	// clear lead
	if (state->cur_offset < CLEAR_LEAD_SIZE)
	{
		cur_size = vod_min(size, CLEAR_LEAD_SIZE - state->cur_offset);
		rc = state->next_filter->write(state->next_filter_context, buffer, cur_size);
		if (rc != VOD_OK)
		{
			return rc;
		}

		buffer += cur_size;
		state->cur_offset += cur_size;
	}

	// encrypted part
	offset_limit = vod_min(end_offset, state->max_encrypt_offset);
	while (state->cur_offset < offset_limit)
	{
		// encrypt as much as possible
		cur_size = vod_min(sizeof(encrypted_buffer), offset_limit - state->cur_offset);

		if (1 != EVP_EncryptUpdate(&state->cipher, encrypted_buffer, &out_size, buffer, cur_size))
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"sample_aes_aac_filter_write_frame_body: EVP_EncryptUpdate failed");
			return VOD_UNEXPECTED;
		}

		buffer += cur_size;
		state->cur_offset += cur_size;

		if (out_size <= 0)
		{
			continue;
		}

		// write the block
		rc = state->next_filter->write(state->next_filter_context, encrypted_buffer, out_size);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// clear trail
	if (state->cur_offset < end_offset)
	{
		return state->next_filter->write(state->next_filter_context, buffer, end_offset - state->cur_offset);
	}

	return VOD_OK;
}

static vod_status_t
sample_aes_aac_flush_frame(void* context, bool_t last_stream_frame)
{
	sample_aes_aac_filter_state_t* state = (sample_aes_aac_filter_state_t*)context;

	return state->next_filter->flush_frame(state->next_filter_context, last_stream_frame);
}

static void
sample_aes_aac_simulated_start_frame(void* context, output_frame_t* frame)
{
	sample_aes_aac_filter_state_t* state = (sample_aes_aac_filter_state_t*)context;

	state->next_filter->simulated_start_frame(state->next_filter_context, frame);
}

static void
sample_aes_aac_simulated_write(void* context, uint32_t size)
{
	sample_aes_aac_filter_state_t* state = (sample_aes_aac_filter_state_t*)context;

	state->next_filter->simulated_write(state->next_filter_context, size);
}

static void
sample_aes_aac_simulated_flush_frame(void* context, bool_t last_stream_frame)
{
	sample_aes_aac_filter_state_t* state = (sample_aes_aac_filter_state_t*)context;

	state->next_filter->simulated_flush_frame(state->next_filter_context, last_stream_frame);
}


const media_filter_t sample_aes_aac = {
	sample_aes_aac_start_frame,
	sample_aes_aac_write,
	sample_aes_aac_flush_frame,
	sample_aes_aac_simulated_start_frame,
	sample_aes_aac_simulated_write,
	sample_aes_aac_simulated_flush_frame,
};

#else

// empty stubs
vod_status_t 
sample_aes_aac_filter_init(
	void** context,
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	u_char* key,
	u_char* iv)
{
	return VOD_UNEXPECTED;
}

const media_filter_t sample_aes_aac = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

#endif //(VOD_HAVE_OPENSSL_EVP)
