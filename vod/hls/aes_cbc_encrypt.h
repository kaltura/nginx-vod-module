#ifndef __AES_CBC_ENCRYPT_H__
#define __AES_CBC_ENCRYPT_H__

// includes
#include "../aes_defs.h"

// typedefs
typedef struct {
	request_context_t* request_context;
	write_callback_t callback;
	void* callback_context;
#if (VOD_HAVE_OPENSSL_EVP)
	EVP_CIPHER_CTX* cipher;
#endif //(VOD_HAVE_OPENSSL_EVP)
	u_char last_block[AES_BLOCK_SIZE];
} aes_cbc_encrypt_context_t;

// functions
vod_status_t aes_cbc_encrypt_init(
	aes_cbc_encrypt_context_t** ctx,
	request_context_t* request_context,
	write_callback_t callback, 
	void* callback_context, 
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

vod_status_t aes_cbc_encrypt_flush(aes_cbc_encrypt_context_t* ctx);

#endif // __AES_CBC_ENCRYPT_H__
