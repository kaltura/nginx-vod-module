#ifndef __AES_ENCRYPT_H__
#define __AES_ENCRYPT_H__

// TODO make it optional, like #if (NGX_OPENSSL)

// includes
#include <openssl/evp.h>
#include "common.h"

// macros
#ifndef AES_BLOCK_SIZE
#define AES_BLOCK_SIZE (16)
#endif // AES_BLOCK_SIZE
#define aes_round_to_block(n) (((n) + 0x10) & ~0xf)

// typedefs
typedef struct {
	request_context_t* request_context;
	write_callback_t callback;
	void* callback_context;
	EVP_CIPHER_CTX cipher;
	u_char last_block[AES_BLOCK_SIZE];
} aes_encrypt_context_t;

// functions
vod_status_t aes_encrypt_init(
	aes_encrypt_context_t* ctx, 
	request_context_t* request_context, 
	write_callback_t callback, 
	void* callback_context, 
	const u_char* key, 
	uint32_t segment_index);

vod_status_t aes_encrypt_write(
	aes_encrypt_context_t* ctx, 
	u_char* buffer, 
	uint32_t size, 
	bool_t* reuse_buffer);

vod_status_t aes_encrypt_flush(aes_encrypt_context_t* ctx);

void aes_encrypt_cleanup(aes_encrypt_context_t* ctx);

#endif // __AES_ENCRYPT_H__
