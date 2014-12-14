<?php

if ($argc != 2)
{
	die("Usage:\n\t".basename(__file__)." <vod status url>\nthe url is usually 'http://server-name/vod_status'\n");
}

// get the status xml
$status = file_get_contents($argv[1]);

// convert the xml to array
$xml = @simplexml_load_string($status);
$json = json_encode($xml);
$array = json_decode($json, true);

// print the result
$pc = $array["performance_counters"];

echo "name,sum,count,max,max_time\n";
foreach ($pc as $key => $row)
{
        echo $key;
        foreach ($row as $cell)
        {
                echo ",".$cell;
        }
        echo "\n";
}
