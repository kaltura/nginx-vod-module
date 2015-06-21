#include "aes_encrypt.h"

vod_status_t 
aes_encrypt_init(
	aes_encrypt_context_t* ctx,
	request_context_t* request_context,
	write_callback_t callback,
	void* callback_context,
	const u_char* key,
	uint32_t segment_index)
{
	u_char iv[AES_BLOCK_SIZE];
	u_char* p;

	ctx->callback = callback;
	ctx->callback_context = callback_context;
	ctx->request_context = request_context;

	// the IV is the segment index in big endian
	vod_memzero(iv, sizeof(iv) - sizeof(uint32_t));
	segment_index += 1;
	p = iv + sizeof(iv) - sizeof(uint32_t);
	*p++ = (u_char)(segment_index >> 24);
	*p++ = (u_char)(segment_index >> 16);
	*p++ = (u_char)(segment_index >> 8);
	*p++ = (u_char)(segment_index);

	EVP_CIPHER_CTX_init(&ctx->cipher);

	if (1 != EVP_EncryptInit_ex(&ctx->cipher, EVP_aes_128_cbc(), NULL, key, iv))
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"aes_encrypt_init: EVP_EncryptInit_ex failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

vod_status_t 
aes_encrypt_write(
	aes_encrypt_context_t* ctx, 
	u_char* buffer, 
	uint32_t size, 
	bool_t* reuse_buffer)
{
	int out_size;

	u_char* encrypted_buffer;
	bool_t unused;

	*reuse_buffer = TRUE;			// we allocate a new buffer

	encrypted_buffer = vod_alloc(ctx->request_context->pool, aes_round_to_block(size));
	if (encrypted_buffer == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, ctx->request_context->log, 0,
			"aes_encrypt_write: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	if (1 != EVP_EncryptUpdate(&ctx->cipher, encrypted_buffer, &out_size, buffer, size))
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"aes_encrypt_write: EVP_EncryptUpdate failed");
		return VOD_UNEXPECTED;
	}

	if (out_size == 0)
	{
		return VOD_OK;
	}

	return ctx->callback(ctx->callback_context, encrypted_buffer, out_size, &unused);
}

vod_status_t 
aes_encrypt_flush(aes_encrypt_context_t* ctx)
{
	int last_block_len;
	bool_t unused;

	if (1 != EVP_EncryptFinal_ex(&ctx->cipher, ctx->last_block, &last_block_len))
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"aes_encrypt_flush: EVP_EncryptFinal_ex failed");
		return VOD_UNEXPECTED;
	}

	if (last_block_len == 0)
	{
		return VOD_OK;
	}

	return ctx->callback(ctx->callback_context, ctx->last_block, last_block_len, &unused);
}

void 
aes_encrypt_cleanup(aes_encrypt_context_t* ctx)
{
	EVP_CIPHER_CTX_cleanup(&ctx->cipher);
}
