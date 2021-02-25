import email.utils as eut
import time

INGORED_HEADERS = set([
	'x-vod-me','x-vod-session',
	'x-proxy-me', 'x-proxy-session',
	'x-me', 'x-kaltura-session', 
	'x-varnish', 'x-amz-id-2', 'x-amz-request-id',
])

IGNORE_HEADER_VALUES = set([
	'server',
	'date',
	'content-length',		# ignore content length since we compare the buffer, the content length may be different due to different host name lengths
	'etag',					# derived from content length
])

def parseHttpTime(timeStr):
	return time.mktime(eut.parsedate(timeStr))
	
def compareHeaders(headers1, headers2):
	for headerName in INGORED_HEADERS:
		headers1.pop(headerName, None)
		headers2.pop(headerName, None)
	
	onlyIn1 = set(headers1.keys()) - set(headers2.keys())
	if len(onlyIn1) != 0:
		return 'Error: headers %s found only in headers1' % (','.join(onlyIn1))
	onlyIn2 = set(headers2.keys()) - set(headers1.keys())
	if len(onlyIn2) != 0 and onlyIn2 != set(['access-control-allow-origin']):		# allow CORS to be added
		return 'Error: headers %s found only in headers2' % (','.join(onlyIn2))
	for curHeader in headers1.keys():
		if curHeader in IGNORE_HEADER_VALUES:
			continue
		value1 = headers1[curHeader]
		value2 = headers2[curHeader]
		if value1 == value2:
			continue
		if curHeader in set(['expires', 'last-modified']):
			if abs(parseHttpTime(value1[0]) - parseHttpTime(value2[0])) < 10:
				continue
		return 'Error: different value for header %s - %s vs %s' % (curHeader, value1, value2)
	return None
