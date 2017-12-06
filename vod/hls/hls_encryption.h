#ifndef __HLS_ENCRYPTION_H__
#define __HLS_ENCRYPTION_H__

// includes
#include "../common.h"
#include "../aes_defs.h"

// typedefs
typedef enum {
	HLS_ENC_NONE,
	HLS_ENC_AES_128,
	HLS_ENC_SAMPLE_AES,
	HLS_ENC_SAMPLE_AES_CENC,
} hls_encryption_type_t;

typedef struct {
	hls_encryption_type_t type;
	u_char* key;
	u_char* iv;
	vod_str_t key_uri;
	bool_t return_iv;
	u_char iv_buf[AES_BLOCK_SIZE];
} hls_encryption_params_t;

#endif // __HLS_ENCRYPTION_H__
