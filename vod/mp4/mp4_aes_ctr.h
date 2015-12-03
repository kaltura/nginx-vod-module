#ifndef __MP4_AES_CTR_H__
#define __MP4_AES_CTR_H__

// includes
#include "../common.h"
#include <openssl/evp.h>

// constants
#ifndef AES_BLOCK_SIZE
#define AES_BLOCK_SIZE (16)
#endif // AES_BLOCK_SIZE

#define MP4_AES_CTR_KEY_SIZE (16)
#define MP4_AES_CTR_IV_SIZE (8)
#define MP4_AES_CTR_COUNTER_SIZE (AES_BLOCK_SIZE)

// typedefs
typedef struct {
	request_context_t* request_context;
	EVP_CIPHER_CTX cipher;
	u_char counter[MP4_AES_CTR_COUNTER_SIZE];
	u_char encrypted_counter[MP4_AES_CTR_COUNTER_SIZE];
	int block_offset;
} mp4_aes_ctr_state_t;

// functions
vod_status_t mp4_aes_ctr_init(
	mp4_aes_ctr_state_t* state,
	request_context_t* request_context,
	u_char* key);

void mp4_aes_ctr_set_iv(
	mp4_aes_ctr_state_t* state,
	u_char* iv);

vod_status_t mp4_aes_ctr_process(
	mp4_aes_ctr_state_t* state,
	u_char* dest,
	const u_char* src,
	uint32_t size);

void mp4_aes_ctr_increment_be64(
	u_char* counter);

#endif //__MP4_AES_CTR_H__
