# Change log

Note: the list of changes below may not include all changes, it will include mostly "breaking" changes.
	Usually, these are changes that require some update to nginx.conf in order to retain the existing behavior.

## 2016/02/03 - added support for Matroska container

The following configuration settings were removed:
* vod_moov_cache - replaced by vod_metadata_cache
* vod_max_moov_size - replaced by vod_max_metadata_size
	
## 2015/12/15 - removed the upstream module implementation
	
nginx-vod is now built to make use of standard nginx upstream modules (e.g. proxy)
The following configuration settings were removed:
* vod_child_request - use proxy_pass instead
* vod_child_request_path - replaced by vod_xxx_upstream_location
* vod_upstream_host_header - use proxy_set_header instead
* vod_upstream - replaced by vod_upstream_location
* vod_connect_timeout - use proxy_connect_timeout
* vod_send_timeout - use proxy_send_timeout
* vod_read_timeout - use proxy_read_timeout
* vod_fallback_upstream - replaced by vod_fallback_upstream_location
* vod_fallback_connect_timeout - use proxy_connect_timeout
* vod_fallback_send_timeout - use proxy_send_timeout
* vod_fallback_read_timeout - use proxy_read_timeout
* vod_drm_upstream - replaced by vod_drm_upstream_location
* vod_drm_connect_timeout - use proxy_connect_timeout
* vod_drm_send_timeout - use proxy_send_timeout
* vod_drm_read_timeout - use proxy_read_timeout

## 2015/12/06 - added support for MP4 edit lists

nginx-vod now respects edit lists (elst MP4 atom), this can change the set of frames returned in media segments,
and cause errors in case of a live upgrade. To retain the previous behavior, set vod_ignore_edit_list to on.
