# NGINX-based VOD Packager
## nginx-vod-module

### Features

* On-the-fly repackaging of MP4 files to DASH, HDS, HLS, MSS

* Adaptive bitrate support

* Working modes:
  1. Local - serve locally accessible files (local disk/NFS mounted)
  2. Remote - serve files accessible via HTTP using range requests
  3. Mapped - perform an HTTP request to map the input URI to a locally accessible file

* Fallback support for file not found in local/mapped modes
  
* Video codecs: H264, H265 (dash only)

* Audio codecs: AAC

* Audio only/video only files

* Track selection for multi audio/video MP4 files

* Source file clipping (only from I-Frame to P-frame)

* Serving of files for progressive download playback

* HLS: Generation of I-frames playlist (EXT-X-I-FRAMES-ONLY)

* HLS: AES-128 encryption support

### Limitations

* Only AAC audio is supported (MP3 audio is not)

* Clipping is not supported in progressive download

* I-frames playlist generation is not supported when encryption is enabled

* SAMPLE-AES encryption is not supported

* Tested on Linux only

### Nginx patch

The module depends on a small patch to the nginx code - in src/http/ngx_http_upstream.c, in ngx_http_upstream_process_body_in_memory replace:
	for ( ;; ) {
with:
	while (u->length) {
	
The problem with the existing code is that when the upstream buffer size matches the response size exactly, the upstream module fails with 'upstream buffer is too small to read response' instead of completing the result successfully.
Since the nginx-vod-module performs range requests and sets the upstream buffer to the exact response size, this error always happens.

### Build

cd to NGINX source directory and execute:

    ./configure --add-module=/path/to/nginx-vod-module
    make
    make install
	
For asynchronous I/O support add `--with-file-aio` (highly recommended, local and mapped modes only)

    ./configure --add-module=/path/to/nginx-vod-module --with-file-aio
	
To compile nginx with debug messages add `--with-debug`

    ./configure --add-module=/path/to/nginx-vod-module --with-debug

To disable compiler optimizations (for debugging with gdb) add `CFLAGS="-g -O0"`

	CFLAGS="-g -O0" ./configure ....

### Common configuration directives

#### vod
* **syntax**: `vod segmenter`
* **default**: `n/a`
* **context**: `location`

Enables the nginx-vod module on the enclosing location. 
Currently the allowed values for `segmenter` are:
1. `none` - serves the MP4 files as is
2. `dash` - Dynamic Adaptive Streaming over HTTP packetizer
2. `hds` - Adobe HTTP Dynamic Streaming packetizer
2. `hls` - Apple HTTP Live Streaming packetizer
2. `mss` - Microsoft Smooth Streaming packetizer

#### vod_mode
* **syntax**: `vod_mode mode`
* **default**: `local`
* **context**: `http`, `server`, `location`

Sets the file access mode - local, remote or mapped

#### vod_status
* **syntax**: `vod_status`
* **default**: `n/a`
* **context**: `location`

Enables the nginx-vod status page on the enclosing location. 

#### vod_multi_uri_suffix
* **syntax**: `vod_multi_uri_suffix suffix`
* **default**: `.urlset`
* **context**: `http`, `server`, `location`

A URL suffix that is used to identify multi URLs. A multi URL is a way to encode several different URLs
that should be played together as an adaptive streaming, under a single URL. When the default suffix is
used, an HLS set URL may look like: 
http://host/hls/common-prefix,bitrate1,bitrate2,common-suffix.urlset/master.m3u8

#### vod_segment_duration
* **syntax**: `vod_segment_duration duration`
* **default**: `10s`
* **context**: `http`, `server`, `location`

Sets the HLS segment duration in milliseconds.

#### vod_secret_key
* **syntax**: `vod_secret_key string`
* **default**: `empty`
* **context**: `http`, `server`, `location`

Sets the secret that is used to generate the TS encryption key, if empty, no encryption is performed.

#### vod_duplicate_bitrate_threshold
* **syntax**: `vod_duplicate_bitrate_threshold threshold`
* **default**: `4096`
* **context**: `http`, `server`, `location`

The bitrate threshold for removing identical bitrates, streams whose bitrate differences are less than
this value will be considered identical.

#### vod_https_header_name
* **syntax**: `vod_https_header_name name`
* **default**: `empty`
* **context**: `http`, `server`, `location`

Sets the name of an HTTP header whose existence determines whether the request was issued over HTTPS.
If not set, the decision is made according to the protocol used to connect to the nginx server.
A common scenario for using this setting is a load-balancer placed before the nginx that performs SSL-offloading.

#### vod_moov_cache
* **syntax**: `vod_moov_cache zone_name zone_size`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the size and shared memory object name of the moov atom cache

#### vod_initial_read_size
* **syntax**: `vod_initial_read_size size`
* **default**: `4K`
* **context**: `http`, `server`, `location`

Sets the size of the initial read operation of the MP4 file.

#### vod_max_moov_size
* **syntax**: `vod_max_moov_size size`
* **default**: `32MB`
* **context**: `http`, `server`, `location`

Sets the maximum supported MP4 moov atom size.

#### vod_cache_buffer_size
* **syntax**: `vod_cache_buffer_size size`
* **default**: `64K`
* **context**: `http`, `server`, `location`

Sets the size of the cache buffers used when reading MP4 frames.

#### vod_child_request
* **syntax**: `vod_child_request`
* **default**: `n/a`
* **context**: `location`

Configures the enclosing location as handling nginx-vod module child requests (remote/mapped modes only)
There should be at least one location with this command when working in remote/mapped modes.
Note that multiple vod locations can point to a single location having vod_child_request.

#### vod_child_request_path
* **syntax**: `vod_child_request_path path`
* **default**: `none`
* **context**: `location`

Sets the path of an internal location that has vod_child_request enabled (remote/mapped modes only)

#### vod_upstream
* **syntax**: `vod_upstream upstream_name`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the upstream that should be used for reading the MP4 file (remote mode) or mapping the request URI (mapped mode).

#### vod_upstream_host_header
* **syntax**: `vod_upstream_host_header host_name`
* **default**: `the host name of original request`
* **context**: `http`, `server`, `location`

Sets the value of the HTTP host header that should be sent to the upstream (remote/mapped modes only).

#### vod_upstream_extra_args
* **syntax**: `vod_upstream_extra_args "arg1=value1&arg2=value2&..."`
* **default**: `empty`
* **context**: `http`, `server`, `location`

Extra query string arguments that should be added to the upstream request (remote/mapped modes only).

#### vod_connect_timeout
* **syntax**: `vod_connect_timeout timeout`
* **default**: `60s`
* **context**: `http`, `server`, `location`

Sets the timeout in milliseconds for connecting to the upstream (remote/mapped modes only).

#### vod_send_timeout
* **syntax**: `vod_send_timeout timeout`
* **default**: `60s`
* **context**: `http`, `server`, `location`

Sets the timeout in milliseconds for sending data to the upstream (remote/mapped modes only).

#### vod_read_timeout
* **syntax**: `vod_read_timeout timeout`
* **default**: `60s`
* **context**: `http`, `server`, `location`

Sets the timeout in milliseconds for reading data from the upstream (remote/mapped modes only).

#### vod_path_mapping_cache
* **syntax**: `vod_path_mapping_cache zone_name zone_size`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the size and shared memory object name of the path mapping cache (mapped mode only).

#### vod_path_response_prefix
* **syntax**: `vod_path_response_prefix prefix`
* **default**: `<?xml version="1.0" encoding="utf-8"?><xml><result>`
* **context**: `http`, `server`, `location`

Sets the prefix that is expected in URI mapping responses (mapped mode only).

#### vod_path_response_postfix
* **syntax**: `vod_path_response_postfix postfix`
* **default**: `</result></xml>`
* **context**: `http`, `server`, `location`

Sets the postfix that is expected in URI mapping responses (mapped mode only).

#### vod_max_path_length
* **syntax**: `vod_max_path_length length`
* **default**: `1K`
* **context**: `http`, `server`, `location`

Sets the maximum length of a path returned from upstream (mapped mode only).

#### vod_fallback_upstream
* **syntax**: `vod_fallback_upstream upstream_name`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets an upstream to forward the request to when encountering a file not found error (local/mapped modes only).

#### vod_fallback_connect_timeout
* **syntax**: `vod_fallback_connect_timeout timeout`
* **default**: `60s`
* **context**: `http`, `server`, `location`

Sets the timeout in milliseconds for connecting to the fallback upstream (local/mapped modes only).

#### vod_fallback_send_timeout
* **syntax**: `vod_fallback_send_timeout timeout`
* **default**: `60s`
* **context**: `http`, `server`, `location`

Sets the timeout in milliseconds for sending data to the fallback upstream (local/mapped modes only).

#### vod_fallback_read_timeout
* **syntax**: `vod_fallback_read_timeout timeout`
* **default**: `60s`
* **context**: `http`, `server`, `location`

Sets the timeout in milliseconds for reading data from the fallback upstream (local/mapped modes only).

#### vod_proxy_header_name
* **syntax**: `vod_proxy_header_name name`
* **default**: `X-Kaltura-Proxy`
* **context**: `http`, `server`, `location`

Sets the name of an HTTP header that is used to prevent fallback proxy loops (local/mapped modes only).

#### vod_proxy_header_value
* **syntax**: `vod_proxy_header_value name`
* **default**: `dumpApiRequest`
* **context**: `http`, `server`, `location`

Sets the value of an HTTP header that is used to prevent fallback proxy loops (local/mapped modes only).

#### vod_clip_to_param_name
* **syntax**: `vod_clip_to_param_name name`
* **default**: `clipTo`
* **context**: `http`, `server`, `location`

The name of the clip to request parameter.

#### vod_clip_from_param_name
* **syntax**: `vod_clip_from_param_name name`
* **default**: `clipFrom`
* **context**: `http`, `server`, `location`

The name of the clip from request parameter.

#### vod_tracks_param_name
* **syntax**: `vod_tracks_param_name name`
* **default**: `tracks`
* **context**: `http`, `server`, `location`

The name of the clip from request parameter.

### Configuration directives - DASH

#### vod_dash_absolute_manifest_urls
* **syntax**: `vod_dash_absolute_manifest_urls on/off`
* **default**: `on`
* **context**: `http`, `server`, `location`

When enabled the server returns absolute URLs in MPD requests

#### vod_dash_manifest_file_name_prefix
* **syntax**: `vod_dash_manifest_file_name_prefix name`
* **default**: `manifest`
* **context**: `http`, `server`, `location`

The name of the MPD file (an mpd extension is implied).

#### vod_dash_init_file_name_prefix
* **syntax**: `vod_dash_init_file_name_prefix name`
* **default**: `init`
* **context**: `http`, `server`, `location`

The name of the MP4 initialization file (an mp4 extension is implied).

#### vod_dash_fragment_file_name_prefix
* **syntax**: `vod_dash_fragment_file_name_prefix name`
* **default**: `frag`
* **context**: `http`, `server`, `location`

The name of the fragment files (an m4s extension is implied).

### Configuration directives - HDS

#### vod_hds_manifest_file_name_prefix
* **syntax**: `vod_hds_manifest_file_name_prefix name`
* **default**: `manifest`
* **context**: `http`, `server`, `location`

The name of the HDS manifest file (an f4m extension is implied).

#### vod_hds_fragment_file_name_prefix
* **syntax**: `vod_hds_fragment_file_name_prefix name`
* **default**: `frag`
* **context**: `http`, `server`, `location`

The prefix of fragment file names, the actual file name is `frag-f<file-index>-v<video-track-index>-a<audio-track-index>-Seg1-Frag<index>`.

### Configuration directives - HLS

#### vod_hls_absolute_master_urls
* **syntax**: `vod_hls_absolute_master_urls on/off`
* **default**: `on`
* **context**: `http`, `server`, `location`

When enabled the server returns absolute playlist URLs in master playlist requests

#### vod_hls_absolute_index_urls
* **syntax**: `vod_hls_absolute_index_urls on/off`
* **default**: `on`
* **context**: `http`, `server`, `location`

When enabled the server returns absolute segment URLs in media playlist requests

#### vod_hls_absolute_iframe_urls
* **syntax**: `vod_hls_absolute_iframe_urls on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled the server returns absolute segment URLs in iframe playlist requests

#### vod_hls_master_file_name_prefix
* **syntax**: `vod_hls_master_file_name_prefix name`
* **default**: `master`
* **context**: `http`, `server`, `location`

The name of the HLS master playlist file (an m3u8 extension is implied).

#### vod_hls_index_file_name_prefix
* **syntax**: `vod_hls_index_file_name_prefix name`
* **default**: `index`
* **context**: `http`, `server`, `location`

The name of the HLS media playlist file (an m3u8 extension is implied).

#### vod_hls_iframes_file_name_prefix
* **syntax**: `vod_hls_iframes_file_name_prefix name`
* **default**: `iframes`
* **context**: `http`, `server`, `location`

The name of the HLS I-frames playlist file (an m3u8 extension is implied).

#### vod_hls_segment_file_name_prefix
* **syntax**: `vod_hls_segment_file_name_prefix name`
* **default**: `seg`
* **context**: `http`, `server`, `location`

The prefix of segment file names, the actual file name is `seg-<index>-v<video-track-index>-a<audio-track-index>.ts`.

#### vod_hls_encryption_key_file_name
* **syntax**: `vod_hls_encryption_key_file_name name`
* **default**: `encryption.key`
* **context**: `http`, `server`, `location`

The name of the encryption key file name (only relevant when vod_secret_key is used).

### Configuration directives - MSS

#### vod_mss_manifest_file_name_prefix
* **syntax**: `vod_mss_manifest_file_name_prefix name`
* **default**: `manifest`
* **context**: `http`, `server`, `location`

The name of the manifest file (has no extension).

### Sample configurations

#### Local configuration

	http {
		upstream fallback {
			server kalhls-a-pa.origin.kaltura.com:80;
		}

		server {		
			open_file_cache          max=1000 inactive=5m;
			open_file_cache_valid    2m;
			open_file_cache_min_uses 1;
			open_file_cache_errors   on;

			aio on;

			location /content/ {
				vod;
				vod_mode local;
				vod_moov_cache moov_cache 512m;
				vod_fallback_upstream fallback;

				root /web/content;
				
				gzip on;
				gzip_types application/vnd.apple.mpegurl;

				expires 100d;
				add_header Last-Modified "Sun, 19 Nov 2000 08:52:00 GMT";
			}
		}
	}


#### Mapped configuration

	http {
		upstream kalapi {
			server www.kaltura.com:80;
		}

		upstream fallback {
			server kalhls-a-pa.origin.kaltura.com:80;
		}

		server {

			open_file_cache          max=1000 inactive=5m;
			open_file_cache_valid    2m;
			open_file_cache_min_uses 1;
			open_file_cache_errors   on;

			aio on;
			
			location /__child_request__/ {
				internal;
				vod_child_request;
			}
		
			location ~ ^/p/\d+/(sp/\d+/)?serveFlavor/ {
				vod;
				vod_mode mapped;
				vod_moov_cache moov_cache 512m;
				vod_secret_key mukkaukk;
				vod_child_request_path /__child_request__/;
				vod_upstream kalapi;
				vod_upstream_host_header www.kaltura.com;
				vod_upstream_extra_args "pathOnly=1";
				vod_path_mapping_cache mapping_cache 5m;
				vod_fallback_upstream fallback;

				gzip on;
				gzip_types application/vnd.apple.mpegurl;

				expires 100d;
				add_header Last-Modified "Sun, 19 Nov 2000 08:52:00 GMT";
			}
		}
	}

#### Remote configuration

	http {
		upstream kalapi {
			server www.kaltura.com:80;
		}

		server {		
			location /__child_request__/ {
				internal;
				vod_child_request;
			}

			location ~ ^/p/\d+/(sp/\d+/)?serveFlavor/ {
				vod;
				vod_mode remote;
				vod_moov_cache moov_cache 512m;
				vod_secret_key mukkaukk;
				vod_child_request_path /__child_request__/;
				vod_upstream kalapi;
				vod_upstream_host_header www.kaltura.com;

				gzip on;
				gzip_types application/vnd.apple.mpegurl;

				expires 100d;
				add_header Last-Modified "Sun, 19 Nov 2000 08:52:00 GMT";
			}
		}
	}
