# NGINX-based VOD Packager
## nginx-vod-module [![Build Status](https://travis-ci.org/kaltura/nginx-vod-module.svg?branch=master)](https://travis-ci.org/kaltura/nginx-vod-module)

[Join the list of organizations using this video packager project](https://github.com/kaltura/nginx-vod-module/issues/730/).

For live video streaming, please use [Media-Framework](https://github.com/kaltura/media-framework/).

### Features

* On-the-fly repackaging of MP4 files to DASH, HDS, HLS, MSS

* Working modes:
  1. Local - serve locally accessible files (local disk/NFS mounted)
  2. Remote - serve files accessible via HTTP using range requests
  3. Mapped - serve files according to a specification encoded in JSON format. The JSON can pulled from a remote server, or read from a local file

* Adaptive bitrate support

* Playlist support (playing several different media files one after the other) - mapped mode only

* Simulated live support (generating a live stream from MP4 files) - mapped mode only

* Fallback support for file not found in local/mapped modes (useful in multi-datacenter environments)
  
* Video codecs: H264, H265 (DASH/HLS), AV1 (DASH/HLS), VP8 (DASH), VP9 (DASH)

* Audio codecs: AAC, MP3 (HLS/HDS/MSS), AC-3 (DASH/HLS), E-AC-3 (DASH/HLS), VORBIS (DASH), OPUS (DASH), FLAC (HLS), DTS (HLS)

* Captions support - 
  
  Input:
  1. WebVTT
  2. SRT
  3. DFXP/TTML
  4. CAP (Cheetah)
  
  Output:
  1. DASH - either a single WebVTT or SMPTE-TT segments (configurable)
  2. HLS - segmented WebVTT (m3u8)
  3. MSS - converted to TTML and packaged in fragmented MP4 (no support for styling)

* Audio only/video only files

* Alternative audio renditions - supporting both:
  1. Generation of manifest with different audio renditions, allowing selection on the client side
  2. Muxing together audio and video streams from separate files / tracks - provides the ability
	to serve different audio renditions of a single video, without the need for any special support
	on the client side.

* Track selection for multi audio/video MP4 files

* Playback rate change - 0.5x up to 2x (requires libavcodec and libavfilter)

* Source file clipping (only from I-Frame to P-frame)

* Support for variable segment lengths - enabling the player to select the optimal bitrate fast,
without the overhead of short segments for the whole duration of the video

* Clipping of MP4 files for progressive download playback

* Thumbnail capture (requires libavcodec) and resize (requires libswscale)

* Volume map (requires libavcodec) - returns a CSV containing the volume level in each interval

* Decryption of CENC-encrypted MP4 files (it is possible to create such files with MP4Box)

* DASH: common encryption (CENC) support

* MSS: PlayReady encryption support

* HLS: Generation of I-frames playlist (EXT-X-I-FRAMES-ONLY)

* HLS: support for AES-128 / SAMPLE-AES encryption

### Limitations

* Track selection and playback rate change are not supported in progressive download

* I-frames playlist generation is not supported when encryption is enabled

* Tested on Linux only

### Compilation

#### Dependencies

In general, if you have the dependencies that are required to build nginx, you should be able to build nginx-vod-module.
However, some optional features of this module depend on additional packages. The module detects these packages 
during `configure` - if a package is missing, the respective feature will be disabled.

The optional features are:
1. Thumbnail capture & volume map - depend on ffmpeg (3.0 or newer)
2. Audio filtering (for changing playback rate / gain) - depends on ffmpeg (3.0 or newer) and also on libfdk_aac.
	Due to licensing issues, libfdk_aac is not built into kaltura ffmpeg packages
3. Encryption / decryption (DRM / HLS AES) - depends on openssl
4. DFXP captions - depends on libxml2
5. UTF-16 encoded SRT files - depends on iconv

#### Build

To link statically against nginx, cd to nginx source directory and execute:

    ./configure --add-module=/path/to/nginx-vod-module
    make
    make install

To compile as a dynamic module (nginx 1.9.11+), use:
  
	./configure --add-dynamic-module=/path/to/nginx-vod-module

In this case, the `load_module` directive should be used in nginx.conf in order to load the module.

Optional recommended settings:
1. `--with-file-aio` - enable asynchronous I/O support, highly recommended, relevant only to local and mapped modes
2. `--with-threads` (nginx 1.7.11+) - enable asynchronous file open using thread pool (also requires `vod_open_file_thread_pool` in nginx.conf), relevant only to local and mapped modes
3. `--with-cc-opt="-O3 -mpopcnt"` - enable additional compiler optimizations (we saw about 8% reduction in the mp4 parse time
	and frame processing time compared to the nginx default `-O`)

Debug settings:
1. `--with-debug` - enable debug messages (also requires passing `debug` in the `error_log` directive in nginx.conf).
2. `--with-cc-opt="-O0"` - disable compiler optimizations (for debugging with gdb)

C Macro Configurations:
1. `--with-cc-opt="-DNGX_VOD_MAX_TRACK_COUNT=256 -mavx2"` - increase the maximum track count (preferably to multiples of 64). It's recommended to enable vector extensions (AVX2) as well.

### Installation

#### RHEL/CentOS 6/7 RPM
```sh
# rpm -ihv http://installrepo.kaltura.org/releases/kaltura-release.noarch.rpm
# yum install kaltura-nginx
```

#### Debian/Ubuntu deb package
*Ubuntu NOTE: before trying to install kaltura-nginx, you must also make sure the multiverse repo is enabled*

For Debian Wheezy [7], Debian Jessie [8], Ubuntu 14.04 and 14.10, add this repo:
```sh
# wget -O - http://installrepo.kaltura.org/repo/apt/debian/kaltura-deb-curr.gpg.key|apt-key add -
# echo "deb [arch=amd64] http://installrepo.kaltura.org/repo/apt/debian propus main" > /etc/apt/sources.list.d/kaltura.list
```

For Ubuntu 16.04, 16.10 add this repo:
```sh
# wget -O - http://installrepo.kaltura.org/repo/apt/xenial/kaltura-deb-curr-256.gpg.key|apt-key add -
# echo "deb [arch=amd64] http://installrepo.kaltura.org/repo/apt/xenial propus main" > /etc/apt/sources.list.d/kaltura.list
```

For Ubuntu 20.04 add this repo:
```sh
# wget -O - http://installrepo.kaltura.org/repo/aptn/focal/kaltura-deb-256.gpg.key|apt-key add -
# echo "deb [arch=amd64] http://installrepo.kaltura.org/repo/aptn/focal quasar main" > /etc/apt/sources.list.d/kaltura.list
```


Then install the kaltura-nginx package:
```sh
# apt-get update
# apt-get install kaltura-nginx
```


If you wish to make use of the following features:
- Thumbnail capture
- Playback rate change - 0.5x up to 2x

You will also need to install the kaltura-ffmpeg (>= 3.1) package.

### URL structure

#### Basic URL structure

The basic structure of an nginx-vod-module URL is:
`http://<domain>/<location>/<fileuri>/<filename>`

Where:
* domain - the domain of the nginx-vod-module server
* location - the location specified in the nginx conf
* fileuri - a URI to the mp4 file:
  * local mode - the full file path is determined according to the root / alias nginx.conf directives
  * mapped mode - the full file path is determined according to the JSON received from the upstream / local file
  * remote mode - the mp4 file is read from upstream in chunks
  * Note: in mapped & remote modes, the URL of the upstream request is `http://<upstream>/<location>/<fileuri>?<extraargs>`
  (extraargs is determined by the `vod_upstream_extra_args` parameter)
* filename - detailed below

#### Multi URL structure

Multi URLs are used to encode several URLs on a single URL. A multi URL can be used to specify
the URLs of several different MP4 files that should be included together in a DASH MPD for example.

The structure of a multi URL is:
`http://<domain>/<location>/<prefix>,<middle1>,<middle2>,<middle3>,<postfix>.urlset/<filename>`

The sample URL above represents 3 URLs:
* `http://<domain>/<location>/<prefix><middle1><postfix>/<filename>`
* `http://<domain>/<location>/<prefix><middle2><postfix>/<filename>`
* `http://<domain>/<location>/<prefix><middle3><postfix>/<filename>`

The suffix `.urlset` (can be changed using `vod_multi_uri_suffix`) indicates that the URL should be treated as a multi URL.
For example - the URL `http://example.com/hls/videos/big_buck_bunny_,6,9,15,00k.mp4.urlset/master.m3u8` will return a manifest containing:
* http://example.com/hls/videos/big_buck_bunny_600k.mp4/index.m3u8
* http://example.com/hls/videos/big_buck_bunny_900k.mp4/index.m3u8
* http://example.com/hls/videos/big_buck_bunny_1500k.mp4/index.m3u8

#### URL path parameters

The following parameters are supported on the URL path:
* clipFrom - an offset in milliseconds since the beginning of the video, where the generated stream should start. 
	For example, `.../clipFrom/10000/...` will generate a stream that starts 10 seconds into the video.
* clipTo - an offset in milliseconds since the beginning of the video, where the generated stream should end.
	For example, `.../clipTo/60000/...` will generate a stream truncated to 60 seconds.
* tracks - can be used to select specific audio/video tracks. The structure of the parameter is: `v<id1>-v<id2>-a<id1>-a<id2>...`
	For example, `.../tracks/v1-a1/...` will select the first video track and first audio track.
	The default is to include all tracks.
* shift - can be used to apply a timing shift to one or more streams. The structure of the parameter is: `v<vshift>-a<ashift>-s<sshift>`
	For example, `.../shift/v100/...` will apply a forward shift of 100ms to the video timestamps.

#### Filename structure

The structure of filename is:
`<basename>[<seqparams>][<fileparams>][<trackparams>][<langparams>].<extension>`

Where:
* basename + extension - the set of options is packager specific (the list below applies to the default settings):
  * dash - manifest.mpd
  * hds - manifest.f4m
  * hls master playlist - master.m3u8
  * hls media playlist - index.m3u8
  * mss - manifest
  * thumb - `thumb-<offset>[<resizeparams>].jpg` (offset is the thumbnail video offset in milliseconds)
  * volume_map - `volume_map.csv`
* seqparams - can be used to select specific sequences by id (provided in the mapping JSON), e.g. master-sseq1.m3u8.
* fileparams - can be used to select specific sequences by index when using multi URLs.
	For example, manifest-f1.mpd will return an MPD only from the first URL.
* trackparams - can be used to select specific audio/video tracks.
	For example, manifest-a1.f4m will return an F4M containing only the first audio stream of each sequence.
	The default is to include the first audio and first video tracks of each file.
	The tracks selected on the file name are AND-ed with the tracks selected with the /tracks/ path parameter.
	v0/a0 select all video/audio tracks respectively.
	The a/v parameters can be combined with f/s, e.g. f1-v1-f2-a1 = video1 of file1 + audio1 of file2, f1-f2-v1 = video1 of file1 + video1 of file2.
* langparams - can be used to filter audio tracks/subtitles according to their language (ISO639-3 code).
	For example, master-leng.m3u8 will return only english audio tracks.
* resizeparams - can be used to resize the returned thumbnail image. For example, thumb-1000-w150-h100.jpg captures a thumbnail
	1 second into the video, and resizes it to 150x100. If one of the dimensions is omitted, its value is set so that the 
	resulting image will retain the aspect ratio of the video frame.

### Mapping response format

When configured to run in mapped mode, nginx-vod-module issues an HTTP request to a configured upstream server 
in order to receive the layout of media streams it should generate.
The response has to be in JSON format. 

This section contains a few simple examples followed by a reference of the supported objects and fields. 
But first, a couple of definitions:

1. `Source Clip` - a set of audio and/or video frames (tracks) extracted from a single media file
2. `Generator` - a component that can generate audio/video frames. Currently, the only supported generator is the silence generator.
3. `Filter` - a manipulation that can be applied on audio/video frames. The following filters are supported: 
  * rate (speed) change - applies to both audio and video
  * audio volume change
  * mix - can be used to merge several audio tracks together, or to merge the audio of source A with the video of source B
4. `Clip` - the result of applying zero or more filters on a set of source clips
5. `Dynamic Clip` - a clip whose contents is not known in advance, e.g. targeted ad content
6. `Sequence` - a set of clips that should be played one after the other. 
7. `Set` - several sequences that play together as an adaptive set, each sequence must have the same number of clips.

#### Simple mapping

The JSON below maps the request URI to a single MP4 file:
```json
{
	"sequences": [
		{
			"clips": [
				{
					"type": "source",
					"path": "/path/to/video.mp4"
				}
			]
		}
	]
}
```

When using multi URLs, this is the only allowed JSON pattern. In other words, it is not
possible to combine more complex JSONs using multi URL.

#### Adaptive set

As an alternative to using multi URL, an adaptive set can be defined via JSON:
```json
{
	"sequences": [
		{
			"clips": [
				{
					"type": "source",
					"path": "/path/to/bitrate1.mp4"
				}
			]
		},
		{
			"clips": [
				{
					"type": "source",
					"path": "/path/to/bitrate2.mp4"
				}
			]
		}
	]
}
```

#### Playlist

The JSON below will play 35 seconds of video1 followed by 22 seconds of video2:
```json
{
	"durations": [ 35000, 22000 ],
	"sequences": [
		{
			"clips": [
				{
					"type": "source",
					"path": "/path/to/video1.mp4"
				},
				{
					"type": "source",
					"path": "/path/to/video2.mp4"
				}
			]
		}
	]
}
```

#### Filters

The JSON below takes video1, plays it at x1.5 and mixes the audio of the result with the audio of video2,
after reducing it to 50% volume:
```json
{
	"sequences": [
		{
			"clips": [
				{
					"type": "mixFilter",
					"sources": [
						{
							"type": "rateFilter",
							"rate": 1.5,
							"source": {
								"type": "source",
								"path": "/path/to/video1.mp4"
							}
						},
						{
							"type": "gainFilter",
							"gain": 0.5,
							"source": {
								"type": "source",
								"path": "/path/to/video2.mp4",
								"tracks": "a1"
							}
						}
					]
				}
			]
		}
	]
}
```

#### Continuous live

The JSON below is a sample of a continuous live stream (=a live stream in which all videos have exactly the same encoding parameters).
In practice, this JSON will have to be generated by some script, since it is time dependent.
(see test/playlist.php for a sample implementation)
```json
{
	"playlistType": "live",
	"discontinuity": false,
	"segmentBaseTime": 1451904060000,
	"firstClipTime": 1451917506000,
	"durations": [83000, 83000],
	"sequences": [
		{
			"clips": [
				{
					"type": "source",
					"path": "/path/to/video1.mp4"
				},
				{
					"type": "source",
					"path": "/path/to/video2.mp4"
				}
			]
		}
	]
}
```

#### Non-continuous live

The JSON below is a sample of a non-continuous live stream (=a live stream in which the videos have different encoding parameters).
In practice, this JSON will have to be generated by some script, since it is time dependent 
(see test/playlist.php for a sample implementation)
```json
{
	"playlistType": "live",
	"discontinuity": true,
	"initialClipIndex": 171,
	"initialSegmentIndex": 153,
	"firstClipTime": 1451918170000,
	"durations": [83000, 83000],
	"sequences": [
		{
			"clips": [
				{
					"type": "source",
					"path": "/path/to/video1.mp4"
				},
				{
					"type": "source",
					"path": "/path/to/video2.mp4"
				}
			]
		}
	]
}
```

### Mapping reference

#### Set (top level object in the mapping JSON)

Mandatory fields:
* `sequences` - array of Sequence objects. 
	The mapping has to contain at least one sequence and up to 32 sequences.
	
Optional fields:
* `id` - a string that identifies the set. The id can be retrieved by `$vod_set_id`.
* `playlistType` - string, can be set to `live`, `vod` or `event` (only supported for HLS playlists), default is `vod`.
* `durations` - an array of integers representing clip durations in milliseconds.
	This field is mandatory if the mapping contains more than a single clip per sequence.
	If specified, this array must contain at least one element and up to 128 elements.
* `discontinuity` - boolean, indicates whether the different clips in each sequence have
	different media parameters. This field has different manifestations according to the 
	delivery protocol - a value of true will generate `#EXT-X-DISCONTINUITY` in HLS, 
	and a multi period MPD in DASH. The default value is true, set to false only if the media
	files were transcoded with exactly the same parameters (in AVC for example, 
	the clips should have exactly the same SPS/PPS).
* `segmentDuration` - integer, sets the segment duration in milliseconds. This field, 
	if specified, takes priority over the value set in `vod_segment_duration`.
* `consistentSequenceMediaInfo` - boolean, currently affects only DASH. When set to true (default)
	the MPD will report the same media parameters in each period element. Setting to false
	can have severe performance implications for long sequences (nginx-vod-module has 
	to read the media info of all clips included in the mapping in order to generate the MPD)
* `referenceClipIndex` - integer, sets the (1-based) index of the clip that should be used 
	to retrieve the video metadata for manifest requests (codec, width, height etc.)
	If `consistentSequenceMediaInfo` is set to false, this parameter has no effect -
	all clips are parsed. If this parameter is not specified, nginx-vod-module uses the last clip 
	by default.
* `notifications` - array of notification objects (see below), when a segment is requested,
	all the notifications that fall between the start/end times of the segment are fired.
	the notifications must be ordered in an increasing offset order.
* `clipFrom` - integer, contains a timestamp indicating where the returned stream should start.
	Setting this parameter is equivalent to passing /clipFrom/ on the URL.
* `clipTo` - integer, contains a timestamp indicating where the returned stream should end.
	Setting this parameter is equivalent to passing /clipTo/ on the URL.
* `cache` - boolean, if set to false, the mapping response will not be saved to cache (vod_mapping_cache).
	The default value is true.
* `closedCaptions` - array of closed captions objects (see below), containing languages and ids
	of any embedded CEA-608 / CEA-708 captions. If an empty array is provided, the module will output
	`CLOSED-CAPTIONS=NONE` on each `EXT-X-STREAM-INF` tag. If the list does not appear in the JSON, the 
	module will not output any `CLOSED-CAPTIONS` fields in the playlist.
	
Live fields:
* `firstClipTime` - integer, mandatory for all live playlists unless `clipTimes` is specified.
	Contains the absolute time of the first clip in the playlist, in milliseconds since the epoch (unixtime x 1000)
* `clipTimes` - array of integers, sets the absolute time of all the clips in the playlist, 
	in milliseconds since the epoch (unixtime x 1000). This field can be used only when 
	`discontinuity` is set to true. The timestamps may contain gaps, but they are not allowed to overlap
	(`clipTimes[n + 1] >= clipTimes[n] + durations[n]`)
* `segmentBaseTime` - integer, mandatory for continuous live streams, contains the absolute
	time of the first segment of the stream, in milliseconds since the epoch (unixtime x 1000).
	This value must not change during playback.
	For discontinuous live streams, this field is optional:
	* if not set, sequential segment indexes will be used throughout the playlist.
		In this case, the upstream server generating the mapping json has to maintain state,
		and update initialSegmentIndex every time a clip is removed from the playlist.
	* if set, the timing gaps between clips must not be lower than `vod_segment_duration`.
* `firstClipStartOffset` - integer, optional, measured in milliseconds. This field contains the
	difference between first clip time, and the original start time of the first clip -
	the time it had when it was initially added (before the live window shifted)
* `initialClipIndex` - integer, mandatory for non-continuous live streams that mix videos having
	different encoding parameters (SPS/PPS), contains the index of the first clip in the playlist. 
	Whenever a clip is pushed out of the head of the playlist, this value must be incremented by one.
* `initialSegmentIndex` - integer, mandatory for live streams that do not set `segmentBaseTime`, 
	contains the index of the first segment in the playlist. Whenever a clip is pushed out of the head of
	the playlist, this value must be incremented by the number of segments in the clip.
* `presentationEndTime` - integer, optional, measured in milliseconds since the epoch.
	when supplied, the module will compare the current time to the supplied value, 
	and signal the end of the live presentation if `presentationEndTime` has passed. 
	In HLS, for example, this parameter controls whether an `#EXT-X-ENDLIST` tag should be 
	included in the media playlist.
	When the parameter is not supplied, the module will not signal live presentation end.
* `expirationTime` - integer, optional, measured in milliseconds since the epoch.
	when supplied, the module will compare the current time to the supplied value, 
	and if `expirationTime` has passed, the module will return a 404 error for manifest requests 
	(segment requests will continue to be served).
	when both presentationEndTime and expirationTime have passed, presentationEndTime takes
	priority, i.e. manifest requests will be served and signal presentation end.
* `liveWindowDuration` - integer, optional, provides a way to override `vod_live_window_duration`
	specified in the configuration. If the value exceeds the absolute value specified in 
	`vod_live_window_duration`, it is ignored.
* `timeOffset` - integer, sets an offset that should be applied to the server clock when serving
	live requests. This parameter can be used to test future/past events.
	
#### Sequence

Mandatory fields:
* `clips` - array of Clip objects (mandatory). The number of elements must match the number
	the durations array specified on the set. If the durations array is not specified,
	the clips array must contain a single element.
	
Optional fields:
* `id` - a string that identifies the sequence. The id can be retrieved by `$vod_sequence_id`.
* `language` - a 3-letter (ISO-639-2) language code, this field takes priority over any language
	specified on the media file (MP4 mdhd atom)
* `label` - a friendly string that identifies the sequence. If a language is specified,
	a default label will be automatically derived by it - e.g. if language is `ita`, 
	by default `italiano` will be used as the label.
* `roles` - an array of DASH role schemes as defined in ISO/IEC 23009-1 Section 5.8.5.5. For roles
	such as `caption` or `description`, the label must be explicitly specified. The default label
	does not take roles into account.
* `default` - a boolean that sets the value of the DEFAULT attribute of EXT-X-MEDIA tags using this sequence.
	If not specified, the first EXT-X-MEDIA tag in each group returns DEFAULT=YES, while the others return DEFAULT=NO.
* `bitrate` - an object that can be used to set the bitrate for the different media types,
	in bits per second. For example, `{"v": 900000, "a": 64000}`. If the bitrate is not supplied,
	nginx-vod-module will estimate it based on the last clip in the sequence.
* `avg_bitrate` - an object that can be used to set the average bitrate for the different media types,
	in bits per second. See `bitrate` above for a sample object. If specified, the module will use
	the value to populate the AVERAGE-BANDWIDTH attribute of `#EXT-X-STREAM-INF` in HLS.

#### Clip (abstract)

Mandatory fields:
* `type` - a string that defines the type of the clip. Allowed values are:
	* source
	* rateFilter
	* mixFilter
	* gainFilter
	* silence
	* concat
	* dynamic

Optional fields:
* `keyFrameDurations` - array of integers, containing the durations in milliseconds of the video key frames
	in the clip. This property can only be supplied on the top level clips of each sequence,
	supplying this property on nested clips has no effect.
	Supplying the key frame durations enables the module to both:
	1. align the segments to key frames 
	2. report the correct segment durations in the manifest - providing an alternative to setting
		`vod_manifest_segment_durations_mode` to `accurate`, which is not supported for multi clip
		media sets (for performance reasons).
* `firstKeyFrameOffset` - integer, offset of the first video key frame in the clip, 
	measured in milliseconds relative to `firstClipTime`. Defaults to 0 if not supplied.

#### Source clip

Mandatory fields:
* `type` - a string with the value `source`
* `path` - a string containing the path of the MP4 file. The string `"empty"` can be used to represent
	an empty captions file (useful in case only some videos in a playlist have captions)

Optional fields:
* `id` - a string that identifies the source clip
* `sourceType` - sets the interface that should be used to read the MP4 file, allowed values are:
	`file` and `http`. By default, the module uses `http` if `vod_remote_upstream_location` is set,
	and `file` otherwise.
* `tracks` - a string that specifies the tracks that should be used, the default is "v1-a1",
	which means the first video track and the first audio track
* `clipFrom` - an integer that specifies an offset in milliseconds, from the beginning of the 
	media file, from which to start loading frames
* `encryptionKey` - a base64 encoded string containing the key (128/192/256 bit) that should be used
	to decrypt the file.
* `encryptionIv` - a base64 encoded string containing the iv (128 bit) that should be used
	to decrypt the file.
* `encryptionScheme` - the encryption scheme that was used to encrypt the file. Currently,
	only two schemes are supported - `cenc` for MP4 files, `aes-cbc` for caption files.

#### Rate filter clip

Mandatory fields:
* `type` - a string with the value `rateFilter`
* `rate` - a float that specified the acceleration factor, e.g. a value of 2 means double speed.
	Allowed values are in the range 0.5 - 2 with up to two decimal points
* `source` - a clip object on which to perform the rate filtering

#### Gain filter clip

Mandatory fields:
* `type` - a string with the value `gainFilter`
* `gain` - a float that specified the amplification factor, e.g. a value of 2 means twice as loud.
	The gain must be positive with up to two decimal points
* `source` - a clip object on which to perform the gain filtering

#### Mix filter clip

Mandatory fields:
* `type` - a string with the value `mixFilter`
* `sources` - an array of Clip objects to mix. This array must contain at least one clip and
	up to 32 clips.

#### Concat clip

Mandatory fields:
* `type` - a string with the value `concat`
* `durations` - an array of integers representing MP4 durations in milliseconds,
	this array must match the `paths` array in count and order.

Optional fields:
* `paths` - an array of strings, containing the paths of the MP4 files. Either `paths` or `clipIds` must be specified.
* `clipIds` - an array of strings, containing the ids of source clips. 
	The ids are translated to paths by issuing a request to the uri specified in `vod_source_clip_map_uri`.
	Either `paths` or `clipIds` must be specified.
* `tracks` - a string that specifies the tracks that should be used, the default is "v1-a1",
	which means the first video track and the first audio track
* `offset` - an integer in milliseconds that indicates the timestamp offset of the 
	first frame in the concatenated stream relative to the clip start time
* `basePath` - a string that should be added as a prefix to all the paths
* `notifications` - array of notification objects (see below), when a segment is requested,
	all the notifications that fall between the start/end times of the segment are fired.
	the notifications must be ordered in an increasing offset order.

#### Dynamic clip

Mandatory fields:
* `type` - a string with the value `dynamic`
* `id` - a string that uniquely identifies the dynamic clip, used for mapping the clip to its content

#### Notification

Mandatory fields:
* `offset` - an integer in milliseconds that indicates the time in which the notification should be fired.
	when the notification object is contained in the media set, `offset` is relative to `firstClipTime`
	(0 for vod). when the notification object is contained in a concat clip, `offset` is relative to
	the beginning of the concat clip.
* `id` - a string that identifies the notification, this id can be referenced by `vod_notification_uri`
	using the variable `$vod_notification_id`

#### Closed Captions

Mandatory fields:
* `id` - a string that identifies the embedded captions. This will become the `INSTREAM-ID` field and must
have one of the following values: `CC1`, `CC3`, `CC3`, `CC4`, or `SERVICEn`, where `n` is between 1 and 63.
* `label` - a friendly string that indicates the language of the closed caption track.

Optional fields:
* `language` - a 3-letter (ISO-639-2) language code that indicates the language of the closed caption track.


### Security

#### Authorization

##### CDN-based delivery

Media packaged by nginx-vod-module can be protected using CDN tokens, this works as follows:
* Some application authenticates the user and decides whether the user should be allowed 
	to watch a specific video. If the user is allowed, the application generates a tokenized
	URL for the manifest of the video.
* The CDN validates the token, and if found to be valid, forwards the request to nginx-vod-module 
	on the origin. 
* The nginx server builds the manifest response and generates tokens for the segment URLs
	contained inside it. The module https://github.com/kaltura/nginx-secure-token-module can
	be used to accomplish this task, it currently support Akamai tokens and CloudFront tokens.
	See the readme of this module for more details.
* The CDN validates the token on each segment that is requested.

In this setup it also highly recommended to block direct access to the origin server by
authenticating the CDN requests. Without this protection, a user who somehow gets the address
of the origin will be able to bypass the CDN token enforcement. If using Akamai, this can
be accomplished using https://github.com/refractalize/nginx_mod_akamai_g2o.
For other CDNs, it may be possible to configure the CDN to send a secret header to the origin
and then simply enforce the header using an nginx if statement:
```c
		if ($http_x_secret_origin_header != "secret value") {
			return 403;
		}
```

In addition to the above, most CDNs support other access control settings, such as geo-location.
These restrictions are completely transparent to the origin and should work well. 

##### Direct delivery

Deployments in which the media is pulled directly from nginx-vod-module can protect the media
using nginx access control directives, such `allow`, `deny`, or `access_by_lua` (for more complex
scenarios).

In addition, it is possible to build a token based solution (as detailed in the previous section) 
without a CDN, by having the nginx server validate the token. 
The module https://github.com/kaltura/nginx-akamai-token-validate-module can be used
to validate Akamai tokens. Locations on which the module is enabled will return 403 unless the 
request contains a valid Akamai token. See the readme of this module for more details.

#### URL encryption

As an alternative to tokenization, URL encryption can be used to prevent an attacker from being
able to craft a playable URL. URL encryption can be implemented with 
https://github.com/kaltura/nginx-secure-token-module, and is supported for HLS and DASH (with 
manifest format set to segmentlist). 

In terms of security, the main advantage of CDN tokens over URL encryption is that CDN tokens
usually expire, while encrypted URLs do not (someone who obtains a playable URL will be able to
use it indefinitely)

#### Media encryption

Nginx-vod-module supports AES-128 and SAMPLE-AES HLS encryption schemes. The main difference between
media encryption and DRM (detailed below) is the mechanism used to transfer the encryption key to 
the client. With media encryption the key is fetched by the client by performing a simple GET request
to nginx-vod-module, while with DRM the key is returned inside a vendor specific license response.

Media encryption reduces the problem of securing the media to the need to secure the encryption key. 
The media segment URLs (which compose the vast majority of the traffic) can be completely unprotected, 
and easily cacheable by any proxies between the client and servers (unlike tokenization). 
The encryption key request can then be protected using one of the methods mentioned above (CDN tokens,
nginx access rules etc.). 

In addition, it is possible to configure nginx-vod-module to return the encryption key over HTTPS
while having the segments delivered over HTTP. The way to configure this is to set `vod_segments_base_url`
to `http://nginx-vod-host` and set `vod_base_url` to `https://nginx-vod-host`.

#### DRM

Nginx-vod-module has the ability to perform on-the-fly encryption for MPEG DASH (CENC), MSS Play Ready and FairPlay HLS.
As in the case of media encryption, the encryption is performed while serving a video/audio segment to the client, 
therefore, when working with DRM it is recommended not to serve the content directly from nginx-vod-module to end-users.
A more scalable architecture would be to use proxy servers or a CDN in order to cache the encrypted segments.

In order to perform the encryption, nginx-vod-module needs several parameters, including key & key_id, these parameters
are fetched from an external server via HTTP GET requests.
The `vod_drm_upstream_location` parameter specifies an nginx location that is used to access the DRM server,
and the request uri is configured using `vod_drm_request_uri` (this parameter can include nginx variables). 
The response of the DRM server is a JSON, with the following format:

```json
[{
	"pssh": [{
			"data": "CAESEGMyZjg2MTczN2NjNGYzODIaB2thbHR1cmEiCjBfbmptaWlwbXAqBVNEX0hE", 
			"uuid": "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed"
		}], 
	"key": "GzoNU9Dfwc//Iq3/zbzMUw==", 
	"key_id": "YzJmODYxNzM3Y2M0ZjM4Mg=="
}]
```

* `pssh.data` - base64 encoded binary data, the format of this data is drm vendor specific
* `pssh.uuid` - the drm system UUID, in this case, edef8ba9-79d6-4ace-a3c8-27dcd51d21ed stands for Widevine
* `key` - base64 encoded encryption key (128 bit)
* `key_id` - base64 encoded key identifier (128 bit)
* `iv` - optional base64 encoded initialization vector (128 bit). The IV is currently used only in HLS (FairPlay), 
	in the other protocols an IV is generated automatically by nginx-vod-module.

##### Sample configurations

Apple FairPlay HLS:
```nginx
location ~ ^/fpshls/p/\d+/(sp/\d+/)?serveFlavor/entryId/([^/]+)/(.*) {
	vod hls;
	vod_hls_encryption_method sample-aes;
	vod_hls_encryption_key_uri "skd://entry-$2";
	vod_hls_encryption_key_format "com.apple.streamingkeydelivery";
	vod_hls_encryption_key_format_versions "1";

	vod_drm_enabled on;
	vod_drm_request_uri "/udrm/system/ovp/$vod_suburi";

	vod_last_modified_types *;
	add_header Access-Control-Allow-Headers '*';
	add_header Access-Control-Expose-Headers 'Server,range,Content-Length,Content-Range';
	add_header Access-Control-Allow-Methods 'GET, HEAD, OPTIONS';
	add_header Access-Control-Allow-Origin '*';
	expires 100d;
}
```

Common Encryption HLS:
```nginx
location ~ ^/cenchls/p/\d+/(sp/\d+/)?serveFlavor/entryId/([^/]+)/(.*) {
	vod hls;
	vod_hls_encryption_method sample-aes-cenc;
	vod_hls_encryption_key_format "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
	vod_hls_encryption_key_format_versions "1";

	vod_drm_enabled on;
	vod_drm_request_uri "/udrm/system/ovp/$vod_suburi";

	vod_last_modified_types *;
	add_header Access-Control-Allow-Headers '*';
	add_header Access-Control-Expose-Headers 'Server,range,Content-Length,Content-Range';
	add_header Access-Control-Allow-Methods 'GET, HEAD, OPTIONS';
	add_header Access-Control-Allow-Origin '*';
	expires 100d;
}
```

##### Verified configurations

Following is a list of configurations that were tested and found working:
* DASH/CENC with PlayReady & Widevine PSSH together
* MSS PlayReady
* HLS FairPlay

### Performance recommendations

1. For medium/large scale deployments, don't have users play the videos directly from nginx-vod-module.
	Since all the different streaming protocols supported by nginx vod are HTTP based, they can be cached by standard HTTP proxies / CDNs. 
	For medium scale add a layer of caching proxies between the vod module and the end users 
	(can use standard nginx servers with proxy_pass & proxy_cache). 
	For large scale deployments, it is recommended to use a CDN (such as Akamai, Level3 etc.). 
	
	In general, it's best to have nginx vod as close as possible to where the mp4 files are stored, 
	and have the caching proxies as close as possible to the end users.
2. Enable nginx-vod-module caches:
	* `vod_metadata_cache` - saves the need to re-read the video metadata for each segment. This cache should be rather large, in the order of GBs.
	* `vod_response_cache` - saves the responses of manifest requests. This cache may not be required when using a second layer of caching servers before nginx vod. 
		No need to allocate a large buffer for this cache, 128M is probably more than enough for most deployments.
	* `vod_mapping_cache` - for mapped mode only, few MBs is usually enough.
	* nginx's open_file_cache - caches open file handles.

	The hit/miss ratios of these caches can be tracked by enabling performance counters (`vod_performance_counters`)
	and setting up a status page for nginx vod (`vod_status`)
3. In local & mapped modes, enable aio. - nginx has to be compiled with aio support, and it has to be enabled in nginx conf (aio on). 
	You can verify it works by looking at the performance counters on the vod status page - read_file (aio off) vs. async_read_file (aio on)
4. In local & mapped modes, enable asynchronous file open - nginx has to be compiled with threads support, and `vod_open_file_thread_pool`
	has to be specified in nginx.conf. You can verify it works by looking at the performance counters on the vod status page - 
	open_file vs. async_open_file. Note that open_file may be nonzero with vod_open_file_thread_pool enabled, due to the open file cache - 
	open requests that are served from cache will be counted as synchronous open_file.
5. When using DRM enabled DASH/MSS, if the video files have a single nalu per frame, set `vod_min_single_nalu_per_frame_segment` to non-zero.
6. The muxing overhead of the streams generated by this module can be reduced by changing the following parameters:
	* HDS - set `vod_hds_generate_moof_atom` to off
	* HLS - set `vod_hls_mpegts_align_frames` to off and `vod_hls_mpegts_interleave_frames` to on
7. Enable gzip compression on manifest responses - 

	`gzip_types application/vnd.apple.mpegurl video/f4m application/dash+xml text/xml`
8. Apply common nginx performance best practices, such as tcp_nodelay=on, client_header_timeout etc.

### Configuration directives - base

#### vod
* **syntax**: `vod segmenter`
* **default**: `n/a`
* **context**: `location`

Enables the nginx-vod module on the enclosing location. 
The allowed values for `segmenter` are:

1. `none` - serves the MP4 files as is / clipped
2. `dash` - Dynamic Adaptive Streaming over HTTP packager
3. `hds` - Adobe HTTP Dynamic Streaming packager
4. `hls` - Apple HTTP Live Streaming packager
5. `mss` - Microsoft Smooth Streaming packager
6. `thumb` - thumbnail capture
7. `volume_map` - audio volume map

#### vod_mode
* **syntax**: `vod_mode mode`
* **default**: `local`
* **context**: `http`, `server`, `location`

Sets the file access mode - local, remote or mapped (see the features section above for more details)

#### vod_status
* **syntax**: `vod_status`
* **default**: `n/a`
* **context**: `location`

Enables the nginx-vod status page on the enclosing location. 
The following query params are supported:
* `?reset=1` - resets the performance counters and cache stats.
* `?format=prom` - returns the output in format compatible with Prometheus (the default format is XML).

### Configuration directives - segmentation

#### vod_segment_duration
* **syntax**: `vod_segment_duration duration`
* **default**: `10s`
* **context**: `http`, `server`, `location`

Sets the segment duration in milliseconds. It is highly recommended to use a segment duration that is a multiple of the GOP duration.
If the segment duration is not a multiple of GOP duration, and `vod_align_segments_to_key_frames` is enabled, there could be significant
differences between the segment duration that is reported in the manifest and the actual segment duration. This could also lead to
the appearance of empty segments within the stream.

#### vod_live_window_duration
* **syntax**: `vod_live_window_duration duration`
* **default**: `30000`
* **context**: `http`, `server`, `location`

Sets the total duration in milliseconds of the segments that should be returned in a live manifest.
If the value is positive, nginx vod returns a range of maximum `vod_live_window_duration` milliseconds, ending at the current server time.
If the value is negative, nginx vod returns a range of maximum `-vod_live_window_duration` milliseconds from the end of the mapping json.
If the value is set to zero, the live manifest will contain all the segments that are fully contained in the mapping json time frame.

#### vod_force_playlist_type_vod
* **syntax**: `vod_force_playlist_type_vod on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

Generate a vod stream even when the media set has `playlistType=live`. 
Enabling this setting has the following effects:
1. Frame timestamps will be continuous and start from zero
2. Segment indexes will start from one
3. In case of HLS, the returned manifest will have both `#EXT-X-PLAYLIST-TYPE:VOD` and `#EXT-X-ENDLIST`

This can be useful for clipping vod sections out of a live stream.

#### vod_force_continuous_timestamps
* **syntax**: `vod_force_continuous_timestamps on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

Generate continuous timestamps even when the media set has gaps (gaps can created by the use of `clipTimes`)
If ID3 timestamps are enabled (`vod_hls_mpegts_output_id3_timestamps`), they contain the original timestamps that were set in `clipTimes`.

#### vod_bootstrap_segment_durations
* **syntax**: `vod_bootstrap_segment_durations duration`
* **default**: `none`
* **context**: `http`, `server`, `location`

Adds a bootstrap segment duration in milliseconds. This setting can be used to make the first few segments
shorter than the default segment duration, thus making the adaptive bitrate selection kick-in earlier without 
the overhead of short segments throughout the video.

#### vod_align_segments_to_key_frames
* **syntax**: `vod_align_segments_to_key_frames on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, the module forces all segments to start with a key frame. Enabling this setting can lead to differences
between the actual segment durations and the durations reported in the manifest (unless `vod_manifest_segment_durations_mode` is set to accurate).

#### vod_segment_count_policy
* **syntax**: `vod_segment_count_policy last_short/last_long/last_rounded`
* **default**: `last_short`
* **context**: `http`, `server`, `location`

Configures the policy for calculating the segment count, for segment_duration = 10 seconds:
* last_short - a file of 33 sec is partitioned as - 10, 10, 10, 3
* last_long - a file of 33 sec is partitioned as - 10, 10, 13
* last_rounded - a file of 33 sec is partitioned as - 10, 10, 13, a file of 38 sec is partitioned as 10, 10, 10, 8

#### vod_manifest_duration_policy
* **syntax**: `vod_manifest_duration_policy min/max`
* **default**: `max`
* **context**: `http`, `server`, `location`

Configures the policy for calculating the duration of a manifest containing multiple streams:
* max - uses the maximum stream duration (default)
* min - uses the minimum non-zero stream duration

#### vod_manifest_segment_durations_mode
* **syntax**: `vod_manifest_segment_durations_mode estimate/accurate`
* **default**: `estimate`
* **context**: `http`, `server`, `location`

Configures the calculation mode of segment durations within manifest requests:
* estimate - reports the duration as configured in nginx.conf, e.g. if `vod_segment_duration` has the value 10000,
an HLS manifest will contain #EXTINF:10
* accurate - reports the exact duration of the segment, taking into account the frame durations, e.g. for a 
frame rate of 29.97 and 10 second segments it will report the first segment as 10.01. accurate mode also
takes into account the key frame alignment, in case `vod_align_segments_to_key_frames` is on

#### vod_media_set_override_json
* **syntax**: `vod_media_set_override_json json`
* **default**: `{}`
* **context**: `http`, `server`, `location`

This parameter provides a way to override portions of the media set JSON (mapped mode only).
For example, `vod_media_set_override_json '{"clipTo":20000}'` clips the media set to 20 sec.
The parameter value can contain variables.

### Configuration directives - upstream

#### vod_upstream_location
* **syntax**: `vod_upstream_location location`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets an nginx location that is used to read the MP4 file (remote mode) or mapping the request URI (mapped mode).

#### vod_remote_upstream_location
* **syntax**: `vod_remote_upstream_location location`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets an nginx location that is used to read the MP4 file on remote or mapped mode. If this directive is set on mapped mode, the module reads 
the MP4 files over HTTP, treating the paths in the mapping JSON as URIs (the default behavior is to read from local files)

#### vod_max_upstream_headers_size
* **syntax**: `vod_max_upstream_headers_size size`
* **default**: `4k`
* **context**: `http`, `server`, `location`

Sets the size that is allocated for holding the response headers when issuing upstream requests (to vod_xxx_upstream_location).

#### vod_upstream_extra_args
* **syntax**: `vod_upstream_extra_args "arg1=value1&arg2=value2&..."`
* **default**: `empty`
* **context**: `http`, `server`, `location`

Extra query string arguments that should be added to the upstream request (remote/mapped modes only).
The parameter value can contain variables.

#### vod_media_set_map_uri
* **syntax**: `vod_media_set_map_uri uri`
* **default**: `$vod_suburi`
* **context**: `http`, `server`, `location`

Sets the uri of media set mapping requests, the parameter value can contain variables.
In case of multi url, `$vod_suburi` will be the current sub uri (a separate request is issued per sub URL)

#### vod_path_response_prefix
* **syntax**: `vod_path_response_prefix prefix`
* **default**: `{"sequences":[{"clips":[{"type":"source","path":"`
* **context**: `http`, `server`, `location`

Sets the prefix that is expected in URI mapping responses (mapped mode only).

#### vod_path_response_postfix
* **syntax**: `vod_path_response_postfix postfix`
* **default**: `"}]}]}`
* **context**: `http`, `server`, `location`

Sets the postfix that is expected in URI mapping responses (mapped mode only).

#### vod_max_mapping_response_size
* **syntax**: `vod_max_mapping_response_size length`
* **default**: `1K`
* **context**: `http`, `server`, `location`

Sets the maximum length of a path returned from upstream (mapped mode only).

### Configuration directives - fallback

#### vod_fallback_upstream_location
* **syntax**: `vod_fallback_upstream_location location`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets an nginx location to which the request is forwarded after encountering a file not found error (local/mapped modes only).

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

### Configuration directives - performance

#### vod_metadata_cache
* **syntax**: `vod_metadata_cache zone_name zone_size [expiration]`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the size and shared memory object name of the video metadata cache. For MP4 files, this cache holds the moov atom.

#### vod_mapping_cache
* **syntax**: `vod_mapping_cache zone_name zone_size [expiration]`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the size and shared memory object name of the mapping cache for vod (mapped mode only).

#### vod_live_mapping_cache
* **syntax**: `vod_live_mapping_cache zone_name zone_size [expiration]`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the size and shared memory object name of the mapping cache for live (mapped mode only).

#### vod_response_cache
* **syntax**: `vod_response_cache zone_name zone_size [expiration]`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the size and shared memory object name of the response cache. The response cache holds manifests
and other non-video content (like DASH init segment, HLS encryption key etc.). Video segments are not cached.

#### vod_live_response_cache
* **syntax**: `vod_live_response_cache zone_name zone_size [expiration]`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the size and shared memory object name of the response cache for time changing live responses. 
This cache holds the following types of responses for live: DASH MPD, HLS index M3U8, HDS bootstrap, MSS manifest.

#### vod_initial_read_size
* **syntax**: `vod_initial_read_size size`
* **default**: `4K`
* **context**: `http`, `server`, `location`

Sets the size of the initial read operation of the MP4 file.

#### vod_max_metadata_size
* **syntax**: `vod_max_metadata_size size`
* **default**: `128MB`
* **context**: `http`, `server`, `location`

Sets the maximum supported video metadata size (for MP4 - moov atom size)

#### vod_max_frames_size
* **syntax**: `vod_max_frames_size size`
* **default**: `16MB`
* **context**: `http`, `server`, `location`

Sets the limit on the total size of the frames of a single segment

#### vod_max_frame_count
* **syntax**: `vod_max_frame_count count`
* **default**: `1048576`
* **context**: `http`, `server`, `location`

Sets the limit on the total count of the frames read to serve non segment (e.g. playlist) request.

#### vod_segment_max_frame_count
* **syntax**: `vod_segment_max_frame_count count`
* **default**: `65536`
* **context**: `http`, `server`, `location`

Sets the limit on the total count of the frames read to serve segment request.

#### vod_cache_buffer_size
* **syntax**: `vod_cache_buffer_size size`
* **default**: `256K`
* **context**: `http`, `server`, `location`

Sets the size of the cache buffers used when reading MP4 frames.

#### vod_open_file_thread_pool
* **syntax**: `vod_open_file_thread_pool pool_name`
* **default**: `off`
* **context**: `http`, `server`, `location`

Enables the use of asynchronous file open via thread pool.
The thread pool must be defined with a thread_pool directive, if no pool name is specified the default pool is used.
This directive is supported only on nginx 1.7.11 or newer when compiling with --add-threads.
Note: this directive currently disables the use of nginx's open_file_cache by nginx-vod-module

#### vod_output_buffer_pool
* **syntax**: `vod_output_buffer_pool size count`
* **default**: `off`
* **context**: `http`, `server`, `location`

Pre-allocates buffers for generating response data, saving the need allocate/free the buffers on every request.

#### vod_performance_counters
* **syntax**: `vod_performance_counters zone_name`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the shared memory object name of the performance counters

### Configuration directives - url structure

#### vod_base_url
* **syntax**: `vod_base_url url`
* **default**: `see below`
* **context**: `http`, `server`, `location`

Sets the base URL (scheme + domain) that should be returned in manifest responses.
The parameter value can contain variables, if the parameter evaluates to an empty string, relative URLs will be used.
If the parameter evaluates to a string ending with /, it is assumed to be a full URL - the module only appends the
file name to it, instead of a full URI.
If not set, the base URL is determined as follows:
1. If the request did not contain a host header (HTTP/1.0) relative URLs will be returned
2. Otherwise, the base URL will be `$scheme://$http_host`
The setting currently affects only HLS and DASH. In MSS and HDS, relative URLs are always returned.

#### vod_segments_base_url
* **syntax**: `vod_segments_base_url url`
* **default**: `see below`
* **context**: `http`, `server`, `location`

Sets the base URL (scheme + domain) that should be used for delivering video segments.
The parameter value can contain variables, if the parameter evaluates to an empty string, relative URLs will be used.
If not set, vod_base_url will be used.
The setting currently affects only HLS.

#### vod_multi_uri_suffix
* **syntax**: `vod_multi_uri_suffix suffix`
* **default**: `.urlset`
* **context**: `http`, `server`, `location`

A URL suffix that is used to identify multi URLs. A multi URL is a way to encode several different URLs
that should be played together as an adaptive streaming set, under a single URL. When the default suffix is
used, an HLS set URL may look like: 
http://host/hls/common-prefix,bitrate1,bitrate2,common-suffix.urlset/master.m3u8

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

The name of the tracks request parameter.

#### vod_time_shift_param_name
* **syntax**: `vod_time_shift_param_name name`
* **default**: `shift`
* **context**: `http`, `server`, `location`

The name of the shift request parameter.

#### vod_speed_param_name
* **syntax**: `vod_speed_param_name name`
* **default**: `speed`
* **context**: `http`, `server`, `location`

The name of the speed request parameter.

#### vod_lang_param_name
* **syntax**: `vod_lang_param_name name`
* **default**: `lang`
* **context**: `http`, `server`, `location`

The name of the language request parameter.

#### vod_force_sequence_index
* **syntax**: `vod_force_sequence_index on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

Use sequence index in segment uris even if there is only one sequence

### Configuration directives - response headers

#### vod_expires
* **syntax**: `vod_expires time`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the value of the "Expires" and "Cache-Control" response headers for successful requests.
This directive is similar to nginx's built-in `expires` directive, except that it only supports the expiration interval scenario
(epoch, max, off, day time are not supported)
Main motivation for using this directive instead of the built-in `expires` is to have different expiration for VOD and dynamic live content.
If this directive is not specified, nginx-vod-module will not set the "Expires" / "Cache-Control" headers.
This setting affects all types of requests in VOD playlists and segment requests in live playlists.

#### vod_expires_live
* **syntax**: `vod_expires_live time`
* **default**: `none`
* **context**: `http`, `server`, `location`

Same as `vod_expires` (above) for live requests that are not time dependent and not segments (e.g. HLS - master.m3u8, HDS - manifest.f4m).

#### vod_expires_live_time_dependent
* **syntax**: `vod_expires_live_time_dependent time`
* **default**: `none`
* **context**: `http`, `server`, `location`

Same as `vod_expires` (above) for live requests that are time dependent (HLS - index.m3u8, HDS - bootstrap.abst, MSS - manifest, DASH - manifest.mpd).

#### vod_last_modified
* **syntax**: `vod_last_modified time`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the value of the Last-Modified header returned on the response, by default the module does not return a Last-Modified header.
The reason for having this parameter here is in order to support If-Modified-Since / If-Unmodified-Since.
Since nginx's builtin ngx_http_not_modified_filter_module runs before any other header filter module, it will not see any headers set by add_headers / more_set_headers.
This makes nginx always reply as if the content changed (412 for If-Unmodified-Since / 200 for If-Modified-Since)
For live requests that are not segments (e.g. live DASH MPD), Last-Modified is set to the current server time.

#### vod_last_modified_types
* **syntax**: `vod_last_modified_types mime-type1 mime-type2 ...`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the MIME types for which the Last-Modified header should be set.
The special value "*" matches any MIME type.

### Configuration directives - ad stitching (mapped mode only)

#### vod_dynamic_mapping_cache
* **syntax**: `vod_dynamic_mapping_cache zone_name zone_size [expiration]`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the size and shared memory object name of the cache that stores the mapping of dynamic clips.

#### vod_dynamic_clip_map_uri
* **syntax**: `vod_dynamic_clip_map_uri uri`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the uri that should be used to map dynamic clips. 
The parameter value can contain variables, specifically, `$vod_clip_id` contains the id of the clip that should be mapped.
The expected response from this uri is a JSON containing a concat clip object.

#### vod_source_clip_map_uri
* **syntax**: `vod_source_clip_map_uri uri`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the uri that should be used to map source clips defined using the clipIds property of concat. 
The parameter value can contain variables, specifically, `$vod_clip_id` contains the id of the clip that should be mapped.
The expected response from this uri is a JSON containing a source clip object.

#### vod_redirect_segments_url
* **syntax**: `vod_redirect_segments_url url`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets a url to which requests for segments should be redirected.
The parameter value can contain variables, specifically, `$vod_dynamic_mapping` contains a serialized representation of the mapping of dynamic clips.

#### vod_apply_dynamic_mapping
* **syntax**: `vod_apply_dynamic_mapping mapping`
* **default**: `none`
* **context**: `http`, `server`, `location`

Maps dynamic clips to concat clips using the given expression, previously generated by `$vod_dynamic_mapping`.
The parameter value can contain variables.

#### vod_notification_uri
* **syntax**: `vod_notification_uri uri`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the uri that should be used to issue notifications. 
The parameter value can contain variables, specifically, `$vod_notification_id` contains the id of the notification that is being fired.
The response from this uri is ignored.

### Configuration directives - DRM / encryption

#### vod_secret_key
* **syntax**: `vod_secret_key string`
* **default**: `empty`
* **context**: `http`, `server`, `location`

Sets the seed that is used to generate the TS encryption key and DASH/MSS encryption IVs.
The parameter value can contain variables, and will usually have the structure "secret-$vod_filepath".
See the list of nginx variables added by this module below.

#### vod_encryption_iv_seed
* **syntax**: `vod_encryption_iv_seed string`
* **default**: `empty`
* **context**: `http`, `server`, `location`

Sets the seed that is used to generate the encryption IV, currently applies only to HLS/fMP4 with AES-128 encryption.
The parameter value can contain variables.

#### vod_drm_enabled
* **syntax**: `vod_drm_enabled on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, the module encrypts the media segments according to the response it gets from the drm upstream.
Currently supported only for dash and mss (play ready).

#### vod_drm_single_key
* **syntax**: `vod_drm_single_key on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, the module requests the drm info only for the first sequence and applies it to all sequences.
When disabled, the drm info is requested for each sequence separately.
In addition, in DASH, enabling this setting makes the module place the ContentProtection tag under AdaptationSet,
otherwise, it is placed under Representation.

#### vod_drm_clear_lead_segment_count
* **syntax**: `vod_drm_clear_lead_segment_count count`
* **default**: `1`
* **context**: `http`, `server`, `location`

Sets the number of clear (unencrypted) segments in the beginning of the stream. A clear lead enables the player to start playing without having to wait for the license response.

#### vod_drm_max_info_length
* **syntax**: `vod_drm_max_info_length length`
* **default**: `4K`
* **context**: `http`, `server`, `location`

Sets the maximum length of a drm info returned from upstream.

#### vod_drm_upstream_location
* **syntax**: `vod_drm_upstream_location location`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the nginx location that should be used for getting the DRM info for the file.

#### vod_drm_info_cache
* **syntax**: `vod_drm_info_cache zone_name zone_size [expiration]`
* **default**: `off`
* **context**: `http`, `server`, `location`

Configures the size and shared memory object name of the drm info cache.

#### vod_drm_request_uri
* **syntax**: `vod_drm_request_uri uri`
* **default**: `$vod_suburi`
* **context**: `http`, `server`, `location`

Sets the uri of drm info requests, the parameter value can contain variables.
In case of multi url, `$vod_suburi` will be the current sub uri (a separate drm info request is issued per sub URL)

#### vod_min_single_nalu_per_frame_segment
* **syntax**: `vod_min_single_nalu_per_frame_segment index`
* **default**: `0`
* **context**: `http`, `server`, `location`

Sets the minimum segment index (1-based) that should be assumed to have a single h264 nalu per frame.
If the value is 0, no assumption is being made on the number of nal units per frame.
This setting only affects DASH and MSS configurations that have DRM enabled.

When transcoding videos using libx264, by default, all frames have a single nal unit, except the first frame
that contains an additional nalu with the libx264 copyright information. Setting this parameter to a value
greater than 0 can provide a significant performance improvement, since the layout of the segment can be
calculated in advance, allowing the module to:
* Output segment buffers as they are generated (it doesn't have to wait for the whole segment to complete)
* Avoid frame processing for requests that do not need the segment data (e.g. HEAD, range 0-0, etc.)

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

#### vod_dash_profiles
* **syntax**: `vod_dash_profiles profiles`
* **default**: `urn:mpeg:dash:profile:isoff-main:2011`
* **context**: `http`, `server`, `location`

Sets the profiles that are returned in the MPD tag in manifest responses.

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

#### vod_dash_manifest_format
* **syntax**: `vod_dash_manifest_format format`
* **default**: `segmenttimeline`
* **context**: `http`, `server`, `location`

Sets the MPD format, available options are:
* `segmentlist` - uses SegmentList and SegmentURL tags, in this format the URL of each fragment is explicitly set in the MPD
* `segmenttemplate` - uses SegmentTemplate, reporting a single duration for all fragments
* `segmenttimeline` - uses SegmentTemplate and SegmentTimeline to explicitly set the duration of the fragments

#### vod_dash_subtitle_format
* **syntax**: `vod_dash_subtitle_format format`
* **default**: `webvtt`
* **context**: `http`, `server`, `location`

Sets the format of the subtitles returned in the MPD, available options are:
* `webvtt` - WebVTT
* `smpte-tt` - SMPTE Timed Text

#### vod_dash_init_mp4_pssh
* **syntax**: `vod_dash_init_mp4_pssh on/off`
* **default**: `on`
* **context**: `http`, `server`, `location`

When enabled, the DRM pssh boxes are returned in the DASH init segment and in the manifest.
When disabled, the pssh boxes are returned only in the manifest.

#### vod_dash_duplicate_bitrate_threshold
* **syntax**: `vod_dash_duplicate_bitrate_threshold threshold`
* **default**: `4096`
* **context**: `http`, `server`, `location`

The bitrate threshold for removing identical bitrates, streams whose bitrate differences are less than
this value will be considered identical.

#### vod_dash_use_base_url_tag
* **syntax**: `vod_dash_use_base_url_tag on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, a BaseURL tag will be used to specify the fragments/init segment base url.
Otherwise, the media/initialization attributes under SegmentTemplate will contain absolute URLs. 

### Configuration directives - HDS

#### vod_hds_absolute_manifest_urls
* **syntax**: `vod_hds_absolute_manifest_urls on/off`
* **default**: `on`
* **context**: `http`, `server`, `location`

When enabled the server returns the base URL in the F4M manifest

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

#### vod_hds_generate_moof_atom
* **syntax**: `vod_hds_generate_moof_atom on/off`
* **default**: `on`
* **context**: `http`, `server`, `location`

When enabled the module generates a moof atom in the HDS fragments, when disabled only an mdat atom is generated.
Turning this parameter off reduces the packaging overhead, however the default is on since Adobe tools are generating this atom.

### Configuration directives - HLS

#### vod_hls_encryption_method
* **syntax**: `vod_hls_encryption_method method`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the encryption method of HLS segments, allowed values are: none (default), aes-128, sample-aes, sample-aes-cenc.

#### vod_hls_force_unmuxed_segments
* **syntax**: `vod_hls_force_unmuxed_segments on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled the server returns the audio stream in separate segments than the ones used by the video stream (using EXT-X-MEDIA)

#### vod_hls_container_format
* **syntax**: `vod_hls_container_format mpegts/fmp4/auto`
* **default**: `auto`
* **context**: `http`, `server`, `location`

Sets the container format of the HLS segments. 
The default behavior is to use fmp4 for HEVC, and mpegts otherwise (Apple does not support HEVC over MPEG TS).

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

#### vod_hls_output_iframes_playlist
* **syntax**: `vod_hls_output_iframes_playlist on/off`
* **default**: `on`
* **context**: `http`, `server`, `location`

When disabled iframe playlists are not returned as part of master playlists

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

#### vod_hls_init_file_name_prefix
* **syntax**: `vod_hls_init_file_name_prefix name`
* **default**: `init`
* **context**: `http`, `server`, `location`

The name of the init segment file name, only relevant when using fmp4 container.

#### vod_hls_encryption_key_file_name
* **syntax**: `vod_hls_encryption_key_file_name name`
* **default**: `encryption.key`
* **context**: `http`, `server`, `location`

The name of the encryption key file name, only relevant when encryption method is not `none`.

#### vod_hls_encryption_key_uri
* **syntax**: `vod_hls_encryption_key_uri uri`
* **default**: `a url pointing to encryption.key`
* **context**: `http`, `server`, `location`

Sets the value of the URI attribute of EXT-X-KEY, only relevant when encryption method is not `none`.
The parameter value can contain variables.

#### vod_hls_encryption_key_format
* **syntax**: `vod_hls_encryption_key_format format`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the value of the KEYFORMAT attribute of EXT-X-KEY, only relevant when encryption method is not `none`.

#### vod_hls_encryption_key_format_versions
* **syntax**: `vod_hls_encryption_key_format_versions versions`
* **default**: `none`
* **context**: `http`, `server`, `location`

Sets the value of the KEYFORMATVERSIONS attribute of EXT-X-KEY, only relevant when encryption method is not `none`.

#### vod_hls_mpegts_interleave_frames
* **syntax**: `vod_hls_mpegts_interleave_frames on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, the HLS muxer interleaves frames of different streams (audio / video).
When disabled, on every switch between audio / video the muxer flushes the MPEG TS packet.

#### vod_hls_mpegts_align_frames
* **syntax**: `vod_hls_mpegts_align_frames on/off`
* **default**: `on`
* **context**: `http`, `server`, `location`

When enabled, every video / audio frame is aligned to MPEG TS packet boundary,
padding is added as needed.

#### vod_hls_mpegts_output_id3_timestamps
* **syntax**: `vod_hls_mpegts_output_id3_timestamps on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, an ID3 TEXT frame is outputted in each TS segment.
The content of the ID3 TEXT frame can be set using the directive `vod_hls_mpegts_id3_data`.

#### vod_hls_mpegts_id3_data
* **syntax**: `vod_hls_mpegts_id3_data string`
* **default**: `{"timestamp":$vod_segment_time,"sequenceId":"$vod_sequence_id"}`
* **context**: `http`, `server`, `location`

Sets the data of the ID3 TEXT frame outputted in each TS segment, when `vod_hls_mpegts_output_id3_timestamps` is set to `on`.
When the directive is not set, the ID3 frames contain by default a JSON object of the format `{"timestamp":1459779115000,"sequenceId":"{id}"}`:
- `timestamp` - an absolute time measured in milliseconds since the epoch (unixtime x 1000).
- `sequenceId` - the id field of the sequence object, as specified in the mapping JSON. The field is omitted when the sequence id is empty / not specified in the mapping JSON.
The parameter value can contain variables.

#### vod_hls_mpegts_align_pts
* **syntax**: `vod_hls_mpegts_align_pts on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, the module will shift back the dts timestamps by the pts delay of the initial frame.
This can help keep the pts timestamps aligned across multiple renditions.

#### vod_hls_encryption_output_iv
* **syntax**: `vod_hls_encryption_output_iv on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, the module outputs the `IV` attribute in returned `#EXT-X-KEY` tags.

### Configuration directives - MSS

#### vod_mss_manifest_file_name_prefix
* **syntax**: `vod_mss_manifest_file_name_prefix name`
* **default**: `manifest`
* **context**: `http`, `server`, `location`

The name of the manifest file (has no extension).

#### vod_mss_duplicate_bitrate_threshold
* **syntax**: `vod_mss_duplicate_bitrate_threshold threshold`
* **default**: `4096`
* **context**: `http`, `server`, `location`

The bitrate threshold for removing identical bitrates, streams whose bitrate differences are less than
this value will be considered identical.

### Configuration directives - thumbnail capture

#### vod_thumb_file_name_prefix
* **syntax**: `vod_thumb_file_name_prefix name`
* **default**: `thumb`
* **context**: `http`, `server`, `location`

The name of the thumbnail file (a jpg extension is implied).

#### vod_thumb_accurate_positioning
* **syntax**: `vod_thumb_accurate_positioning on/off`
* **default**: `on`
* **context**: `http`, `server`, `location`

When enabled, the module grabs the frame that is closest to the requested offset.
When disabled, the module uses the keyframe that is closest to the requested offset.
Setting this parameter to off can result in faster thumbnail capture, since the module 
always decodes a single video frame per request.

#### vod_gop_look_behind
* **syntax**: `vod_gop_look_behind millis`
* **default**: `10000`
* **context**: `http`, `server`, `location`

Sets the interval (in milliseconds) before the thumbnail offset that should be loaded.
This setting should be set to the maximum GOP size, setting it to a lower value may result in capture failure.
Note that the metadata of all frames between `offset - vod_gop_look_behind` and `offset + vod_gop_look_ahead`
is loaded, however only the frames of the minimum GOP containing `offset` will be read and decoded.

#### vod_gop_look_ahead
* **syntax**: `vod_gop_look_ahead millis`
* **default**: `1000`
* **context**: `http`, `server`, `location`

Sets the interval (in milliseconds) after the thumbnail offset that should be loaded.

### Configuration directives - volume map

#### vod_volume_map_file_name_prefix
* **syntax**: `vod_volume_map_file_name_prefix name`
* **default**: `volume_map`
* **context**: `http`, `server`, `location`

The name of the volume map file (a csv extension is implied).

#### vod_volume_map_interval
* **syntax**: `vod_volume_map_interval millis`
* **default**: `1000`
* **context**: `http`, `server`, `location`

Sets the interval/resolution (in milliseconds) of the volume map.

### Configuration directives - misc

#### vod_ignore_edit_list
* **syntax**: `vod_ignore_edit_list on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, the module ignores any edit lists (elst) in the MP4 file.

#### vod_parse_hdlr_name
* **syntax**: `vod_parse_hdlr_name on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, the module parses the name field of the hdlr MP4 atom, and uses it as the stream label.

#### vod_parse_udta_name
* **syntax**: `vod_parse_udta_name on/off`
* **default**: `off`
* **context**: `http`, `server`, `location`

When enabled, the module parses the name atom child of the udta MP4 atom, and uses it as the stream label.

### Nginx variables

The module adds the following nginx variables:
* `$vod_suburi` - the current sub uri. For example, if the url is:
  `http://<domain>/<location>/<prefix>,<middle1>,<middle2>,<middle3>,<postfix>.urlset/<filename>`
  `$vod_suburi` will have the value `http://<domain>/<location>/<prefix><middle1><postfix>/<filename>` 
  when processing the first uri.
* `$vod_filepath` - in local / mapped modes, the file path of current sub uri. In remote mode, has the same value as `$vod_suburi`.
* `$vod_set_id` - contains the id of the set.
* `$vod_sequence_id` - contains the id of the current sequence, if no id was specified in the mapping json this variable will be the same as `$vod_suburi`.
* `$vod_clip_id` - the id of the current clip, this variable has a value during these phases:
  1. Mapping of dynamic clips to concat clips
  2. Mapping of source clip to paths
* `$vod_notification_id` - the id of the current notification, the value is non-empty only when referenced by `vod_notification_uri`
* `$vod_dynamic_mapping` - a serialized representation of the mapping of dynamic clips to concat clips.
* `$vod_request_params` - a serialized representation of the request params, e.g. 12-f2-v1-a1. The variable contains:
  1. The segment index (for a segment request)
  2. The sequence index
  3. A selection of audio/video tracks
* `$vod_status` - the internal error code of the module, provides a more fine grained classification of errors than http status.
	the following values are defined:
	`BAD_REQUEST` - the request is invalid, for example, `clipFrom` is larger than the video duration
	`NO_STREAMS` - an invalid segment index was requested
	`EMPTY_MAPPING` - the mapping response is empty
	`BAD_MAPPING` - the mapping json is invalid, for example, the `sequences` element is missing
	`BAD_DATA` - the video file is corrupt
	`EXPIRED` - the current server time is larger than `expirationTime`
	`ALLOC_FAILED` - the module failed to allocate memory
	`UNEXPECTED` - a scenario that is not supposed to happen, most likely a bug in the module
* `$vod_segment_time` - for segment requests, contains the absolute timestamp of the first frame in the segment, measured in milliseconds since the epoch (unixtime x 1000).
* `$vod_segment_duration` - for segment requests, contains the duration of the segment in milliseconds
* `$vod_frames_bytes_read` - for segment requests, total number of bytes read while processing media frames

Note: Configuration directives that can accept variables are explicitly marked as such.

### Sample configurations

#### Local configuration
```nginx
	http {
		upstream fallback {
			server fallback.kaltura.com:80;
		}

		server {
			# vod settings
			vod_mode local;
			vod_fallback_upstream_location /fallback;
			vod_last_modified 'Sun, 19 Nov 2000 08:52:00 GMT';
			vod_last_modified_types *;

			# vod caches
			vod_metadata_cache metadata_cache 512m;
			vod_response_cache response_cache 128m;
			
			# gzip manifests
			gzip on;
			gzip_types application/vnd.apple.mpegurl;

			# file handle caching / aio
			open_file_cache          max=1000 inactive=5m;
			open_file_cache_valid    2m;
			open_file_cache_min_uses 1;
			open_file_cache_errors   on;
			aio on;
			
			location ^~ /fallback/ {
				internal;
				proxy_pass http://fallback/;
				proxy_set_header Host $http_host;
			}

			location /content/ {
				root /web/;
				vod hls;
				
				add_header Access-Control-Allow-Headers '*';
				add_header Access-Control-Expose-Headers 'Server,range,Content-Length,Content-Range';
				add_header Access-Control-Allow-Methods 'GET, HEAD, OPTIONS';
				add_header Access-Control-Allow-Origin '*';
				expires 100d;
			}
		}
	}
```

#### Mapped configuration
```nginx
	http {
		upstream kalapi {
			server www.kaltura.com:80;
		}

		upstream fallback {
			server fallback.kaltura.com:80;
		}

		server {
			# vod settings
			vod_mode mapped;
			vod_upstream_location /kalapi;
			vod_upstream_extra_args "pathOnly=1";
			vod_fallback_upstream_location /fallback;
			vod_last_modified 'Sun, 19 Nov 2000 08:52:00 GMT';
			vod_last_modified_types *;

			# vod caches
			vod_metadata_cache metadata_cache 512m;
			vod_response_cache response_cache 128m;
			vod_mapping_cache mapping_cache 5m;
			
			# gzip manifests
			gzip on;
			gzip_types application/vnd.apple.mpegurl;

			# file handle caching / aio
			open_file_cache          max=1000 inactive=5m;
			open_file_cache_valid    2m;
			open_file_cache_min_uses 1;
			open_file_cache_errors   on;
			aio on;
			
			location ^~ /fallback/ {
				internal;
				proxy_pass http://fallback/;
				proxy_set_header Host $http_host;
			}

			location ^~ /kalapi/ {
				internal;
				proxy_pass http://kalapi/;
				proxy_set_header Host $http_host;
			}

			location ~ ^/p/\d+/(sp/\d+/)?serveFlavor/ {
				# encrypted hls
				vod hls;
				vod_secret_key "mukkaukk$vod_filepath";
				vod_hls_encryption_method aes-128;
				
				add_header Access-Control-Allow-Headers '*';
				add_header Access-Control-Expose-Headers 'Server,range,Content-Length,Content-Range';
				add_header Access-Control-Allow-Methods 'GET, HEAD, OPTIONS';
				add_header Access-Control-Allow-Origin '*';
				expires 100d;
			}
		}
	}
```

#### Mapped + Remote configuration
```nginx
	http {
		upstream jsonupstream {
			server jsonserver:80;
		}

		server {
			# vod settings
			vod_mode mapped;
			vod_upstream_location /json;
			vod_remote_upstream_location /proxy;
			vod_upstream_extra_args "pathOnly=1";
			vod_last_modified 'Sun, 19 Nov 2000 08:52:00 GMT';
			vod_last_modified_types *;

			# vod caches
			vod_metadata_cache metadata_cache 512m;
			vod_response_cache response_cache 128m;
			vod_mapping_cache mapping_cache 5m;

			# gzip manifests
			gzip on;
			gzip_types application/vnd.apple.mpegurl;

			# file handle caching / aio
			open_file_cache	  max=1000 inactive=5m;
			open_file_cache_valid    2m;
			open_file_cache_min_uses 1;
			open_file_cache_errors   on;
			aio on;

			location ^~ /json/hls/ {
				internal;
				proxy_pass http://jsonupstream/;
				proxy_set_header Host $http_host;
			}

			location ~ /proxy/([^/]+)/(.*) {
				internal;
				proxy_pass $1://$2;
				resolver 8.8.8.8;
			}

			location ~ ^/hls/ {
				vod hls;

				add_header Access-Control-Allow-Headers '*';
				add_header Access-Control-Expose-Headers 'Server,range,Content-Length,Content-Range';
				add_header Access-Control-Allow-Methods 'GET, HEAD, OPTIONS';
				add_header Access-Control-Allow-Origin '*';
				expires 100d;
			}
		}
	}
```

Set it up so that http://jsonserver:80/test.json returns the following JSON:
```json
	{
		"sequences": [{
			"clips": [{
				"type": "source",
				"path": "/http/commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4"
			}]
		}]
	}
```

And use this stream URL - http://nginx-vod-server/hls/test.json/master.m3u8

#### Remote configuration
```nginx
	http {
		upstream kalapi {
			server www.kaltura.com:80;
		}

		server {
			# vod settings
			vod_mode remote;
			vod_upstream_location /kalapi;
			vod_last_modified 'Sun, 19 Nov 2000 08:52:00 GMT';
			vod_last_modified_types *;

			# vod caches
			vod_metadata_cache metadata_cache 512m;
			vod_response_cache response_cache 128m;
			
			# gzip manifests
			gzip on;
			gzip_types application/vnd.apple.mpegurl;
			
			location ^~ /kalapi/ {
				internal;
				proxy_pass http://kalapi/;
				proxy_set_header Host $http_host;
			}

			location ~ ^/p/\d+/(sp/\d+/)?serveFlavor/ {
				vod hls;
				
				add_header Access-Control-Allow-Headers '*';
				add_header Access-Control-Expose-Headers 'Server,range,Content-Length,Content-Range';
				add_header Access-Control-Allow-Methods 'GET, HEAD, OPTIONS';
				add_header Access-Control-Allow-Origin '*';
				expires 100d;
			}
		}
	}
```
### Copyright & License

All code in this project is released under the [AGPLv3 license](http://www.gnu.org/licenses/agpl-3.0.html) unless a different license for a particular library is specified in the applicable library path. 

Copyright © Kaltura Inc. All rights reserved.
