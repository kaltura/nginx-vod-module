from __future__ import print_function
from xml.dom.minidom import parseString
import http_utils
import calendar
import struct
import base64
import math
import time

try:
	xrange  # Python 2
except NameError:
	xrange = range  # Python 3

CHUNK_LIST_ITEMS_TO_COMPARE = 0

#filter array by taking evenly spaced elements must must include first and last
def filterChunkList(arr):
	n = len(arr) - 2
	m = CHUNK_LIST_ITEMS_TO_COMPARE - 2
	if CHUNK_LIST_ITEMS_TO_COMPARE > 0 and n > m:
		print("Taking only %d segments from the %d segments available" % (CHUNK_LIST_ITEMS_TO_COMPARE, len(arr)))
		#Bresenham's line algorithm, but take first and last as well
		return arr[0:1]+[arr[1 + i * n // m + len(arr) // (2 * m)] for i in range(m)] + [arr[-1]]
	return arr

def getAttributesDict(node):
	result = {}
	for attIndex in xrange(len(node.attributes)):
		curAtt = node.attributes.item(attIndex)
		result[curAtt.name] = curAtt.value
	return result

def getAbsoluteUrl(url, baseUrl = ''):
	if not url.startswith('http://') and not url.startswith('https://'):
		if baseUrl == '':
			raise Exception('bad url %s' % url)
		queryPos = baseUrl.find('?')
		if queryPos >= 0:
			baseUrl = baseUrl[:queryPos]

		baseUrl = baseUrl[:baseUrl.rfind('/')]
		url = baseUrl + '/' + url
	return url
	
def getHlsMediaPlaylistUrls(baseUrl, urlContent):
	result = []
	for curLine in urlContent.split('\n'):
		curLine = curLine.strip()
		if len(curLine) == 0:
			continue
		if curLine[0] == '#':
			spilttedLine = curLine.split('URI="', 1)
			if len(spilttedLine) < 2:
				continue
			result.append(getAbsoluteUrl(spilttedLine[1].split('"')[0], baseUrl))
			continue
		result.append(getAbsoluteUrl(curLine, baseUrl))
	result = filterChunkList(result)
	return result
	
def getHlsMasterPlaylistUrls(baseUrl, urlContent, headers):
	result = []
	for curLine in urlContent.split('\n'):
		curLine = curLine.strip()
		if len(curLine) == 0:
			continue
		# get the current url
		if curLine[0] == '#':
			if not 'URI="' in curLine:
				continue
			curUrl = curLine.split('URI="')[1].split('"')[0]
		else:
			curUrl = curLine
		curUrl = getAbsoluteUrl(curUrl, baseUrl)
		result.append(curUrl)
		
		# get the segments of the current url
		code, _, mediaContent = http_utils.getUrl(curUrl, headers)
		if code != 200 or len(mediaContent) == 0:
			continue
		curBaseUrl = curUrl.rsplit('/', 1)[0] + '/'
		result += getHlsMediaPlaylistUrls(curBaseUrl, mediaContent)
	return result

def parseDashInterval(ts):
	if not ts.startswith('PT'):
		return 0
	ts = ts[2:]
	result = 0
	while len(ts) > 0:
		mul = None
		for pos in xrange(len(ts)):
			if ts[pos] == 'H':
				mul = 3600
			elif ts[pos] == 'M':
				mul = 60
			elif ts[pos] == 'S':
				mul = 1
			if mul != None:
				break
		if pos <= 0 or pos >= len(ts):
			return 0
		value = float(ts[:pos])
		result += value * mul * 1000
		ts = ts[(pos + 1):]
	return int(result)

def parseDashDate(date):
	return int(calendar.timegm(time.strptime(date,'%Y-%m-%dT%H:%M:%SZ')) * 1000)

def getDashManifestUrls(baseUrl, urlContent, headers):
	parsed = parseString(urlContent)

	# try SegmentList
	urls = set([])
	for node in parsed.getElementsByTagName('SegmentList'):
		for childNode in node.getElementsByTagName('SegmentURL'):
			atts = getAttributesDict(childNode)
			urls.add(atts['media'])
		for childNode in node.getElementsByTagName('Initialization'):
			atts = getAttributesDict(childNode)
			urls.add(atts['sourceURL'])
	if len(urls) > 0:
		return map(lambda x: getAbsoluteUrl(x, baseUrl), urls)
	
	# try SegmentTemplate - get media duration
	mediaDuration = None
	timeShiftBufferDepth = None
	availabilityStartTime = None
	publishTime = None
	for node in parsed.getElementsByTagName('MPD'):
		atts = getAttributesDict(node)
		if atts.has_key('mediaPresentationDuration'):
			mediaDuration = parseDashInterval(atts['mediaPresentationDuration'])
		if atts.has_key('timeShiftBufferDepth'):
			timeShiftBufferDepth = parseDashInterval(atts['timeShiftBufferDepth'])
		if atts.has_key('availabilityStartTime'):
			availabilityStartTime = parseDashDate(atts['availabilityStartTime'])
		if atts.has_key('publishTime'):
			publishTime = parseDashDate(atts['publishTime'])

	# get the period times
	periodTimes = []
	for period in parsed.getElementsByTagName('Period'):
		atts = getAttributesDict(period)
		if not atts.has_key('id') or not atts.has_key('start'):
			continue
		start = parseDashInterval(atts['start'])
		id = atts['id']
		periodTimes.append((id, start))
	
	# get the url templates and segment duration
	result = []
	for base in parsed.getElementsByTagName('AdaptationSet'):
	
		initUrls = set([])
		mediaUrls = set([])

		segmentDuration = None
		startNumber = 0
		for node in base.getElementsByTagName('SegmentTemplate'):
			atts = getAttributesDict(node)
			initUrls.add(atts['initialization'])
			mediaUrls.add(atts['media'])
			if atts.has_key('duration'):
				segmentDuration = int(atts['duration'])
			if atts.has_key('startNumber'):
				startNumber = int(atts['startNumber']) - 1

		# get the representation ids
		repIds = set([])
		for node in base.getElementsByTagName('Representation'):
			atts = getAttributesDict(node)
			repIds.add((atts['id'], atts['bandwidth']))
			
		# get the segment count from SegmentTimeline
		segmentCount = None
		segmentTimes = []
		curTime = 0
		for node in base.getElementsByTagName('SegmentTimeline'):
			segmentCount = 0
			for childNode in node.childNodes:
				if childNode.nodeType == node.ELEMENT_NODE and childNode.nodeName == 'S':
					atts = getAttributesDict(childNode)
					repeatCount = 1
					if atts.has_key('r'):
						repeatCount += int(atts['r'])
					if atts.has_key('t'):
						curTime = int(atts['t'])
					if atts.has_key('d'):
						duration = int(atts['d'])
						for _ in xrange(repeatCount):
							segmentTimes.append(curTime)
							curTime += duration
					segmentCount += repeatCount

		if segmentCount == None:
			if segmentDuration == None:
				for curBaseUrl in base.getElementsByTagName('BaseURL'):
					result.append(getAbsoluteUrl(curBaseUrl.firstChild.nodeValue))
				continue
			period = base.parentNode
			periodAtts = getAttributesDict(period)

			# period start time
			periodStartTime = 0
			if periodAtts.has_key('start'):
				periodStartTime = parseDashInterval(periodAtts['start'])

			# period end time
			if periodAtts.has_key('duration'):
				periodEndTime = periodStartTime + parseDashInterval(periodAtts['duration'])
			elif mediaDuration != None:
				periodEndTime = periodStartTime + mediaDuration
			else:
				# derive the duration from the diff in start time
				periodId = getAttributesDict(period)['id']
				for index in xrange(len(periodTimes)):
					if periodTimes[index][0] == periodId:
						break
				if index + 1 < len(periodTimes):
					periodEndTime = periodTimes[index + 1][1]
				else:
					periodEndTime = publishTime - availabilityStartTime

			# window start time
			startTime = periodStartTime
			if startTime == 0 and timeShiftBufferDepth != None:		# continuous live
				startTime = periodEndTime - timeShiftBufferDepth

			# find the segment number / count
			startNumber += (startTime - periodStartTime + segmentDuration - 1) / segmentDuration
			segmentCount = (periodEndTime - periodStartTime) / segmentDuration - startNumber
		
		for url in initUrls:
			for repId, bandwidth in repIds:
				curUrl = url.replace('$RepresentationID$', repId)
				curUrl = curUrl.replace('$Bandwidth$', bandwidth)
				result.append(getAbsoluteUrl(curUrl, baseUrl))
		for url in mediaUrls:
			for curSeg in xrange(segmentCount):
				for repId, bandwidth in repIds:
					curUrl = url.replace('$Number$', '%s' % (startNumber + curSeg + 1))
					if len(segmentTimes) > 0:
						curUrl = curUrl.replace('$Time$', '%s' % (segmentTimes[curSeg]))
					curUrl = curUrl.replace('$RepresentationID$', repId)
					curUrl = curUrl.replace('$Bandwidth$', bandwidth)
					result.append(getAbsoluteUrl(curUrl, baseUrl))

	result = filterChunkList(result)
	return result

def getHdsSegmentIndexes(data):
	result = []
	mediaTime = struct.unpack('>Q', data[0x15:0x1d])[0]
	curPos = 0x5a
	prevDuration = 0
	while curPos < len(data):
		index, timestamp, duration = struct.unpack('>LQL', data[curPos:(curPos + 0x10)])
		curPos += 0x10
		if duration == 0:
			curPos += 1

		if prevDuration != 0:
			if (index, timestamp, duration) == (0, 0, 0):
				repeatCount = (mediaTime - prevTimestamp) / prevDuration
			else:
				repeatCount = index - prevIndex
			result += range(prevIndex, prevIndex + repeatCount)
		(prevIndex, prevTimestamp, prevDuration) = (index, timestamp, duration)

	if prevDuration != 0 and mediaTime > prevTimestamp:
		repeatCount = (mediaTime - prevTimestamp) / prevDuration
		result += range(prevIndex, prevIndex + repeatCount)
	return result
	
def getHdsManifestUrls(baseUrl, urlContent, headers):
	result = []
	
	# parse the xml
	parsed = parseString(urlContent)
	
	# get the bootstraps
	segmentIndexes = {}
	for node in parsed.getElementsByTagName('bootstrapInfo'):
		atts = getAttributesDict(node)
		if atts.has_key('url'):
			curUrl = getAbsoluteUrl(atts['url'], baseUrl)
			result.append(curUrl)
			
			# get the bootstrap info
			code, _, bootstrapInfo = http_utils.getUrl(curUrl, headers)
			if code != 200 or len(bootstrapInfo) == 0:
				continue
		else:
			bootstrapInfo = base64.b64decode(node.firstChild.nodeValue)
		bootstrapId = atts['id']
		segmentIndexes[bootstrapId] = getHdsSegmentIndexes(bootstrapInfo)
	
	# add the media urls
	for node in parsed.getElementsByTagName('media'):
		atts = getAttributesDict(node)
		bootstrapId = atts['bootstrapInfoId']
		if not segmentIndexes.has_key(bootstrapId):
			continue
		
		url = atts['url']
		url = url.split('?')[0]
		fragments = []
		for curSeg in segmentIndexes[bootstrapId]:
			fragments.append(getAbsoluteUrl('%s/%sSeg1-Frag%s' % (baseUrl, url, curSeg)))

		result += filterChunkList(fragments)

	return result

def getMssManifestUrls(baseUrl, urlContent, headers):
	result = []
	parsed = parseString(urlContent)
	for node in parsed.getElementsByTagName('StreamIndex'):		
		# get the bitrates
		bitrates = set([])
		for childNode in node.getElementsByTagName('QualityLevel'):
			bitrates.add(getAttributesDict(childNode)['Bitrate'])
		
		# get the timestamps
		timestamps = []
		curTimestamp = 0
		for childNode in node.getElementsByTagName('c'):
			curAtts = getAttributesDict(childNode)
			if curAtts.has_key('t'):
				curTimestamp = int(curAtts['t'])
			duration = int(curAtts['d'])
			timestamps.append('%s' % curTimestamp)
			curTimestamp += duration
			
		# build the final urls
		atts = getAttributesDict(node)
		url = atts['Url']
		for bitrate in bitrates:
			for timestamp in timestamps:
				result.append(getAbsoluteUrl('%s/%s' % (baseUrl, url.replace('{bitrate}', bitrate).replace('{start time}', timestamp))))

	result = filterChunkList(result)
	return result

PARSER_BY_MIME_TYPE = {
	'application/dash+xml': getDashManifestUrls,
	'video/f4m': getHdsManifestUrls,
	'application/vnd.apple.mpegurl': getHlsMasterPlaylistUrls,
	'application/x-mpegurl': getHlsMasterPlaylistUrls,
	'text/xml': getMssManifestUrls,
}

def getManifestUrls(baseUrl, urlContent, mimeType, headers):
	if not PARSER_BY_MIME_TYPE.has_key(mimeType):
		return []
	return PARSER_BY_MIME_TYPE[mimeType](baseUrl, urlContent, headers)
