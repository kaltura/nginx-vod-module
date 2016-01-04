<?php

/*
	This script builds JSON playlist definitions for nginx-vod-module.
	When playlist type is set to live, it will play the MP4 files one after the other in a loop.
	When playlist type is vod, it will play each MP4 file once.
	The discontinuity parameter is true by default, set it to false only when all MP4's have 
	exactly the same encoding parameters (in the case of h264, the SPS/PPS have to be the same).
*/

// input params
$playlistType = "live";			// live / vod
$discontinuity = true;
$filePaths = array(
	"/web/content/r71v1/entry/data/241/803/0_p7y00o7h_0_93j6yqit_1.mp4",
	"/web/content/r70v1/entry/data/138/192/1_46gm1o7f_1_22any1sp_1.mp4",
	"/web/content/r70v1/entry/data/138/192/1_5bik2rp6_1_kg6dq8kb_1.mp4",
	);
$ffprobeBin = '/web/content/shared/bin/ffmpeg-2.7.2-bin/ffprobe.sh';

// nginx conf params
$segmentDuration = 10000;
$segmentCount = 10;

function getSourceClip($filePath)
{
	return array(
		"type" => "source",
		"path" => $filePath
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
	$commandLine = "{$ffprobeBin} -i {$filePath} -show_format -print_format json -v quiet";
	$output = null;
	exec($commandLine, $output);
	
	// parse the result
	$ffmpegResult = json_decode(implode("\n", $output));
	$duration = floatval($ffmpegResult->format->duration);
	$duration = intval($duration * 1000);
	
	// store to cache
	apc_store($cacheKey,, $duration);
	
	return $duration;
}

$durations = array_map('getDurationMillis', $filePaths);

$result = array();

if ($playlistType == "live")
{
	// find the duration of each cycle
	$cycleDuration = array_sum($durations);
	
	// start the playlist from now() - <dvr window size>
	$currentTime = (time() - 10) * 1000 - $segmentDuration * $segmentCount;
	
	// set the first clip time to now() rounded down to cycle duration
	$result["firstClipTime"] = floor($currentTime / $cycleDuration) * $cycleDuration;
	
	// get the reference time (the first clip time of the first run)
	$referenceTime = apc_fetch('reference_time');
	if (!$referenceTime)
	{
		$referenceTime = $result["firstClipTime"];
		apc_store('reference_time', $referenceTime);
	}

	if ($discontinuity)
	{
		// find the total number of segments in each cycle
		$totalSegmentCount = 0;
		foreach ($durations as $duration)
		{
			$totalSegmentCount += ceil($duration / $segmentDuration);
		}

		// find the initial clip index and initial segment index
		$cycleIndex = floor(($currentTime - $referenceTime) / $cycleDuration);
		$result["initialClipIndex"] = $cycleIndex * count($durations) + 1;
		$result["initialSegmentIndex"] = $cycleIndex * $totalSegmentCount + 1;
	}
	else
	{
		$result["segmentBaseTime"] = $referenceTime;
	}

	// duplicate the clips, this is required so that we won't run out of segments 
	// close to the end of a cycle
	$durations = array_merge($durations, $durations);
	$filePaths = array_merge($filePaths, $filePaths);
}

// generate the result object
$result = array_merge($result, array(
	"discontinuity" => $discontinuity,
	"playlistType" => $playlistType,
	"durations" => $durations,
	"sequences" => array(
		array(
			"clips" => array_map('getSourceClip', $filePaths)
		)
	)
));

echo json_encode($result);
