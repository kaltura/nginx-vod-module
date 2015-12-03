#include "mp4_aes_ctr.h"

static void
mp4_aes_ctr_cleanup(mp4_aes_ctr_state_t* state)
{
	EVP_CIPHER_CTX_cleanup(&state->cipher);
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

	cln->handler = (vod_pool_cleanup_pt)mp4_aes_ctr_cleanup;
	cln->data = state;

	EVP_CIPHER_CTX_init(&state->cipher);

	if (1 != EVP_EncryptInit_ex(&state->cipher, EVP_aes_128_ecb(), NULL, key, NULL))
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
	state->block_offset = 0;
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
	int out_size;

	while (src < src_end)
	{
		if (state->block_offset == 0)
		{
			if (1 != EVP_EncryptUpdate(
				&state->cipher,
				state->encrypted_counter,
				&out_size,
				state->counter,
				sizeof(state->counter)) ||
				out_size != sizeof(state->encrypted_counter))
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_aes_ctr_process: EVP_EncryptUpdate failed");
				return VOD_UNEXPECTED;
			}

			mp4_aes_ctr_increment_be64(state->counter + 8);
		}

		encrypted_counter_pos = state->encrypted_counter + state->block_offset;
		cur_end_pos = src + MP4_AES_CTR_COUNTER_SIZE - state->block_offset;
		cur_end_pos = vod_min(cur_end_pos, src_end);

		state->block_offset += cur_end_pos - src;
		state->block_offset &= (MP4_AES_CTR_COUNTER_SIZE - 1);

		while (src < cur_end_pos)
		{
			*dest++ = *src++ ^ *encrypted_counter_pos++;
		}
	}

	return VOD_OK;
}
