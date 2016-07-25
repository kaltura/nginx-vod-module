<?php

// pseudo random generator (must generate consistent results)
class Random 
{
	private $RSeed = 0;

	public function __construct($s = 0) 
	{
		$this->RSeed = abs(intval($s)) % 9999999 + 1;
		$this->num(0, 1);
	}

	public function num($min, $max)
	{
		if ($this->RSeed == 0) $this->seed(mt_rand());
		$this->RSeed = ($this->RSeed * 125) % 2796203;
		return $this->RSeed % ($max - $min + 1) + $min;
	}
}

function getRequestParams()
{
        $scriptParts = explode('/', $_SERVER['SCRIPT_NAME']);
        $pathParts = array();
        if (isset($_SERVER['PHP_SELF']))
                $pathParts = explode('/', $_SERVER['PHP_SELF']);
        $pathParts = array_diff($pathParts, $scriptParts);

        $params = array();
        reset($pathParts);
        while(current($pathParts))
        {
                $key = each($pathParts);
                $value = each($pathParts);
                if (!array_key_exists($key['value'], $params))
                {
                        $params[$key['value']] = $value['value'];
                }
        }
        return $params;
}

function getSourceClips($filePaths, $durations, $generateKfs)
{
	$result = array();
	for ($i = 0; $i < count($filePaths); $i++)
	{
		$clip = array(
			"type" => "source",
			"path" => $filePaths[$i]
		);
		
		if ($generateKfs)
		{
			$r = new Random();
			$curPos = $r->num(0, 1000);
			$clip["firstKeyFrameOffset"] = $curPos;
			$limit = $durations[$i];
			$kfDurations = array();
			for (;;)
			{
				$curDuration = $r->num(500, 4000);
				$curPos += $curDuration;
				if ($curPos > $limit)
				{
					break;
				}
				$kfDurations[] = $curDuration;
			}
			$clip["keyFrameDurations"] = $kfDurations;
		}
		
		$result[] = $clip;
	}
	
	return $result;
}

// input params
$filePaths = array(
	"/web/content/r71v1/entry/data/241/803/0_p7y00o7h_0_93j6yqit_1.mp4",
	"/web/content/r70v1/entry/data/138/192/1_46gm1o7f_1_22any1sp_1.mp4",
	"/web/content/r70v1/entry/data/138/192/1_5bik2rp6_1_kg6dq8kb_1.mp4",
	);

$durations = array(
	34469,
	46738,
	24936,
);

$gapSize = 23456;

$sumDurations = array_sum($durations);

// validate input
$paramValues = array(
	'type' => array('vod', 'playlist', 'live'),
	'disc' => array('no', 'yes', 'gap'),
	'kf' => array('yes', 'no'),			// include key frame durations
	'csmi' => array('yes', 'no'),		// consistentSequenceMediaInfo
	'sbt' => array('yes', 'no'),		// segmentBaseTime
	'ici' => array('yes', 'no'),		// initialClipIndex
	'pet' => array('past', 'future'),	// presentationEndTime
);

$params = getRequestParams();
foreach ($paramValues as $paramName => $values)
{
	if (!in_array($params[$paramName], $values)) 
	{
		die("invalid value for '{$paramName}', must be one of [".implode(',', $values)."]\n");
	}
}

if (!is_numeric($params['window']))
{
	die("invalid value for 'window', must be numeric\n");
}

// parse the input
$discontinuity = $params['disc'] != 'no';
$playlistType = $params['type'];
$time = isset($params['time']) ? $params['time'] : time();

$mediaSet = array();

switch ($playlistType)
{
case 'vod':
	$sequence = array(
		"clips" => getSourceClips(array($filePaths[0]), array($durations[0]), $params['kf'] == 'yes')
		);
	$mediaSet = array_merge($mediaSet, array(
		"sequences" => array($sequence)
	));
	die(json_encode($mediaSet));
	
case 'playlist':
	$playlistType = 'vod';		// playlist needs to be returned as vod
	break;
	
case 'live':
	$firstClipTime = $time * 1000 - floor($sumDurations / 2);
	$roundTo = floor($sumDurations / 4);
	$firstClipTime = floor($firstClipTime / $roundTo) * $roundTo;
	if ($params['disc'] == 'gap')
	{
		$curTime = $firstClipTime;
		$clipTimes = array();
		foreach ($durations as $duration)
		{
			$clipTimes[] = $curTime;
			$curTime += $duration + 23456;
		}
		$mediaSet["clipTimes"] = $clipTimes;
	}

	$mediaSet["firstClipTime"] = $firstClipTime;
	
	if ($discontinuity)
	{	
		if ($params['ici'] == 'yes')
		{
			$mediaSet["initialClipIndex"] = 123;
		}
		$mediaSet["initialSegmentIndex"] = 234;
	}
	
	if ($params['sbt'] == 'yes')
	{
		$mediaSet["segmentBaseTime"] = 733038085;
	}
	
	$mediaSet["liveWindowDuration"] = intval($params['window']) * 1000;
	$mediaSet["presentationEndTime"] = $time * 1000 + ($params['pet'] == 'past' ? -1000000 : 1000000);
	break;
}

$sequence = array(
	"clips" => getSourceClips($filePaths, $durations, $params['kf'] == 'yes')
);

// generate the result object
$mediaSet = array_merge($mediaSet, array(
	"discontinuity" => $discontinuity,
	"playlistType" => $playlistType,
	"durations" => $durations,
	"sequences" => array($sequence),
	"consistentSequenceMediaInfo" => $params['csmi'] == 'yes',
));

echo json_encode($mediaSet);
