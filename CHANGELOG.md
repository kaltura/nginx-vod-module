# Change log

Note: the list of changes below may not include all changes, it will include mostly "breaking" changes.
	Usually, these are changes that require some update to nginx.conf in order to retain the existing behavior.

## 2016/06/09 - live timing enhancements

The following configuration settings were removed:
* vod_live_segment_count - use vod_live_window_duration instead, multiply by vod_segment_duration.
	
## 2016/05/08 - provide more control of the domain of returned URLs

The following configuration settings were removed:
* vod_https_header_name - use vod_base_url instead, e.g. if vod_https_header_name was set
	to `my-https-header`, the updated config may look like:
```
http {

	map $http_my_https_header $protocol {
		default             "http";
		"ON"                "https";
	}

	server {

		if ($http_host != "") {
			set $base_url "$protocol://$http_host";
		}
	
		if ($http_host = "") {
			set $base_url "";		# no host header - use relative urls
		}

		vod_base_url $base_url;
```

The behavior of the following configurations were changed:
* vod_segments_base_url - when this variable is defined and evaluates to a non-empty string,
	it is assumed to contain both the scheme and the host name. Before the change, when the 
	url did not contain a scheme, a defualt scheme was added.

## 2016/03/06 - ad stitching supporting features

The following configuration settings were removed:
* vod_path_mapping_cache - replaced by vod_mapping_cache
* vod_live_path_mapping_cache - replaced by vod_live_mapping_cache
	
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
