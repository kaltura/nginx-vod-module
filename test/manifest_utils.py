from xml.dom.minidom import parseString
import http_utils
import struct
import base64
import math

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
		if baseUrl.endswith('/'):
			url = baseUrl + url
		else:
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
		curBaseUrl = curUrl.rsplit('/', 1)[0]
		result += getHlsMediaPlaylistUrls(curBaseUrl, mediaContent)
	return result

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
	for node in parsed.getElementsByTagName('MPD'):
		atts = getAttributesDict(node)
		mediaDuration = float(atts['mediaPresentationDuration'][2:-1])
	
	# get the url templates and segment duration
	segmentDuration = None
	for node in parsed.getElementsByTagName('SegmentTemplate'):
		atts = getAttributesDict(node)
		urls.add(atts['media'])
		urls.add(atts['initialization'])
		if atts.has_key('duration'):
			segmentDuration = int(atts['duration'])

	# get the representation ids
	repIds = set([])
	for node in parsed.getElementsByTagName('Representation'):
		atts = getAttributesDict(node)
		repIds.add(atts['id'])
		
	# get the segment count from SegmentTimeline
	segmentCount = None
	for node in parsed.getElementsByTagName('SegmentTimeline'):
		segmentCount = 0
		for childNode in node.childNodes:
			if childNode.nodeType == node.ELEMENT_NODE and childNode.nodeName == 'S':
				atts = getAttributesDict(childNode)
				if atts.has_key('r'):
					segmentCount += int(atts['r'])
				segmentCount += 1

	if segmentCount == None:
		segmentCount = int(math.ceil(mediaDuration * 1000 / segmentDuration))
	
	result = []
	for url in urls:
		for curSeg in xrange(segmentCount):
			for repId in repIds:
				result.append(getAbsoluteUrl(url.replace('$Number$', '%s' % (curSeg + 1)).replace('$RepresentationID$', repId)))
	return result

def getHdsManifestUrls(baseUrl, urlContent, headers):
	parsed = parseString(urlContent)

	# get the media base urls
	urls = set([])
	for node in parsed.getElementsByTagName('media'):
		atts = getAttributesDict(node)
		urls.add(atts['url'])

	# get the bootstrap info
	segmentCount = None
	for node in parsed.getElementsByTagName('bootstrapInfo'):
		bootstrapInfo = base64.b64decode(node.firstChild.nodeValue)
		segmentCount = struct.unpack('>L', bootstrapInfo[0x40:0x44])[0]
	
	# generate the segment urls
	result = []
	for url in urls:
		for curSeg in xrange(segmentCount):
			result.append(getAbsoluteUrl('%s/%sSeg1-Frag%s' % (baseUrl, url, curSeg + 1)))
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
			duration = getAttributesDict(childNode)['d']
			timestamps.append('%s' % curTimestamp)
			curTimestamp += int(duration)
			
		# build the final urls
		atts = getAttributesDict(node)
		url = atts['Url']
		for bitrate in bitrates:
			for timestamp in timestamps:
				result.append(getAbsoluteUrl('%s/%s' % (baseUrl, url.replace('{bitrate}', bitrate).replace('{start time}', timestamp))))
	return result

PARSER_BY_MIME_TYPE = {
	'application/dash+xml': getDashManifestUrls,
	'video/f4m': getHdsManifestUrls,
	'application/vnd.apple.mpegurl': getHlsMasterPlaylistUrls,
	'text/xml': getMssManifestUrls,
}

def getManifestUrls(baseUrl, urlContent, mimeType, headers):
	if not PARSER_BY_MIME_TYPE.has_key(mimeType):
		return []
	return PARSER_BY_MIME_TYPE[mimeType](baseUrl, urlContent, headers)
