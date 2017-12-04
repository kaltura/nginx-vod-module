#include "sample_aes_avc_filter.h"

// macros
#define THIS_FILTER (MEDIA_FILTER_ENCRYPT)
#define get_context(ctx) ((sample_aes_avc_filter_state_t*)ctx->context[THIS_FILTER])

#include <openssl/evp.h>
#include "aes_cbc_encrypt.h"
#include "../avc_defs.h"

#define SAMPLE_AES_KEY_SIZE (16)
#define MAX_UNENCRYPTED_UNIT_SIZE (48)
#define FIRST_ENCRYPTED_OFFSET (32)
#define ENCRYPTED_BLOCK_PERIOD (10)			// 1 out of 10 blocks is encrypted

typedef struct
{
	// fixed input data
	media_filter_write_t write;
	u_char iv[AES_BLOCK_SIZE];
	u_char key[SAMPLE_AES_KEY_SIZE];
	EVP_CIPHER_CTX* cipher;

	// state
	bool_t encrypt;
	uint32_t cur_offset;
	uint32_t next_encrypt_offset;
	uint32_t max_encrypt_offset;
	uint32_t zero_run;
} sample_aes_avc_filter_state_t;

static u_char emulation_prevention_byte[] = { 0x03 };

static void
sample_aes_avc_cleanup(sample_aes_avc_filter_state_t* state)
{
	EVP_CIPHER_CTX_free(state->cipher);
}

vod_status_t
sample_aes_avc_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context,
	u_char* key,
	u_char* iv)
{
	sample_aes_avc_filter_state_t* state;
	request_context_t* request_context = context->request_context;
	vod_pool_cleanup_t *cln;

	// allocate state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"sample_aes_avc_filter_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// allocate cleanup item
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"sample_aes_avc_filter_init: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}
	
	state->cipher = EVP_CIPHER_CTX_new();
	if (state->cipher == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"sample_aes_avc_filter_init: EVP_CIPHER_CTX_new failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)sample_aes_avc_cleanup;
	cln->data = state;

	state->write = filter->write;
	vod_memcpy(state->iv, iv, sizeof(state->iv));
	vod_memcpy(state->key, key, sizeof(state->key));

	state->encrypt = FALSE;

	// save the context
	context->context[THIS_FILTER] = state;

	return VOD_OK;
}

static vod_status_t
sample_aes_avc_write_emulation_prevention(
	media_filter_context_t* context,
	const u_char* buffer, 
	uint32_t size)
{
	sample_aes_avc_filter_state_t* state = get_context(context);
	const u_char* last_output_pos = buffer;
	const u_char* buffer_end = buffer + size;
	const u_char* cur_pos;
	vod_status_t rc;

	for (cur_pos = buffer; cur_pos < buffer_end; cur_pos++)
	{
		if (state->zero_run < 2)
		{
			if (*cur_pos == 0)
			{
				state->zero_run++;
			}
			else
			{
				state->zero_run = 0;
			}
			continue;
		}

		if ((*cur_pos & ~3) == 0)
		{
			if (cur_pos > last_output_pos)
			{
				rc = state->write(context, last_output_pos, cur_pos - last_output_pos);
				if (rc != VOD_OK)
				{
					return rc;
				}

				last_output_pos = cur_pos;
			}

			rc = state->write(context, emulation_prevention_byte, sizeof(emulation_prevention_byte));
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		state->zero_run = *cur_pos == 0;
	}

	return state->write(context, last_output_pos, buffer_end - last_output_pos);
}

vod_status_t
sample_aes_avc_start_nal_unit(
	media_filter_context_t* context, 
	int unit_type, 
	uint32_t unit_size)
{
	sample_aes_avc_filter_state_t* state = get_context(context);

	if ((unit_type != AVC_NAL_SLICE && unit_type != AVC_NAL_IDR_SLICE) || unit_size <= MAX_UNENCRYPTED_UNIT_SIZE)
	{
		state->encrypt = FALSE;
		return VOD_OK;
	}

	state->encrypt = TRUE;
	state->cur_offset = 0;
	state->next_encrypt_offset = FIRST_ENCRYPTED_OFFSET;
	state->max_encrypt_offset = unit_size - AES_BLOCK_SIZE;
	state->zero_run = 0;

	// reset the IV (it is ok to call EVP_EncryptInit_ex several times without cleanup)
	if (1 != EVP_EncryptInit_ex(state->cipher, EVP_aes_128_cbc(), NULL, state->key, state->iv))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"sample_aes_avc_start_nal_unit: EVP_EncryptInit_ex failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

vod_status_t
sample_aes_avc_filter_write_nal_body(
	media_filter_context_t* context, 
	const u_char* buffer, 
	uint32_t size)
{
	sample_aes_avc_filter_state_t* state = get_context(context);
	uint32_t end_offset;
	uint32_t cur_size;
	u_char encrypted_block[AES_BLOCK_SIZE];
	vod_status_t rc;
	int out_size;

	if (!state->encrypt)
	{
		return state->write(context, buffer, size);
	}

	for (end_offset = state->cur_offset + size;
		state->cur_offset < end_offset;
		state->cur_offset += cur_size, buffer += cur_size)
	{
		if (state->cur_offset < state->next_encrypt_offset)
		{
			// unencrypted part
			cur_size = vod_min(state->next_encrypt_offset, end_offset) - state->cur_offset;
			rc = sample_aes_avc_write_emulation_prevention(context, buffer, cur_size);
			if (rc != VOD_OK)
			{
				return rc;
			}
			continue;
		}

		// encrypted block
		cur_size = vod_min(state->next_encrypt_offset + AES_BLOCK_SIZE, end_offset) - state->cur_offset;

		if (1 != EVP_EncryptUpdate(state->cipher, encrypted_block, &out_size, buffer, cur_size))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"sample_aes_avc_filter_write_nal_body: EVP_EncryptUpdate failed");
			return VOD_UNEXPECTED;
		}

		if (out_size <= 0)
		{
			continue;
		}

		// write the encrypted block
		rc = sample_aes_avc_write_emulation_prevention(context, encrypted_block, AES_BLOCK_SIZE);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// find the next encrypt offset
		state->next_encrypt_offset += ENCRYPTED_BLOCK_PERIOD * AES_BLOCK_SIZE;
		if (state->next_encrypt_offset >= state->max_encrypt_offset)
		{
			state->next_encrypt_offset = UINT_MAX;
		}
	}

	return VOD_OK;
}
