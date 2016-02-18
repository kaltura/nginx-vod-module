#ifndef __HDS_ENCRYPTION_H__
#define __HDS_ENCRYPTION_H__

// includes
#include "../common.h"

// typedefs
typedef enum {
	HDS_ENC_NONE,
	HDS_ENC_SELECTIVE,		// SE = Selective Encryption
} hds_encryption_type_t;

typedef struct {
	hds_encryption_type_t type;
	u_char* key;
	u_char* iv;
} hds_encryption_params_t;

#endif // __HDS_ENCRYPTION_H__
