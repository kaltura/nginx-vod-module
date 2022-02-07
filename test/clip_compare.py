from __future__ import print_function
import http_utils
import time

# note: run once with moov cache enabled and once with moov cache disabled
# 	it's recommended to disable debug logs since this test generates a lot of log lines

'''
nginx.conf

	location /local/content/ {
			vod none;
			vod_mode local;
			alias /path/to/mp4/files/;
	}

	location /remote/content/ {
			vod none;
			vod_mode remote;
			vod_upstream backend;
	}

	location /local/mp4/content {
			mp4;
			alias /path/to/mp4/files/;
	}

the following changes to ngx_http_mp4_module are required for a successful comparison

in ngx_http_mp4_handler replace
            ngx_set_errno(0);
            start = (int) (strtod((char *) value.data, NULL) * 1000);

            if (ngx_errno != 0) {
                start = -1;
            }

with
			start = ngx_atofp(value.data, value.len, 3);
			
in ngx_http_mp4_handler replace
            ngx_set_errno(0);
            end = (int) (strtod((char *) value.data, NULL) * 1000);

            if (ngx_errno != 0) {
                end = -1;
            }
			
with
			end = ngx_atofp(value.data, value.len, 3);

in ngx_http_mp4_update_mdat_atom replace
    atom_data_size = end_offset - start_offset;
    mp4->mdat_data.buf->file_pos = start_offset;
    mp4->mdat_data.buf->file_last = end_offset;
	 
with
    if (start_offset >= end_offset)
    {
        atom_data_size = 0;
        mp4->mdat_data.buf->in_file = 0;
    }
    else
    {
		atom_data_size = end_offset - start_offset;
		mp4->mdat_data.buf->file_pos = start_offset;
		mp4->mdat_data.buf->file_last = end_offset;
    }

in ngx_http_mp4_update_stts_atom after
    if (ngx_http_mp4_crop_stts_data(mp4, trak, 0) != NGX_OK) {
        return NGX_ERROR;
    }

add
    if (trak->start_sample >= trak->end_sample)
        return NGX_ERROR;

in ngx_http_mp4_crop_stsc_data replace
    uint32_t               start_sample, chunk, samples, id, next_chunk, n,
                           prev_samples;

with
    uint32_t               start_sample, chunk, samples = 0, id, next_chunk, n,
                           prev_samples;
						   
in ngx_http_mp4_crop_stsc_data replace
    chunk = ngx_mp4_get_32value(entry->chunk);
    samples = ngx_mp4_get_32value(entry->samples);
    id = ngx_mp4_get_32value(entry->id);
    prev_samples = 0;

with
    prev_samples = samples;

    chunk = ngx_mp4_get_32value(entry->chunk);
    samples = ngx_mp4_get_32value(entry->samples);
    id = ngx_mp4_get_32value(entry->id);

'''

FILE_NAME = 'b.mp4'
FILE_DURATION = 728000
FILE_BASES = ['remote', 'local']

URL1_FORMAT = {
	'prefix': 'http://localhost:8001/{fileBase}/content/',
	'start': 'clipFrom/%d%03d/',
	'end': 'clipTo/%d%03d/',
	'suffix': FILE_NAME,
	'noEndSupport': True,
}

URL2_FORMAT = {
	'prefix': 'http://localhost:8001/local/mp4/content/%s?' % FILE_NAME,
	'start': 'start=%d.%03d&',
	'end': 'end=%d.%03d&',
	'suffix': '',
	'noEndSupport': False,
}

TEST_SUITES = [
	{
		'min': 0,
		'max': 5000,
		'step': 25,
		'testNoEnd': False,
	},
	{
		'min': FILE_DURATION - 5000,
		'max': FILE_DURATION,
		'step': 25,
		'testNoEnd': True,
	},
]

def buildUrl(urlFormat, fileBase, start, end):
	if not urlFormat['noEndSupport'] and end <= 0:
		end = 100000000
	result = urlFormat['prefix']
	if start > 0:
		result += urlFormat['start'] % (start / 1000, start % 1000)
	if end > 0:
		result += urlFormat['end'] % (end / 1000, end % 1000)
	result += urlFormat['suffix']
	return result.replace('{fileBase}', fileBase)

def getUrl(url):
	startTime = time.time()
	code, headers, body = http_utils.getUrl(url)
	print ('Info: get %s took %s' % (url, time.time() - startTime))
	if code == 0:
		print(body)
	return (code, body)

def runSingleTest(fileBase, start, end):
	if start == 0 and end == 0:
		return
	url1 = buildUrl(URL1_FORMAT, fileBase, start, end)
	url2 = buildUrl(URL2_FORMAT, fileBase, start, end)
	print('curling %s' % url1)
	code1, data1 = getUrl(url1)
	print('curling %s' % url2)
	code2, data2 = getUrl(url2)
	if code1 != code2:
		if set([code1, code2]) == set([400, 500]):
			return
		print('Error: different codes %s %s' % (code1, code2))
		return
	if data1 != data2:
		print('Error: %s %s' % (url1, url2))

def runTestSuite(fileBase, testCase):
	start = testCase['min']
	while start < testCase['max']:
		end = start
		while end < testCase['max']:
			if testCase['testNoEnd'] and start == end:
				runSingleTest(fileBase, start, 0)
			else:
				runSingleTest(fileBase, start, end)
			end += testCase['step']
		start += testCase['step']

def runTestSuites(fileBase):
	for testCase in TEST_SUITES:
		runTestSuite(fileBase, testCase)

for fileBase in FILE_BASES:
	runTestSuites(fileBase)
