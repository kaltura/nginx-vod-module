from __future__ import print_function
from mpeg_ts_defs import *
import manifest_utils
import mp4_utils
import urllib2
import struct
import sys

SEGMENT_DURATION = 100000000
THRESHOLD = 10
URL = sys.argv[1]

START_PTS = 'startPts'
START_DTS = 'startDts'
END_PTS = 'endPts'
END_DTS = 'endDts'

def getTimingInfoFromTrunAtom(trunAtom, startPts):
	trunFlags = struct.unpack('>L', trunAtom[:4])[0]
	durations = []
	ptsDelays = []
	
	if trunFlags == 0x01000f01:
		for pos in xrange(12, len(trunAtom), 16):
			durations.append(struct.unpack('>L', trunAtom[pos:(pos + 4)])[0])
			ptsDelays.append(struct.unpack('>l', trunAtom[(pos + 12):(pos + 16)])[0])
	elif trunFlags == 0xf01:
		for pos in xrange(12, len(trunAtom), 16):
			durations.append(struct.unpack('>L', trunAtom[pos:(pos + 4)])[0])
			ptsDelays.append(struct.unpack('>L', trunAtom[(pos + 12):(pos + 16)])[0])
	elif trunFlags == 0x301:
		for pos in xrange(12, len(trunAtom), 8):
			durations.append(struct.unpack('>L', trunAtom[pos:(pos + 4)])[0])
			ptsDelays.append(0)
	else:
		return False
	curDts = startPts - ptsDelays[0]
	endPts = startPts
	for duration, ptsDelay in zip(durations, ptsDelays):
		curDts += duration
		endPts = max(endPts, curDts + ptsDelay)
	return {
		START_PTS: startPts, 
		START_DTS: startPts - ptsDelays[0],
		END_PTS: endPts,
		END_DTS: startPts - ptsDelays[0] + sum(durations),
		}

def getDashFragmentInfo(url):
	req = urllib2.Request(url, headers={'Range': 'bytes=0-65535'})
	d = urllib2.urlopen(req).read()
	atoms = mp4_utils.parseAtoms(d, 0, len(d))
	sidxAtom = mp4_utils.getAtomData(d, atoms, 'sidx')
	if sidxAtom[0] == '\0':
		startPts = struct.unpack('>L', sidxAtom[12:16])[0]
	else:
		startPts = struct.unpack('>Q', sidxAtom[12:20])[0]
		
	trunAtom = mp4_utils.getAtomData(d, atoms, 'moof.traf.trun')
	timingInfo = getTimingInfoFromTrunAtom(trunAtom, startPts)

	urlInfo = url.split('fragment-')[-1]
	segIndex, fileIndex, streamId = urlInfo.split('-')[:3]
	return [(url, int(segIndex), streamId, fileIndex, timingInfo)]

def getDashFragmentsInfo(urls):
	result = []
	for url in urls:
		urlPath = url.split('?')[0]
		if not urlPath.endswith('.m4s'):
			continue
		print('.', end=' ')
		result += getDashFragmentInfo(url)
	return result

def parse24be(buf):
	return struct.unpack('>L', '\0' + buf)[0]
		
def getFragmentInfoFromDtssPtss(url, segIndex, fileIndex, dtss, ptss, audioPacketsCount = None):
	result = []
	for streamId in dtss:
		if len(dtss[streamId]) == 1:
			lastDuration = 0
		elif audioPacketsCount == None or streamId != 'a1':
			lastDuration = (dtss[streamId][-1] - dtss[streamId][0]) / (len(dtss[streamId]) - 1)
		else:
			avgDuration = (dtss[streamId][-1] - dtss[streamId][0]) / sum(audioPacketsCount[:-1])
			lastDuration = audioPacketsCount[-1] * avgDuration

		timingInfo = {
			START_PTS: ptss[streamId][0], 
			START_DTS: dtss[streamId][0],
			END_DTS: dtss[streamId][-1] + lastDuration,
			END_PTS: max(ptss[streamId]) + lastDuration,
			}
		result.append((url, int(segIndex), streamId, fileIndex, timingInfo))
	return result

def getHdsFragmentInfo(url):
	req = urllib2.Request(url, headers=headers)
	d = urllib2.urlopen(req).read()
	atoms = mp4_utils.parseAtoms(d, 0, len(d))
	mdatAtom = mp4_utils.getAtomData(d, atoms, 'mdat')
	curPos = 0
	dtss = {}
	ptss = {}
	while curPos < len(mdatAtom):
		# parse the adobe mux packet header
		tagType = ord(mdatAtom[curPos])
		dataSize = parse24be(mdatAtom[(curPos + 1):(curPos + 4)])
		dts = parse24be(mdatAtom[(curPos + 4):(curPos + 7)])
		dtsExt = ord(mdatAtom[curPos + 7])
		dts |= dtsExt << 24
		if tagType == 9:
			streamId = 'v1'
			ptsDelay = parse24be(mdatAtom[(curPos + 13):(curPos + 16)])
			realFrame = ord(mdatAtom[curPos + 12]) == 1
		elif tagType == 8:
			streamId = 'a1'
			ptsDelay = 0
			realFrame = ord(mdatAtom[curPos + 12]) == 1
		else:
			realFrame = False
		curPos += 11 + dataSize + 4
		if not realFrame:
			continue
		dtss.setdefault(streamId, [])
		dtss[streamId].append(dts)
		ptss.setdefault(streamId, [])
		ptss[streamId].append(dts + ptsDelay)
		
	urlFilename = url.rsplit('/', 1)[-1]
	urlFilename = urlFilename.replace('-v1', '').replace('-a1', '').replace('-Seg1-Frag', '-')
	_, fileIndex, segIndex = urlFilename.split('-')
	return getFragmentInfoFromDtssPtss(url, segIndex, fileIndex, dtss, ptss)
	
def getHdsFragmentsInfo(urls):
	result = []
	for url in urls:
		if not 'Seg1-Frag' in url:
			continue
		print('.', end=' ')
		result += getHdsFragmentInfo(url)
	return result

def countAdtsPackets(d):
	curPos = 0
	result = 0
	while curPos < len(d):
		theAdtsHeader = adtsHeader.parse(d[curPos:])
		result += 1
		curPos += theAdtsHeader.aac_frame_length
		if theAdtsHeader.aac_frame_length == 0:
			break
	return result
		
def getHlsFragmentInfo(url, fileIndex):
	req = urllib2.Request(url, headers=headers)
	d = urllib2.urlopen(req).read()
	dtss = {}
	ptss = {}
	adtsCounts = []
	audioPid = None
	audioPacket = None
	for packetPos in xrange(0, len(d), TS_PACKET_LENGTH):
		# ts header
		curPos = packetPos
		endPos = packetPos + TS_PACKET_LENGTH
		packetHeader = mpegTsHeader.parse(d[curPos:])
		curPos += mpegTsHeader.sizeof()
		
		# skip the adaptation field
		adaptationField = None
		if packetHeader.adaptationFieldExist:
			adaptationField = mpegTsAdaptationField.parse(d[curPos:])
			adaptationFieldDataPos = curPos + mpegTsAdaptationField.sizeof()
			curPos += 1 + adaptationField.adaptationFieldLength

		if not packetHeader.payloadUnitStartIndicator:
			if packetHeader.PID == audioPid:
				audioPacket += d[curPos:endPos]
			continue
			
		# pes header
		if (not d[curPos:].startswith(PES_MARKER) or
			endPos < curPos + 6 + pesOptionalHeader.sizeof()):
			continue
		thePesHeader = pesHeader.parse(d[curPos:])
		if thePesHeader.streamId == 224:
			streamId = 'v1'
		elif thePesHeader.streamId == 192:
			streamId = 'a1'
			audioPid = packetHeader.PID
			if audioPacket != None:
				adtsCounts.append(countAdtsPackets(audioPacket))
			audioPacket = ''
		else:
			continue
		curPos += 6 # pesHeader.sizeof()
		thePesOptHeader = pesOptionalHeader.parse(d[curPos:])
		curPos += pesOptionalHeader.sizeof()
		if not thePesOptHeader.ptsFlag:
			continue
		ptsStruct = pts.parse(d[curPos:])
		curPts = getPts(ptsStruct)
		curPos += pts.sizeof()
		if thePesOptHeader.dtsFlag:
			ptsStruct = pts.parse(d[curPos:])
			curDts = getPts(ptsStruct)
			curPos += pts.sizeof()
		else:
			curDts = curPts
		if packetHeader.PID == audioPid:
			audioPacket += d[curPos:endPos]
		dtss.setdefault(streamId, [])
		dtss[streamId].append(curDts)
		ptss.setdefault(streamId, [])
		ptss[streamId].append(curPts)
	if audioPacket != None:
		adtsCounts.append(countAdtsPackets(audioPacket))
	urlFilename = url.split('?')[0].rsplit('/', 1)[-1]
	urlFilename = urlFilename.replace('-v1', '').replace('-a1', '').replace('-Seg1-Frag', '-').replace('.ts', '')
	if fileIndex > 0:
		segIndex = urlFilename.split('-')[1]
	else:
		_, segIndex, fileIndex = urlFilename.split('-')
	return getFragmentInfoFromDtssPtss(url, segIndex, fileIndex, dtss, ptss, adtsCounts)

def getHlsFragmentsInfo(urls):
	result = []
	baseUrls = []
	for url in urls:
		urlPath = url.split('?')[0]
		baseUrl = url.rsplit('/', 1)[0]
		if urlPath.endswith('.m3u8') and not baseUrl.endswith('.urlset/'):
			baseUrls.append(baseUrl)
		if not urlPath.endswith('.ts'):
			continue
		print('.', end=' ')
		if baseUrl in baseUrls:
			fileIndex = baseUrls.index(baseUrl) + 1
		else:
			fileIndex = 0
		result += getHlsFragmentInfo(url, fileIndex)
	return result
		
def getMssFragmentInfo(url):
	urlInfo = url.split('QualityLevels(', 1)[-1]
	if 'video' in urlInfo:
		streamId = 'v1'
	elif 'audio' in urlInfo:
		streamId = 'a1'
	else:
		return []
	bitrate = int(urlInfo.split(')')[0])
	fileIndex = 'f%s' % ((((bitrate) >> 5) & 0x1F) + 1)
	timestamp = int(url.split('=')[-1].split(')')[0])
	segIndex = timestamp / SEGMENT_DURATION

	req = urllib2.Request(url, headers={'Range': 'bytes=0-65535'})
	d = urllib2.urlopen(req).read()
	atoms = mp4_utils.parseAtoms(d, 0, len(d))

	# get the start pts
	uuidAtoms = mp4_utils.getAtom(atoms, 'moof.traf')[3]['uuid']
	for uuidAtom in uuidAtoms:
		curAtomData = d[(uuidAtom[0] + uuidAtom[1]):uuidAtom[2]]
		if not curAtomData.startswith('6d1d9b0542d544e680e2141daff757b2'.decode('hex')):
			continue
		startPts = struct.unpack('>Q', curAtomData[20:28])[0]
	
	trunAtom = mp4_utils.getAtomData(d, atoms, 'moof.traf.trun')
	timingInfo = getTimingInfoFromTrunAtom(trunAtom, startPts)
	return [(url, int(segIndex), streamId, fileIndex, timingInfo)]
		
def getMssFragmentsInfo(urls):
	result = []
	for url in urls:
		print('.', end=' ')
		result += getMssFragmentInfo(url)
	return result
	
res = urllib2.urlopen(URL)
mimeType = res.info().getheader('Content-Type')
d = res.read()

# apply Set-Cookie headers
headers = {}
for header in res.info().headers:
	splittedHeader = header.split(':', 1)
	if splittedHeader[0] == 'Set-Cookie':
		headers['Cookie'] = splittedHeader[1].strip()
		
PARSER_BY_MIME_TYPE = {
	'application/dash+xml': getDashFragmentsInfo,
	'video/f4m': getHdsFragmentsInfo,
	'application/vnd.apple.mpegurl': getHlsFragmentsInfo,
	'application/x-mpegurl': getHlsFragmentsInfo,
	'text/xml': getMssFragmentsInfo,
}

TIMESCALE = {
	'application/dash+xml': 90000,
	'video/f4m': 1000,
	'application/vnd.apple.mpegurl': 90000,
	'application/x-mpegurl': 90000,
	'text/xml': 10000000,
}

baseUrl = URL.rsplit('/', 1)[0] + '/'
urls = manifest_utils.getManifestUrls(baseUrl, d, mimeType, headers)

print('processing %s urls' % len(urls))
fragmentInfos = PARSER_BY_MIME_TYPE[mimeType](urls)
timescale = TIMESCALE[mimeType]

print('')

# group the results
byStream = {}
bySegIndex = {}
for (url, segIndex, streamId, fileIndex, timingInfo) in fragmentInfos:
	key = '%s-%s' % (fileIndex, streamId)
	byStream.setdefault(key, {})
	byStream[key][segIndex] = (url, timingInfo)
	
	key = '%s-%s' % (segIndex, streamId)
	bySegIndex.setdefault(key, {})
	bySegIndex[key][fileIndex] = (url, timingInfo)

errors = []
	
# consistency within each stream
for streamId in byStream:
	segIndexes = sorted(byStream[streamId].keys())
	expectedDts = None
	expectedPts = None
	for segIndex in segIndexes:
		url, timingInfo = byStream[streamId][segIndex]
		
		# dts
		if expectedDts != None:
			gapSize = abs(timingInfo[START_DTS] - expectedDts)
			if gapSize > THRESHOLD:
				errors.append(['in-stream dts gap', 
					timingInfo[START_DTS],
					'size=%s (%.3f)' % (gapSize, gapSize / float(timescale)),
					'expected=%s (%.3f)' % (expectedDts, expectedDts / float(timescale)), 
					'actual=%s (%.3f)' % (timingInfo[START_DTS], timingInfo[START_DTS] / float(timescale)), 
					'stream=%s' % streamId, 
					'url=%s' % url])
		expectedDts = timingInfo[END_DTS]
		
		# pts
		if expectedPts != None:
			gapSize = abs(timingInfo[START_PTS] - expectedPts)
			if gapSize > THRESHOLD:
				errors.append(['in-stream pts gap', 
					timingInfo[START_PTS],
					'size=%s (%.3f)' % (gapSize, gapSize / float(timescale)),
					'expected=%s (%.3f)' % (expectedPts, expectedPts / float(timescale)), 
					'actual=%s (%.3f)' % (timingInfo[START_PTS], timingInfo[START_PTS] / float(timescale)), 
					'stream=%s' % streamId, 
					'url=%s' % url])
		expectedPts = timingInfo[END_PTS]

# consistency cross stream
for segIndex in bySegIndex.keys():
	firstTime = True
	for fileIndex in bySegIndex[segIndex]:
		url, timingInfo = bySegIndex[segIndex][fileIndex]
		if firstTime:
			firstTime = False
			minDtsInfo = url, timingInfo
			maxDtsInfo = url, timingInfo
			minPtsInfo = url, timingInfo
			maxPtsInfo = url, timingInfo
			continue
			
		if timingInfo[START_DTS] < minDtsInfo[1][START_DTS]:
			minDtsInfo = url, timingInfo
		if timingInfo[START_DTS] > maxDtsInfo[1][START_DTS]:
			maxDtsInfo = url, timingInfo
		if timingInfo[START_PTS] < minPtsInfo[1][START_PTS]:
			minPtsInfo = url, timingInfo
		if timingInfo[START_PTS] > maxPtsInfo[1][START_PTS]:
			maxPtsInfo = url, timingInfo
	# dts
	gapSize = maxDtsInfo[1][START_DTS] - minDtsInfo[1][START_DTS]
	if gapSize > THRESHOLD:
		errors.append(['cross-stream dts gap', 
			minDtsInfo[1][START_DTS],
			'size=%s (%.3f)' % (gapSize, gapSize / float(timescale)),
			'min=%s (%.3f)' % (minDtsInfo[1][START_DTS], minDtsInfo[1][START_DTS] / float(timescale)), 
			'max=%s (%.3f)' % (maxDtsInfo[1][START_DTS], maxDtsInfo[1][START_DTS] / float(timescale)), 
			'stream=%s' % segIndex,
			'minurl=%s' % minDtsInfo[0], 
			'maxurl=%s' % maxDtsInfo[0]])

	# pts
	gapSize = maxPtsInfo[1][START_PTS] - minPtsInfo[1][START_PTS]
	if gapSize > THRESHOLD:
		errors.append(['cross-stream pts gap', 
			minPtsInfo[1][START_PTS],
			'size=%s (%.3f)' % (gapSize, gapSize / float(timescale)),
			'min=%s (%.3f)' % (minPtsInfo[1][START_PTS], minPtsInfo[1][START_PTS] / float(timescale)), 
			'max=%s (%.3f)' % (maxPtsInfo[1][START_PTS], maxPtsInfo[1][START_PTS] / float(timescale)), 
			'stream=%s' % segIndex,
			'minurl=%s' % minPtsInfo[0], 
			'maxurl=%s' % maxPtsInfo[0]])

for cells in sorted(errors):
	print(cells[0] + ', ' + '\n\t'.join(cells[2:]) + '\n')
			
print('done')
