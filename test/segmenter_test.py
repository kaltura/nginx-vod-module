from __future__ import print_function
import itertools
import sys
import os
from functools import reduce

confMatrix = [
	[
		('hls', ['vod hls']),
		('hds', ['vod hds']),
		('mss', ['vod mss']),
		('dash-stl', ['vod dash', 'vod_dash_manifest_format segmenttimeline']),
		('dash-st', ['vod dash', 'vod_dash_manifest_format segmenttemplate']),
		('dash-sl', ['vod dash', 'vod_dash_manifest_format segmentlist']),
	],
	[
		('zw', ['vod_live_window_duration 0']),
		('nzw', ['vod_live_window_duration 60000']),
	],
	[
		('sd10', ['vod_segment_duration 9000']),
		('sd3', ['vod_segment_duration 3000']),
		('bs', ['vod_bootstrap_segment_durations 1000', 'vod_bootstrap_segment_durations 3000', 'vod_bootstrap_segment_durations 5000']),
	],
	[
		('akf', ['vod_align_segments_to_key_frames on']),
		('nkf', ['vod_align_segments_to_key_frames off']),
	],
	[
		('sps', ['vod_segment_count_policy last_short']),
		('spl', ['vod_segment_count_policy last_long']),
		('spr', ['vod_segment_count_policy last_rounded']),
	],
	[
		('dme', ['vod_manifest_segment_durations_mode estimate']),
		('dma', ['vod_manifest_segment_durations_mode accurate']),
	],
]

fileByProtocol = {
	'hls': 'master.m3u8',
	'hds': 'manifest.f4m',
	'mss': 'manifest',
	'dash-stl': 'manifest.mpd',
	'dash-st': 'manifest.mpd',
	'dash-sl': 'manifest.mpd',
}

jsonMatrix = [
	[
		('type', 'vod'),
		('type', 'playlist'),
		('type', 'live'),
	],
	[
		('disc', 'no'),
		('disc', 'yes'),
		('disc', 'gap'),
	],
	[
		('kf', 'yes'),		# include key frame durations
		('kf', 'no'),
	],
	[
		('csmi', 'yes'),	# consistentSequenceMediaInfo
		('csmi', 'no'),
	],
]

liveJsonMatrix = [
	[
		('window', '0'),
		('window', '30'),
	],
	[
		('sbt', 'no'),		# segmentBaseTime
		('sbt', 'yes'),
	],
	[
		('pet', 'past'),	# presentationEndTime
		('pet', 'future'),
	],
	[
		('ici', 'yes'),		# initialClipIndex
		('ici', 'no'),
	],
]

hostHeader = 'localhost:8001'

confTemplate = '''
worker_rlimit_core  500M;
working_directory   /tmp/;

#user nobody;
worker_processes  1;

error_log  /var/log/nginx/error_log debug;

pid		/var/run/nginx.pid;

events {
	worker_connections 4096;
	multi_accept on;
	use epoll;
}

http {

	upstream kalapi {
		server localhost;
	}
	
	include	   mime.types;
	default_type  application/octet-stream;

	log_format  main  '$remote_addr - $remote_user [$time_local] "$request" '
		'$status $bytes_sent $request_time "$http_referer" "$http_user_agent" '
		'"$http_host" $pid $request_length "$sent_http_content_range" $connection';

	access_log /var/log/nginx/access_log main;

	server {

		listen	   8001;
		server_name  vod;
		
		# common vod settings
		vod_mode mapped;
		vod_upstream_location /kalapi_proxy;
		vod_max_metadata_size 256m;
		vod_ignore_edit_list on;
		vod_max_mapping_response_size 64k;
		vod_hls_output_id3_timestamps on;
		add_header 'Access-Control-Allow-Headers' 'Origin,Range,Accept-Encoding,Referer,Cache-Control';
		add_header 'Access-Control-Expose-Headers' 'Server,Content-Length,Content-Range,Date';
		add_header 'Access-Control-Allow-Methods' 'GET, HEAD, OPTIONS';
		add_header 'Access-Control-Allow-Origin' '*';
		
		# internal location for vod subrequests
		location ^~ /kalapi_proxy/ {
			internal;
			proxy_pass http://kalapi/segmenter_test_backend.php/loc/;
			proxy_set_header Host $http_host;
		}
		
		%s
	}
}
'''

def getConf():
	locations = ''
	for comb in itertools.product(*confMatrix):
		locName = '-'.join(map(lambda x: x[0], comb))
		locDirectives = reduce(lambda x, y: x + y, map(lambda x: x[1], comb))
		locations += 'location /%s/ {\n%s\n}\n\n' % (locName, 
			'\n'.join(map(lambda x: '\t%s;' % x, locDirectives)))
	return confTemplate % locations.replace('\n', '\n\t\t')

def getTestUrls():
	result = []
	for confComb in itertools.product(*confMatrix):
		locName = '-'.join(map(lambda x: x[0], confComb))
		for baseJsonComb in itertools.product(*jsonMatrix):
			# decide on the live combinations
			combDict = dict(confComb + baseJsonComb)
			if combDict['type'] == 'live':
				liveCombs = itertools.product(*liveJsonMatrix)
				
				if (not combDict.has_key('sps') or 	# segment policy last short is the only option for live
					combDict.has_key('bs')):		# bootstrap segments not supported for live
					continue
			else:
				liveCombs = [map(lambda x: x[0], liveJsonMatrix)]

				# ignore some 
				if (combDict.has_key('nzw') or 		# live window relevant only for live
					combDict['disc'] == 'gap'):		# gap relevant only for live
					continue
				if combDict['type'] == 'vod':
					if combDict['disc'] == 'yes' or combDict['csmi'] == 'no':	# discontinuity and csmi irrelevant for vod
						continue
			
			for liveJsonComb in liveCombs:
				liveCombDict = dict(liveJsonComb)
				if (combDict['disc'] == 'no' or combDict.has_key('mss')) and combDict['type'] == 'live' and liveCombDict['sbt'] == 'no':
					continue	# segment base time mandatory in continuous live
				if combDict['type'] in set(['live', 'playlist']) and liveCombDict['sbt'] == 'yes' and combDict['disc'] == 'yes':
					continue	# passing segment base time with multi clip in discontinuous mode will result in error of gap too small
					
				# build the url
				jsonComb = list(baseJsonComb) + list(liveJsonComb)
				url = '/' + locName + ''.join(map(lambda x: '/%s/%s' % x, jsonComb)) + \
					'/time/@time@' + \
					'/' + fileByProtocol[confComb[0][0]]
				result.append(url)
	return result

if len(sys.argv) < 2:
	print('Usage:\n\tpython %s urls/conf' % os.path.basename(__file__))
	sys.exit(1)

if sys.argv[1] == 'conf':
	print(getConf())
else:
	for url in getTestUrls():
		print(hostHeader, url)
