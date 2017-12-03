#include "mp4_aes_ctr.h"

#define MIN_ALLOC_SIZE (16)

static void
mp4_aes_ctr_cleanup(mp4_aes_ctr_state_t* state)
{
	EVP_CIPHER_CTX_free(state->cipher);
}

vod_status_t
mp4_aes_ctr_init(
	mp4_aes_ctr_state_t* state,
	request_context_t* request_context, 
	u_char* key)
{
	vod_pool_cleanup_t *cln;

	state->request_context = request_context;

	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_aes_ctr_init: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}
	
	state->cipher = EVP_CIPHER_CTX_new();
	if (state->cipher == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_aes_ctr_init: EVP_CIPHER_CTX_new failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)mp4_aes_ctr_cleanup;
	cln->data = state;

	if (1 != EVP_EncryptInit_ex(state->cipher, EVP_aes_128_ecb(), NULL, key, NULL))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_aes_ctr_init: EVP_EncryptInit_ex failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

void 
mp4_aes_ctr_set_iv(
	mp4_aes_ctr_state_t* state, 
	u_char* iv)
{
	vod_memcpy(state->counter, iv, MP4_AES_CTR_IV_SIZE);
	vod_memzero(state->counter + MP4_AES_CTR_IV_SIZE, sizeof(state->counter) - MP4_AES_CTR_IV_SIZE);
	state->encrypted_pos = NULL;
	state->encrypted_end = NULL;
}

void
mp4_aes_ctr_increment_be64(u_char* counter)
{
	u_char* cur_pos;

	for (cur_pos = counter + 7; cur_pos >= counter; cur_pos--)
	{
		(*cur_pos)++;
		if (*cur_pos != 0)
		{
			break;
		}
	}
}

vod_status_t
mp4_aes_ctr_process(mp4_aes_ctr_state_t* state, u_char* dest, const u_char* src, uint32_t size)
{
	const u_char* src_end = src + size;
	const u_char* cur_end_pos;
	u_char* encrypted_counter_pos;
	u_char* cur_block;
	u_char* next_block;
	u_char* end_block;
	size_t encrypted_size;
	int out_size;

	while (src < src_end)
	{
		if (state->encrypted_pos >= state->encrypted_end)
		{
			// find the size of the data to encrypt
			encrypted_size = aes_round_up_to_block_exact(src_end - src);
			if (encrypted_size > sizeof(state->counter))
			{
				encrypted_size = sizeof(state->counter);
			}

			// initialize the clear counters (the first counter is already initialized)
			end_block = state->counter + encrypted_size - AES_BLOCK_SIZE;
			for (cur_block = state->counter; cur_block < end_block; cur_block = next_block)
			{
				next_block = cur_block + AES_BLOCK_SIZE;
				vod_memcpy(next_block, cur_block, AES_BLOCK_SIZE);
				mp4_aes_ctr_increment_be64(next_block + 8);
			}

			// encrypt the clear counters
			if (1 != EVP_EncryptUpdate(
				state->cipher,
				state->encrypted_counter,
				&out_size,
				state->counter,
				encrypted_size) ||
				out_size != (int)encrypted_size)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_aes_ctr_process: EVP_EncryptUpdate failed");
				return VOD_UNEXPECTED;
			}

			// update the first counter
			if (encrypted_size > AES_BLOCK_SIZE)
			{
				vod_memcpy(state->counter, end_block, AES_BLOCK_SIZE);
			}
			mp4_aes_ctr_increment_be64(state->counter + 8);

			state->encrypted_end = state->encrypted_counter + encrypted_size;

			encrypted_counter_pos = state->encrypted_counter;
			cur_end_pos = src + encrypted_size;
		}
		else
		{
			encrypted_counter_pos = state->encrypted_pos;
			cur_end_pos = src + (state->encrypted_end - encrypted_counter_pos);
		}

		if (src_end < cur_end_pos)
		{
			cur_end_pos = src_end;
		}

		while (src < cur_end_pos)
		{
			*dest++ = *src++ ^ *encrypted_counter_pos++;
		}

		state->encrypted_pos = encrypted_counter_pos;
	}

	return VOD_OK;
}

vod_status_t
mp4_aes_ctr_write_encrypted(
	mp4_aes_ctr_state_t* state,
	write_buffer_state_t* write_buffer,
	u_char* cur_pos,
	uint32_t write_size)
{
	uint32_t cur_write_size;
	size_t alloc_size;
	u_char* write_end;
	u_char* output;
	vod_status_t rc;

	write_end = cur_pos + write_size;
	while (cur_pos < write_end)
	{
		rc = write_buffer_get_bytes(write_buffer, MIN_ALLOC_SIZE, &alloc_size, &output);
		if (rc != VOD_OK)
		{
			return rc;
		}

		cur_write_size = write_end - cur_pos;
		if (alloc_size < cur_write_size)
		{
			cur_write_size = alloc_size;
		}

		rc = mp4_aes_ctr_process(state, output, cur_pos, cur_write_size);
		if (rc != VOD_OK)
		{
			return rc;
		}
		cur_pos += cur_write_size;
		write_buffer->cur_pos += cur_write_size;
	}

	return VOD_OK;
}
