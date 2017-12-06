#include "frame_encrypt_filter.h"

// macros
#define THIS_FILTER (MEDIA_FILTER_ENCRYPT)
#define get_context(ctx) ((frame_encrypt_filter_state_t*)ctx->context[THIS_FILTER])

#include "aes_cbc_encrypt.h"

#define FRAME_ENCRYPT_KEY_SIZE (16)
#define CLEAR_LEAD_SIZE (16)
#define ENCRYPTED_BUFFER_SIZE (256)

// typedefs
typedef struct
{
	// fixed input data
	media_filter_start_frame_t start_frame;
	media_filter_write_t write;
	u_char iv[AES_BLOCK_SIZE];
	u_char key[FRAME_ENCRYPT_KEY_SIZE];

	// state
	EVP_CIPHER_CTX* cipher;
	uint32_t cur_offset;
	uint32_t max_encrypt_offset;
} frame_encrypt_filter_state_t;

static vod_status_t
frame_encrypt_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	frame_encrypt_filter_state_t* state = get_context(context);

	state->cur_offset = 0;
	state->max_encrypt_offset = frame->size - frame->size % AES_BLOCK_SIZE;

	if (state->max_encrypt_offset > CLEAR_LEAD_SIZE)
	{
		// reset the IV (it is ok to call EVP_EncryptInit_ex several times without cleanup)
		if (1 != EVP_EncryptInit_ex(state->cipher, EVP_aes_128_cbc(), NULL, state->key, state->iv))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"frame_encrypt_start_frame: EVP_EncryptInit_ex failed");
			return VOD_ALLOC_FAILED;
		}
	}

	return state->start_frame(context, frame);
}

void
frame_encrypt_start_sub_frame(media_filter_context_t* context, uint32_t size)
{
	frame_encrypt_filter_state_t* state = get_context(context);

	state->cur_offset = 0;
	state->max_encrypt_offset = size - size % AES_BLOCK_SIZE;
}

static vod_status_t
frame_encrypt_write(media_filter_context_t* context, const u_char* buffer, uint32_t size)
{
	frame_encrypt_filter_state_t* state = get_context(context);
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
		rc = state->write(context, buffer, cur_size);
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

		if (1 != EVP_EncryptUpdate(state->cipher, encrypted_buffer, &out_size, buffer, cur_size))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"frame_encrypt_write: EVP_EncryptUpdate failed");
			return VOD_UNEXPECTED;
		}

		buffer += cur_size;
		state->cur_offset += cur_size;

		if (out_size <= 0)
		{
			continue;
		}

		// write the block
		rc = state->write(context, encrypted_buffer, out_size);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// clear trail
	if (state->cur_offset < end_offset)
	{
		return state->write(context, buffer, end_offset - state->cur_offset);
	}

	return VOD_OK;
}

static void
frame_encrypt_cleanup(frame_encrypt_filter_state_t* state)
{
	EVP_CIPHER_CTX_free(state->cipher);
}

vod_status_t
frame_encrypt_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context,
	hls_encryption_params_t* encryption_params)
{
	frame_encrypt_filter_state_t* state;
	request_context_t* request_context = context->request_context;
	vod_pool_cleanup_t *cln;

	// allocate state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"frame_encrypt_filter_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// allocate cleanup item
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"frame_encrypt_filter_init: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}
	
	state->cipher = EVP_CIPHER_CTX_new();
	if (state->cipher == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"frame_encrypt_filter_init: EVP_CIPHER_CTX_new failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)frame_encrypt_cleanup;
	cln->data = state;

	vod_memcpy(state->iv, encryption_params->iv, sizeof(state->iv));
	vod_memcpy(state->key, encryption_params->key, sizeof(state->key));

	// save required functions
	state->start_frame = filter->start_frame;
	state->write = filter->write;

	// override functions
	filter->start_frame = frame_encrypt_start_frame;
	filter->write = frame_encrypt_write;

	// save the context
	context->context[THIS_FILTER] = state;

	return VOD_OK;
}
