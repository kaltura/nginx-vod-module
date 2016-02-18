#ifndef __UDRM_H__
#define __UDRM_H__

// constants
#define DRM_SYSTEM_ID_SIZE (16)
#define DRM_KEY_SIZE (16)
#define DRM_KID_SIZE (16)
#define DRM_IV_SIZE (16)

// typedefs
typedef struct {
	u_char system_id[DRM_SYSTEM_ID_SIZE];
	vod_str_t data;
} drm_system_info_t;

typedef struct {
	uint32_t count;
	drm_system_info_t* first;
	drm_system_info_t* last;
} drm_system_info_array_t;

typedef struct {
	u_char key_id[DRM_KID_SIZE];
	u_char key[DRM_KEY_SIZE];
	u_char iv[DRM_IV_SIZE];
	bool_t iv_set;
	drm_system_info_array_t pssh_array;
} drm_info_t;

// functions
vod_status_t udrm_parse_response(
	request_context_t* request_context,
	vod_str_t* drm_info, 
	bool_t base64_decode_pssh,
	void** output);

vod_status_t udrm_init_parser(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

#endif // __UDRM_H__
