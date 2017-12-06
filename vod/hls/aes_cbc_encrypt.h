#ifndef __AES_CBC_ENCRYPT_H__
#define __AES_CBC_ENCRYPT_H__

// includes
#include "../aes_defs.h"

// typedefs
typedef struct {
	request_context_t* request_context;
	buffer_pool_t* buffer_pool;
	write_callback_t callback;
	void* callback_context;
	EVP_CIPHER_CTX* cipher;
	u_char last_block[AES_BLOCK_SIZE];
} aes_cbc_encrypt_context_t;

// functions
vod_status_t aes_cbc_encrypt_init(
	aes_cbc_encrypt_context_t** ctx,
	request_context_t* request_context,
	write_callback_t callback, 
	void* callback_context, 
	buffer_pool_t* buffer_pool,
	const u_char* key,
	const u_char* iv);

vod_status_t aes_cbc_encrypt(
	aes_cbc_encrypt_context_t* state,
	vod_str_t* dest,
	vod_str_t* src, 
	bool_t flush);

vod_status_t aes_cbc_encrypt_write(
	aes_cbc_encrypt_context_t* ctx, 
	u_char* buffer, 
	uint32_t size);

#endif // __AES_CBC_ENCRYPT_H__
