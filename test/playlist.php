<?php

/*
	This script builds JSON playlist definitions for nginx-vod-module.
	When playlist type is set to live, it will play the MP4 files one after the other in a loop.
	When playlist type is vod, it will play each MP4 file once.
	The discontinuity parameter is true by default, set it to false only when all MP4's have 
	exactly the same encoding parameters (in the case of h264, the SPS/PPS have to be the same).
*/

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

$params = getRequestParams();
$discontinuity = (isset($params['disc']) && $params['disc']) ? true : false;
$playlistType = isset($params['type']) ? $params['type'] : 'live';

// input params
$filePaths = array(
	// clip 1
	array(
		'/path/to/video1.mp4',
		'/path/to/subtitles1.srt',
	),
	// clip 2
	array(
		'/path/to/video2.mp4',
		'/path/to/subtitles2.srt',
	),
);

$languages = array(
	null,
	'eng',
);

if (!$discontinuity)
{
	$filePaths = array($filePaths[0], $filePaths[0]);
}
$ffprobeBin = '/web/content/shared/bin/ffmpeg-2.7.2-bin/ffprobe.sh';
$timeMargin = 10000;		// a safety margin to compensate for clock differences

// nginx conf params
$segmentDuration = 10000;
$segmentCount = 10;

function getSourceClip($filePath)
{
	return array(
		'type' => 'source',
		'path' => $filePath
	);
}

function getDurationMillis($filePath)
{
	global $ffprobeBin;

	// try to fetch from cache
	$cacheKey = "duration-{$filePath}";
	$duration = apc_fetch($cacheKey);
	if ($duration)
	{
		return $duration;
	}

	// execute ffprobe
	$commandLine = "{$ffprobeBin} -i {$filePath} -show_streams -print_format json -v quiet";
	$output = null;
	exec($commandLine, $output);

	// parse the result - take the shortest duration
	$ffmpegResult = json_decode(implode("\n", $output));
	$duration = null;
	foreach ($ffmpegResult->streams as $stream)
	{
		$curDuration = floor(floatval($stream->duration) * 1000);
		if (is_null($duration) || $curDuration < $duration)
		{
			$duration = $curDuration;
		}
	}
	if (!$duration)
	{
		die("Failed to get the duration of {$filePath}\n");
	}

	// store to cache
	apc_store($cacheKey, $duration);

	return $duration;
}

$durations = array();
foreach ($filePaths as $curClip)
{
	$durations[] = getDurationMillis(reset($curClip));
}

$result = array();

if ($playlistType == 'live')
{
	// get cycle info
	$cycleDurations = $durations;
	$cycleFilePaths = $filePaths;
	$cycleClipCount = count($cycleDurations);
	$cycleDuration = array_sum($cycleDurations);
	$cycleSegmentCount = 0;
	foreach ($cycleDurations as $duration)
	{
		$cycleSegmentCount += ceil($duration / $segmentDuration);
	}

	// find the start / end time
	$dvrWindowSize = $segmentDuration * $segmentCount;
	$endTime = (isset($params['time']) ? $params['time'] : time()) * 1000;
	$startTime = $endTime - $timeMargin - $dvrWindowSize;

	// get the reference time (start time of the first run)
	$referenceTime = apc_fetch('reference_time');
	if (!$referenceTime)
	{
		$referenceTime = $startTime;
		apc_store('reference_time', $referenceTime);
	}

	// get the initial cycle info
	$cycleIndex = floor(($startTime - $referenceTime) / $cycleDuration);
	$clipIndex = $cycleIndex * $cycleClipCount;
	$segmentIndex = $cycleIndex * $cycleSegmentCount;
	$durations = array();
	$filePaths = array();

	$currentTime = $referenceTime + $cycleIndex * $cycleDuration;
	while ($currentTime < $endTime)
	{
		// get the current clip duration
		$cycleClipIndex = $clipIndex % $cycleClipCount;
		$duration = $cycleDurations[$cycleClipIndex];
		if ($currentTime + $duration > $startTime)
		{
			if (!$durations)
			{
				// set first clip details
				$result['firstClipTime'] = $currentTime;

				if ($discontinuity)
				{
					$result['initialClipIndex'] = $clipIndex + 1;
					$result['initialSegmentIndex'] = $segmentIndex + 1;
				}
			}

			// add the current clip
			$durations[] = $duration;
			$filePaths[] = $cycleFilePaths[$cycleClipIndex];
		}

		// move to the next clip
		$clipIndex++;
		$segmentIndex += ceil($duration / $segmentDuration);
		$currentTime += $duration;
	}

	if (!$discontinuity)
	{
		$result['segmentBaseTime'] = $referenceTime;
	}
}

$sequences = array();
$seqCount = count(reset($filePaths));
for ($seqIndex = 0; $seqIndex < $seqCount; $seqIndex++)
{
	$clips = array();
	foreach ($filePaths as $curClip)
	{
		$clips[] = getSourceClip($curClip[$seqIndex]);
	}

	$sequence = array(
		'clips' => $clips
	);

	if (isset($languages[$seqIndex]))
	{
		$language = $languages[$seqIndex];
		if ($language)
		{
			$sequence['language'] = $language;
		}
	}

	$sequences[] = $sequence;
}

// generate the result object
$result = array_merge($result, array(
	'discontinuity' => $discontinuity,
	'playlistType' => $playlistType,
	'durations' => $durations,
	'sequences' => $sequences,
));

echo json_encode($result);
