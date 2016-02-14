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
ngx_int_t udrm_parse_response(
	request_context_t* request_context,
	ngx_str_t* drm_info, 
	void** output);

ngx_int_t udrm_init_parser(
	ngx_conf_t* cf);

#endif // __UDRM_H__
