#ifndef __AES_DEFS_H__
#define __AES_DEFS_H__

// includes
#include "common.h"

#if (VOD_HAVE_OPENSSL_EVP)
#include <openssl/evp.h>
#endif // VOD_HAVE_OPENSSL_EVP

// constants
#ifndef AES_BLOCK_SIZE
#define AES_BLOCK_SIZE (16)
#endif // AES_BLOCK_SIZE

// macros
#define aes_round_down_to_block(n) ((n) & ~0xf)
#define aes_round_up_to_block(n) aes_round_down_to_block((n) + 0x10)
#define aes_round_up_to_block_exact(n) aes_round_down_to_block((n) + 0xf)

#endif //__AES_DEFS_H__
