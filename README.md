# NGINX-based VOD Packager
## nginx-vod-module

### Features

* On-the-fly repackaging of MP4 files to HLS

* Working modes:
  1. Local - serve locally accessible files (local disk/NFS mounted)
  2. Remote - serve files accessible via HTTP using range requests
  3. Mapped - perform an HTTP request to map the input URI to a locally accessible file

* Fallback support for file not found in local/mapped modes
  
* H264/AAC support

* Audio only/video only files

* Track selection for multi audio/video MP4 files

* Generation of I-frames playlist (EXT-X-I-FRAMES-ONLY)

* Source file clipping (only from I-Frame to P-frame)

* AES-128 encryption support

### Limitations

* MP4 files have to be "fast start" (the moov atom must be before the mdat atom)

* I-frames playlist generation is not supported when encryption is enabled

* Tested on Linux only

### Build

cd to NGINX source directory & run this:

    ./configure --add-module=/path/to/nginx-vod-module
    make
    make install
	
For asynchronous I/O support add `--with-file-aio` (highly recommended, local and mapped modes only)

    ./configure --add-module=/path/to/nginx-vod-module --with-file-aio
	
For building debug version of nginx add `--with-debug`

    ./configure --add-module=/path/to/nginx-vod-module --with-debug

### Configuration directives

#### vod
* **syntax**: `vod`
* **default**: `n/a`
* **context**: `location`

Enables the nginx-vod module on the enclosing location.

#### vod_mode
* **syntax**: `vod_mode mode`
* **default**: `local`
* **context**: `http`, `server`, `location`

Sets the file access mode - local, remote or mapped

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

#### vod_initial_read_size
* **syntax**: `vod_initial_read_size size`
* **default**: `4K`
* **context**: `http`, `server`, `location`

Sets the size of the initial read operation of the MP4 file.

#### vod_max_moov_size
* **syntax**: `vod_max_moov_size size`
* **default**: `1MB`
* **context**: `http`, `server`, `location`

Sets the maximum supported MP4 moov atom size.

#### vod_cache_buffer_size
* **syntax**: `vod_cache_buffer_size size`
* **default**: `64K`
* **context**: `http`, `server`, `location`

Sets the size of the cache buffers used when reading MP4 frames.

#### vod_upstream
* **syntax**: `vod_upstream upstream_name`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the upstream that should be used for reading the MP4 file (remote mode) or mapping the request URI (mapped mode).

#### vod_upstream_host_header
* **syntax**: `vod_upstream_host_header host_name`
* **default**: `none`
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

#### vod_path_response_prefix
* **syntax**: `vod_path_response_prefix prefix`
* **default**: `<?xml version=\"1.0\" encoding=\"utf-8\"?><xml><result>`
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

#### vod_index_file_name_prefix
* **syntax**: `vod_index_file_name_prefix name`
* **default**: `index`
* **context**: `http`, `server`, `location`

The name of the HLS playlist file (an m3u8 extension is implied).

#### vod_iframes_file_name_prefix
* **syntax**: `vod_iframes_file_name_prefix name`
* **default**: `iframes`
* **context**: `http`, `server`, `location`

The name of the HLS I-frames playlist file (an m3u8 extension is implied).

#### vod_segment_file_name_prefix
* **syntax**: `vod_segment_file_name_prefix name`
* **default**: `seg-`
* **context**: `http`, `server`, `location`

The prefix of segment file names, the actual file name is `seg-<index>-v<video-track-index>-a<audio-track-index>.ts`.

#### vod_encryption_key_file_name
* **syntax**: `vod_encryption_key_file_name name`
* **default**: `encryption.key`
* **context**: `http`, `server`, `location`

The name of the encryption key file name (only relevant when vod_secret_key is used).

### Sample configurations

#### Local configuration

	http {
		upstream fallback {
			server kalhls-a-pa.origin.kaltura.com:80;
		}

		server {		
			location /content/ {
				vod;
				vod_mode local;
				vod_fallback_upstream fallback;

				root /web/content;
				
				gzip on;
				gzip_types application/vnd.apple.mpegurl;

				expires 100d;
				add_header Last-Modified "Sun, 19 Nov 2000 08:52:00 GMT";

				aio on;
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
			location ~ ^/p/\d+/(sp/\d+/)?serveFlavor/ {
				vod;
				vod_mode mapped;
				vod_secret_key mukkaukk;
				vod_upstream kalapi;
				vod_upstream_host_header www.kaltura.com;
				vod_upstream_extra_args "pathOnly=1";
				vod_fallback_upstream fallback;

				gzip on;
				gzip_types application/vnd.apple.mpegurl;

				expires 100d;
				add_header Last-Modified "Sun, 19 Nov 2000 08:52:00 GMT";

				aio on;
			}
		}
	}

#### Remote configuration

	http {
		upstream kalapi {
			server www.kaltura.com:80;
		}

		server {		
			location ~ ^/p/\d+/(sp/\d+/)?serveFlavor/ {
				vod;
				vod_mode remote;
				vod_secret_key mukkaukk;
				vod_upstream kalapi;
				vod_upstream_host_header www.kaltura.com;

				gzip on;
				gzip_types application/vnd.apple.mpegurl;

				expires 100d;
				add_header Last-Modified "Sun, 19 Nov 2000 08:52:00 GMT";
			}
		}
	}
