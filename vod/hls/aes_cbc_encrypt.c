#include "aes_cbc_encrypt.h"
#include "../buffer_pool.h"

static void 
aes_cbc_encrypt_cleanup(aes_cbc_encrypt_context_t* state)
{
	EVP_CIPHER_CTX_free(state->cipher);
}

vod_status_t 
aes_cbc_encrypt_init(
	aes_cbc_encrypt_context_t** context,
	request_context_t* request_context,
	write_callback_t callback,
	void* callback_context,
	buffer_pool_t* buffer_pool,
	const u_char* key,
	const u_char* iv)
{
	aes_cbc_encrypt_context_t* state;
	vod_pool_cleanup_t *cln;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"aes_cbc_encrypt_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"aes_cbc_encrypt_init: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}
	
	state->cipher = EVP_CIPHER_CTX_new();
	if (state->cipher == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"aes_cbc_encrypt_init: EVP_CIPHER_CTX_new failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)aes_cbc_encrypt_cleanup;
	cln->data = state;	

	state->callback = callback;
	state->callback_context = callback_context;
	state->request_context = request_context;
	state->buffer_pool = buffer_pool;
	
	if (1 != EVP_EncryptInit_ex(state->cipher, EVP_aes_128_cbc(), NULL, key, iv))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"aes_cbc_encrypt_init: EVP_EncryptInit_ex failed");
		return VOD_ALLOC_FAILED;
	}
	
	*context = state;

	return VOD_OK;
}

vod_status_t 
aes_cbc_encrypt(
	aes_cbc_encrypt_context_t* state,
	vod_str_t* dest,
	vod_str_t* src,
	bool_t flush)
{
	u_char* output;
	int out_size;

	output = vod_alloc(state->request_context->pool, aes_round_up_to_block(src->len) + AES_BLOCK_SIZE);
	if (output == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"aes_cbc_encrypt: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	if (1 != EVP_EncryptUpdate(state->cipher, output, &out_size, src->data, src->len))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"aes_cbc_encrypt: EVP_EncryptUpdate failed");
		return VOD_UNEXPECTED;
	}

	dest->data = output;
	dest->len = out_size;

	if (flush)
	{
		if (1 != EVP_EncryptFinal_ex(state->cipher, output + out_size, &out_size))
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"aes_cbc_encrypt: EVP_EncryptFinal_ex failed");
			return VOD_UNEXPECTED;
		}

		dest->len += out_size;
	}

	return VOD_OK;
}

static vod_status_t
aes_cbc_encrypt_flush(aes_cbc_encrypt_context_t* state)
{
	int last_block_len;

	if (1 != EVP_EncryptFinal_ex(state->cipher, state->last_block, &last_block_len))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"aes_cbc_encrypt_flush: EVP_EncryptFinal_ex failed");
		return VOD_UNEXPECTED;
	}

	if (last_block_len == 0)
	{
		return VOD_OK;
	}

	return state->callback(state->callback_context, state->last_block, last_block_len);
}

vod_status_t 
aes_cbc_encrypt_write(
	aes_cbc_encrypt_context_t* state,
	u_char* buffer,
	uint32_t size)
{
	u_char* encrypted_buffer;
	size_t required_size;
	size_t buffer_size;
	int out_size;

	// zero size means flush
	if (size <= 0)
	{
		return aes_cbc_encrypt_flush(state);
	}

	required_size = aes_round_up_to_block(size);
	buffer_size = required_size;

	encrypted_buffer = buffer_pool_alloc(
		state->request_context,
		state->buffer_pool,
		&buffer_size);
	if (encrypted_buffer == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"aes_cbc_encrypt_write: buffer_pool_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	if (buffer_size < required_size)
	{
		// Note: this should never happen since the buffer pool size is a multiple of 16
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"aes_cbc_encrypt_write: allocated size %uz smaller than required size %uz", 
			buffer_size, required_size);
		return VOD_UNEXPECTED;
	}

	if (1 != EVP_EncryptUpdate(state->cipher, encrypted_buffer, &out_size, buffer, size))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"aes_cbc_encrypt_write: EVP_EncryptUpdate failed");
		return VOD_UNEXPECTED;
	}

	if (out_size == 0)
	{
		return VOD_OK;
	}

	return state->callback(state->callback_context, encrypted_buffer, out_size);
}
