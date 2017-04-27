<?php

/*
	Sample script for DASH/Clear key encyption

	nginx.conf:

		location /php_proxy/ {
			proxy_pass http://apache/;		# assumes an apache/php backend
			proxy_set_header Host $http_host;
		}
		
		location /edash/ {
			vod dash;
			vod_mode local;
			alias /path/to/mp4s;

			vod_dash_profiles urn:mpeg:dash:profile:isoff-live:2011;
			vod_dash_manifest_format segmenttemplate;
			vod_align_segments_to_key_frames on;

			vod_drm_enabled on;
			vod_drm_clear_lead_segment_count 0;
			vod_drm_upstream_location /php_proxy/;
			vod_drm_request_uri /dash_clear_key.php;

			add_header 'Access-Control-Allow-Headers' 'origin,range,accept-encoding,referer';
			add_header 'Access-Control-Expose-Headers' 'Server,range,Content-Length,Content-Range,Date';
			add_header 'Access-Control-Allow-Methods' 'GET, HEAD, OPTIONS';
			add_header 'Access-Control-Allow-Origin' '*';
		}
	
	test urls:
		manifest:	http://domain/edash/test.mp4/manifest.mpd
		license:	http://domain/php_proxy/dash_clear_key.php

*/

if (!function_exists('hex2bin'))
{
	function hex2bin($str) 
	{
		$sbin = '';
		$len = strlen($str);
		for ($i = 0; $i < $len; $i += 2) 
		{
			$sbin .= pack('H*', substr($str, $i, 2));
		}

		return $sbin;
	}
}

function b64url_dec($s)
{
	return base64_decode(str_replace(array('-', '_'), array('+', '/'), $s), true);
}

function b64url_enc($s)
{
	return str_replace(array('+', '/', '='), array('-', '_', ''), base64_encode($s));
}

function getKey($kid)
{
	// TODO: update secret
	return md5('secret' . $kid, true);
}

// TODO: add security controls and error validation

if ($_SERVER['REQUEST_METHOD'] == 'POST')
{
	// POST - assume license request
	$req = file_get_contents('php://input');
	$req = json_decode($req);
	$type = $req->type;
	$keys = array();
	foreach ($req->kids as $kid)
	{
		$key = b64url_enc(getKey(b64url_dec($kid)));
		$keys[] = array('k' => $key, 'kty' => 'oct', 'kid' => $kid);
	}

	$res = array('keys' => $keys, 'type' => $type);
	header('access-control-allow-origin: *');
}
else
{
	// GET - assume encryption request
	$kid = hex2bin('0123456789abcdef0123456789abcdef');	// TODO: allocate according to media id
	$res = array(array(
		'key' => base64_encode(getKey($kid)),
		'key_id' => base64_encode($kid),
		'pssh' => array(array(
			'uuid' => '1077efec-c0b2-4d02-ace3-3c1e52e2fb4b',
			'data' => base64_encode(hex2bin('00000001') . $kid . hex2bin('00000000'))
		))
	));
}

echo str_replace('\\/', '/', json_encode($res));
