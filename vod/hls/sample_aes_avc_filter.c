#include "sample_aes_avc_filter.h"

#if (VOD_HAVE_OPENSSL_EVP)

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
	request_context_t* request_context;
	media_filter_write_t write_callback;
	void* write_context;
	u_char iv[AES_BLOCK_SIZE];
	u_char key[SAMPLE_AES_KEY_SIZE];
	EVP_CIPHER_CTX cipher;

	// state
	bool_t encrypt;
	uint32_t cur_offset;
	uint32_t next_encrypt_offset;
	uint32_t max_encrypt_offset;
	uint32_t last_three_bytes;
} sample_aes_avc_filter_state_t;

static u_char emulation_prevention_byte[] = { 0x03 };

static void
sample_aes_avc_cleanup(sample_aes_avc_filter_state_t* state)
{
	EVP_CIPHER_CTX_cleanup(&state->cipher);
}

vod_status_t
sample_aes_avc_filter_init(
	void** context,
	request_context_t* request_context,
	media_filter_write_t write_callback,
	void* write_context,
	u_char* key,
	u_char* iv)
{
	sample_aes_avc_filter_state_t* state;
	vod_pool_cleanup_t *cln;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"sample_aes_avc_filter_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"sample_aes_avc_filter_init: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)sample_aes_avc_cleanup;
	cln->data = state;

	state->request_context = request_context;
	state->write_callback = write_callback;
	state->write_context = write_context;
	vod_memcpy(state->iv, iv, sizeof(state->iv));
	vod_memcpy(state->key, key, sizeof(state->key));

	state->encrypt = FALSE;

	EVP_CIPHER_CTX_init(&state->cipher);

	*context = state;

	return VOD_OK;
}

static vod_status_t
sample_aes_avc_write_emulation_prevention(
	sample_aes_avc_filter_state_t* state, 
	const u_char* buffer, 
	uint32_t size)
{
	const u_char* last_output_pos = buffer;
	const u_char* buffer_end = buffer + size;
	const u_char* cur_pos;
	vod_status_t rc;

	for (cur_pos = buffer; cur_pos < buffer_end; cur_pos++)
	{
		state->last_three_bytes = ((state->last_three_bytes << 8) | *cur_pos) & 0xffffff;
		if (state->last_three_bytes > 3)
		{
			continue;
		}

		state->last_three_bytes = 1;

		if (cur_pos > last_output_pos)
		{
			rc = state->write_callback(state->write_context, last_output_pos, cur_pos - last_output_pos);
			if (rc != VOD_OK)
			{
				return rc;
			}

			last_output_pos = cur_pos;
		}

		rc = state->write_callback(state->write_context, emulation_prevention_byte, sizeof(emulation_prevention_byte));
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return state->write_callback(state->write_context, last_output_pos, buffer_end - last_output_pos);
}

vod_status_t
sample_aes_avc_start_nal_unit(void* context, int unit_type, uint32_t unit_size)
{
	sample_aes_avc_filter_state_t* state = (sample_aes_avc_filter_state_t*)context;

	if ((unit_type != AVC_NAL_SLICE && unit_type != AVC_NAL_IDR_SLICE) || unit_size <= MAX_UNENCRYPTED_UNIT_SIZE)
	{
		state->encrypt = FALSE;
		return VOD_OK;
	}

	state->encrypt = TRUE;
	state->cur_offset = 0;
	state->next_encrypt_offset = FIRST_ENCRYPTED_OFFSET;
	state->max_encrypt_offset = unit_size - AES_BLOCK_SIZE;
	state->last_three_bytes = 1;

	// reset the IV (it is ok to call EVP_EncryptInit_ex several times without cleanup)
	if (1 != EVP_EncryptInit_ex(&state->cipher, EVP_aes_128_cbc(), NULL, state->key, state->iv))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"sample_aes_avc_start_nal_unit: EVP_EncryptInit_ex failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

vod_status_t
sample_aes_avc_filter_write_nal_body(void* context, const u_char* buffer, uint32_t size)
{
	sample_aes_avc_filter_state_t* state = (sample_aes_avc_filter_state_t*)context;
	uint32_t end_offset;
	uint32_t cur_size;
	u_char encrypted_block[AES_BLOCK_SIZE];
	vod_status_t rc;
	int out_size;

	if (!state->encrypt)
	{
		return state->write_callback(state->write_context, buffer, size);
	}

	for (end_offset = state->cur_offset + size;
		state->cur_offset < end_offset;
		state->cur_offset += cur_size, buffer += cur_size)
	{
		if (state->cur_offset < state->next_encrypt_offset)
		{
			// unencrypted part
			cur_size = vod_min(state->next_encrypt_offset, end_offset) - state->cur_offset;
			rc = sample_aes_avc_write_emulation_prevention(state, buffer, cur_size);
			if (rc != VOD_OK)
			{
				return rc;
			}
			continue;
		}

		// encrypted block
		cur_size = vod_min(state->next_encrypt_offset + AES_BLOCK_SIZE, end_offset) - state->cur_offset;

		if (1 != EVP_EncryptUpdate(&state->cipher, encrypted_block, &out_size, buffer, cur_size))
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"sample_aes_avc_filter_write_nal_body: EVP_EncryptUpdate failed");
			return VOD_UNEXPECTED;
		}

		if (out_size <= 0)
		{
			continue;
		}

		// write the encrypted block
		rc = sample_aes_avc_write_emulation_prevention(state, encrypted_block, AES_BLOCK_SIZE);
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

#else

// empty stubs
vod_status_t 
sample_aes_avc_filter_init(
	void** context,
	request_context_t* request_context,
	media_filter_write_t write_callback,
	void* write_context,
	u_char* key,
	u_char* iv)
{
	return VOD_UNEXPECTED;
}

vod_status_t 
sample_aes_avc_start_nal_unit(void* context, int unit_type, uint32_t unit_size)
{
	return VOD_UNEXPECTED;
}

vod_status_t 
sample_aes_avc_filter_write_nal_body(void* context, const u_char* buffer, uint32_t size)
{
	return VOD_UNEXPECTED;
}

#endif //(VOD_HAVE_OPENSSL_EVP)
