from xml.parsers.expat import ExpatError
from xml.dom import minidom
from mp4_utils import *
import stress_base
import http_utils
import commands
import operator
import struct
import base64
import pyamf
import time
import re

from hds_compare_params import *

ADOBE_MUX_PACKET_SIZE = 11
AVC_PACKET_TYPE_SEQUENCE_HEADER = 0
AAC_PACKET_TYPE_SEQUENCE_HEADER = 0
TAG_TYPE_AUDIO = 8
TAG_TYPE_VIDEO = 9

DURATION_THRESHOLD = 5
BITRATE_THRESHOLD = 30
METADATA_THRESHOLDS = {
	'audiodatarate': 30,
	'duration': 5,
	'filesize': 20,
	'framerate': 5,
	'videodatarate': 30,
}
BOOTSTRAP_THRESHOLDS = {
	'duration': 5,
	'lastSegmentDuration': 5,
}

def unpackAdobeMuxPacket(data):
	tagType, dataSizeHigh, dataSizeLow, timestampMed, timestampLow, timestampHigh, streamIdHigh, streamIdLow = struct.unpack('>BBHBHBBH', data)
	dataSize = (dataSizeHigh << 16) | dataSizeLow
	timestamp = (timestampHigh << 24) | (timestampMed << 16) | timestampLow
	streamId = (streamIdHigh << 16) | streamIdLow
	return (tagType, dataSize, timestamp, streamId)

def getMdatAtom(buffer):
	atoms = parseAtoms(buffer)
	mdatAtom = getAtom(atoms, 'mdat')
	if mdatAtom == None:
		return False
	mdatStart = mdatAtom[0] + mdatAtom[1]
	mdatEnd = mdatAtom[2]
	return buffer[mdatStart:mdatEnd]

def stripSequenceHeaders(mdatAtom):
	resultVideo = ''
	resultVideoNoTS = ''
	resultAudio = ''
	resultAudioNoTS = ''
	seqHeaders = {}
	curPos = 0
	while curPos + ADOBE_MUX_PACKET_SIZE <= len(mdatAtom):
		(tagType, dataSize, timestamp, streamId) = unpackAdobeMuxPacket(mdatAtom[curPos:(curPos + ADOBE_MUX_PACKET_SIZE)])
		
		isSequenceHeader = False
		if tagType == TAG_TYPE_VIDEO and ord(mdatAtom[curPos + ADOBE_MUX_PACKET_SIZE + 1]) == AVC_PACKET_TYPE_SEQUENCE_HEADER:
			isSequenceHeader = True
		elif tagType == TAG_TYPE_AUDIO and ord(mdatAtom[curPos + ADOBE_MUX_PACKET_SIZE + 1]) == AAC_PACKET_TYPE_SEQUENCE_HEADER:
			isSequenceHeader = True
		
		packetSize = ADOBE_MUX_PACKET_SIZE + dataSize + 4
		packetData = mdatAtom[curPos:(curPos + packetSize)]
		curPos += ADOBE_MUX_PACKET_SIZE + dataSize + 4
		
		packetDataNoTimestamp = packetData[:4] + '\0' * 4 + packetData[8:]
		if isSequenceHeader:
			seqHeaders.setdefault(tagType, packetDataNoTimestamp)
		else:
			if tagType == TAG_TYPE_VIDEO:
				resultVideo += packetData
				resultVideoNoTS += packetDataNoTimestamp
			elif tagType == TAG_TYPE_AUDIO:
				resultAudio += packetData
				resultAudioNoTS += packetDataNoTimestamp
	return tuple(seqHeaders.items()), resultVideo, resultAudio, resultVideoNoTS, resultAudioNoTS

def formatBinaryString(info):
	return commands.getoutput('echo %s | base64 --decode | xxd' % base64.b64encode(info))
	

BOOTSTRAP_INFO_FORMAT1 = [
	'0000 008b 6162 7374 0000 0000 0000 0001 0000 0003 e8',
	(8, 'duration'),
	'00 0000 0000 0000 0000 0000 0000 0100 0000 1961 7372 7400 0000 0000 0000 0001 0000 0001 ',
	(4, 'segmentCount'),
	'0100 0000 4661 6672 7400 0000 0000 0003 e800 0000 0003 0000 0001 0000 0000 0000 0000 ',
	(4, 'segmentDuration'),
	(4, 'lastSegmentIndex'),
	(8, 'lastSegmentTimestamp'),
	(4, 'lastSegmentDuration'),
	'0000 0000 0000 0000 0000 0000 0000 0000 00',
]

BOOTSTRAP_INFO_FORMAT2 = [
	'0000 007b 6162 7374 0000 0000 0000 0001 0000 0003 e8',
	(8, 'duration'),
	'00 0000 0000 0000 0000 0000 0000 0100 0000 1961 7372 7400 0000 0000 0000 0001 0000 0001 ',
	(4, 'segmentCount'),
	'0100 0000 3661 6672 7400 0000 0000 0003 e800 0000 0002 ',
	(4, 'lastSegmentIndex'),
	(8, 'lastSegmentTimestamp'),
	(4, 'lastSegmentDuration'),
	'0000 0000 0000 0000 0000 0000 0000 0000 00'
]

def parseFormat(info, format):
	result = {}
	curPos = 0
	for curFormat in format:
		if type(curFormat) == tuple:
			curValue = info[curPos:(curPos + curFormat[0])]
			curPos += len(curValue)
			if len(curValue) == 4:
				curValue = struct.unpack('>L', curValue)[0]
			elif len(curValue) == 8:
				curValue = struct.unpack('>Q', curValue)[0]
			result[curFormat[1]] = curValue
		else:
			expectedStr = curFormat.replace(' ', '').decode('hex')
			if info[curPos:(curPos + len(expectedStr))] != expectedStr:
				return False
			curPos += len(expectedStr)
	return result

def parseBootstrapInfo(info):
	result = parseFormat(info, BOOTSTRAP_INFO_FORMAT1)
	if result == False:
		result = parseFormat(info, BOOTSTRAP_INFO_FORMAT2)
	return result
	
class ManifestMedia:
	def __init__(self, xml, bootstrapInfos):
		self.bitrate = int(xml.attributes['bitrate'].nodeValue)
		#self.width = int(xml.attributes['width'].nodeValue)
		#self.height = int(xml.attributes['height'].nodeValue)
		self.url = str(xml.attributes['url'].nodeValue)
		bootstrapId = xml.attributes['bootstrapInfoId'].nodeValue
		if bootstrapId.startswith('bootstrap_'):
			self.index = int(bootstrapId[len('bootstrap_'):])
		else:
			self.index = int(bootstrapId[len('bootstrap'):])
		self.bootstrapInfo = bootstrapInfos[bootstrapId]
		
		metadata = str(xml.getElementsByTagName('metadata')[0].firstChild.nodeValue)
		decoder = pyamf.decode(base64.b64decode(metadata), encoding=pyamf.AMF0)
		if str(decoder.next()) == 'onMetaData':
			self.metadata = decoder.next()
		
class Manifest:
	def __init__(self, xml):
		manifestNode = xml.getElementsByTagName('manifest')[0]
		self.duration = float(manifestNode.getElementsByTagName('duration')[0].firstChild.nodeValue)
		
		bootstrapInfos = {}
		for bootstrap in manifestNode.getElementsByTagName('bootstrapInfo'):
			id = bootstrap.attributes['id'].nodeValue
			data = base64.b64decode(str(bootstrap.firstChild.nodeValue))
			bootstrapInfos[id] = data

		self.medias = []
		for media in manifestNode.getElementsByTagName('media'):
			mediaObject = ManifestMedia(media, bootstrapInfos)
			self.medias.append(mediaObject)
		self.medias.sort(key=operator.attrgetter('index'))
		
class TestThread(stress_base.TestThreadBase):
	def __init__(self, index, increment, stopFile):
		stress_base.TestThreadBase.__init__(self, index, increment, stopFile)

	def compareBootstrapInfos(self, info1, info2):
		parsedInfo1 = parseBootstrapInfo(info1)
		if parsedInfo1 == False:
			self.writeOutput('Error: failed to parse bootstrap info\n%s' % formatBinaryString(info1))
			return False
		parsedInfo2 = parseBootstrapInfo(info2)
		if parsedInfo2 == False:
			self.writeOutput('Error: failed to parse bootstrap info\n%s' % formatBinaryString(info2))
			return False
		for key in parsedInfo1:
			value1 = parsedInfo1[key]
			value2 = parsedInfo2[key]
			if BOOTSTRAP_THRESHOLDS.has_key(key):
				threshold = BOOTSTRAP_THRESHOLDS[key]
				if value2 > (value1 * (100 + threshold)) / 100.0 or value2 < (value1 * (100 - threshold)) / 100.0:
					self.writeOutput('Error: bootstrap field value mismatch %s %s %s %s %s' % (key, value1, value2, parsedInfo1, parsedInfo2))
					return False
			elif value1 != value2:
				self.writeOutput('Error: bootstrap field value mismatch %s %s %s %s %s' % (key, value1, value2, parsedInfo1, parsedInfo2))
				return False
		return True
		
	def compareManifests(self, manifest1, manifest2):
		if abs(manifest1.duration - manifest2.duration) > DURATION_THRESHOLD:
			self.writeOutput('Error: duration mismatch %s %s' % (manifest1.duration, manifest2.duration))
			return False
			
		if len(manifest1.medias) != len(manifest2.medias):
			self.writeOutput('Error: media count mismatch %s %s' % (len(manifest1.medias), len(manifest2.medias)))
			return False
			
		for index in xrange(len(manifest1.medias)):
			media1 = manifest1.medias[index]
			media2 = manifest2.medias[index]
			if media2.bitrate > (media1.bitrate * (100 + BITRATE_THRESHOLD)) / 100.0 or \
				media2.bitrate < (media1.bitrate * (100 - BITRATE_THRESHOLD)) / 100.0:
				self.writeOutput('Error: bitrate mismatch %s %s' % (media1.bitrate, media2.bitrate))
				return False
				
			if not self.compareBootstrapInfos(media1.bootstrapInfo, media2.bootstrapInfo):
				return False
			
			keys1 = sorted(media1.metadata.keys())
			keys2 = sorted(media2.metadata.keys())
			if keys1 != keys2:
				self.writeOutput('Error: metadata has different keys %s %s' % (keys1, keys2))
				return False
			
			for key in keys1:
				value1 = media1.metadata[key]
				value2 = media2.metadata[key]
				if METADATA_THRESHOLDS.has_key(key):
					threshold = METADATA_THRESHOLDS[key]
					if value2 > (value1 * (100 + threshold)) / 100.0 or value2 < (value1 * (100 - threshold)) / 100.0:
						self.writeOutput('Error: metadata field value mismatch %s %s %s' % (key, value1, value2))
						return False
				elif value1 != value2:
					self.writeOutput('Error: metadata field value mismatch %s %s %s' % (key, value1, value2))
					return False
		return True

	def getFragmentCount(self, bootstrapInfo):
		asrtPos = bootstrapInfo.find('asrt') + 4
		return struct.unpack('>L', bootstrapInfo[(asrtPos + 13):(asrtPos + 17)])[0]

	def getUrl(self, url):
		startTime = time.time()
		code, headers, body = http_utils.getUrl(url)
		if code == 0:
			self.writeOutput(body)
		self.writeOutput('Info: get %s took %s' % (url, time.time() - startTime))
		return (code, body)

	def compareStream(self, url1, url2, streamName, stream1, stream2, lastChance):
		if stream1 == stream2:
			return True
		if stream1.startswith(stream2):
			self.writeOutput('Notice: %s second mdat is a prefix of the first mdat len1=%s len2=%s diff=%s' % (streamName, len(stream1), len(stream2), abs(len(stream1) - len(stream2))))
		elif stream2.startswith(stream1):
			self.writeOutput('Notice: %s first mdat is a prefix of the second mdat len1=%s len2=%s diff=%s' % (streamName, len(stream1), len(stream2), abs(len(stream1) - len(stream2))))
		elif stream1.endswith(stream2):
			self.writeOutput('Notice: %s second mdat is a postfix of the first mdat len1=%s len2=%s diff=%s' % (streamName, len(stream1), len(stream2), abs(len(stream1) - len(stream2))))
		elif stream2.endswith(stream1):
			self.writeOutput('Notice: %s first mdat is a postfix of the second mdat len1=%s len2=%s diff=%s' % (streamName, len(stream1), len(stream2), abs(len(stream1) - len(stream2))))
		elif stream1 in stream2:
			self.writeOutput('Notice: %s first mdat is contained inside the second mdat len1=%s len2=%s diff=%s' % (streamName, len(stream1), len(stream2), abs(len(stream1) - len(stream2))))
		elif stream2 in stream1:
			self.writeOutput('Notice: %s second mdat is contained inside the second mdat len1=%s len2=%s diff=%s' % (streamName, len(stream1), len(stream2), abs(len(stream1) - len(stream2))))
		else:
			stream1Offset = stream1.find(stream2[:0x1000])
			stream2Offset = stream2.find(stream1[:0x1000])
			if stream1Offset > 0 and stream2.startswith(stream1[stream1Offset:]):
				self.writeOutput('Notice: %s mdat shift len1=%s len2=%s diff=%s commonsize=%s' % (streamName, len(stream1), len(stream2), abs(len(stream1) - len(stream2)), len(stream1) - stream1Offset))
			elif stream2Offset > 0 and stream1.startswith(stream2[stream2Offset:]):
				self.writeOutput('Notice: %s mdat shift len1=%s len2=%s diff=%s commonsize=%s' % (streamName, len(stream1), len(stream2), abs(len(stream1) - len(stream2)), len(stream2) - stream2Offset))
			else:
				if lastChance:
					self.writeOutput('Error: %s fragment test failed %s %s len1=%s len2=%s diff=%s' % (streamName, url1, url2, len(stream1), len(stream2), abs(len(stream1) - len(stream2))))
				return False
		return True
		
	def compareFragments(self, manifestUrl1, manifest1, manifestUrl2, manifest2):
		baseUrl1 = manifestUrl1.rsplit('/', 1)[0] + '/'
		baseUrl2 = manifestUrl2.rsplit('/', 1)[0] + '/'
		for index in xrange(len(manifest1.medias)):
			media1 = manifest1.medias[index]
			media2 = manifest2.medias[index]
			fragmentCount1 = self.getFragmentCount(media1.bootstrapInfo)
			fragmentCount2 = self.getFragmentCount(media2.bootstrapInfo)
			if fragmentCount1 != fragmentCount2:
				self.writeOutput('Error: fragment count mismatch %s %s' % (fragmentCount1, fragmentCount2))
				return False
			for fragmentIndex in xrange(fragmentCount1):
				fragmentSuffix = 'Seg1-Frag%s' % (fragmentIndex + 1)
				url1 = baseUrl1 + media1.url + fragmentSuffix
				url2 = baseUrl2 + media2.url + fragmentSuffix
				self.writeOutput('Info: comparing %s %s' % (url1, url2))
				code1, fragment1 = self.getUrl(url1)
				code2, fragment2 = self.getUrl(url2)
				if code1 != code2:
					self.writeOutput('Error: got different status codes %s vs %s' % (code1, code2))
					return False
				if code1 != 200:
					self.writeOutput('Notice: got error status code %s, skipping comparison' % (code1))
					return True
				mdatAtom1 = getMdatAtom(fragment1)
				if mdatAtom1 == False:
					self.writeOutput('Error: failed to get mdat atoms %s %s' % (url1, url2))
					return False
				mdatAtom2 = getMdatAtom(fragment2)
				if mdatAtom2 == False:
					self.writeOutput('Error: failed to get mdat atoms %s %s' % (url1, url2))
					return False
				seqHeaders1, video1, audio1, videoNoTS1, audioNoTS1 = stripSequenceHeaders(mdatAtom1)
				seqHeaders2, video2, audio2, videoNoTS2, audioNoTS2 = stripSequenceHeaders(mdatAtom2)
				if seqHeaders1 != seqHeaders2:
					self.writeOutput('Error: non matching sequence headers %s %s' % (repr(seqHeaders1), repr(seqHeaders2)))
					return False
				
				if not self.compareStream(url1, url2, 'video', video1, video2, False):
					if not self.compareStream(url1, url2, 'video-ignore-ts', videoNoTS1, videoNoTS2, True):
						return False

				if not self.compareStream(url1, url2, 'audio', audio1, audio2, False):
					if not self.compareStream(url1, url2, 'audio-ignore-ts', audioNoTS1, audioNoTS2, True):
						return False
		return True

	def runTest(self, uri):
		if ',' in uri:
			manifestUrl1 = BASE_URL1 % (uri + MULTI_URL_SUFFIX1)
			manifestUrl2 = BASE_URL2 % (uri + MULTI_URL_SUFFIX2)
		else:
			manifestUrl1 = BASE_URL1 % uri
			manifestUrl2 = BASE_URL2 % uri
		
		self.writeOutput('Info: comparing %s %s' % (manifestUrl1, manifestUrl2))

		# avoid billing real partners
		manifestUrl1 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), manifestUrl1)
		manifestUrl2 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), manifestUrl2)
		
		code1, manifest1 = self.getUrl(manifestUrl1)
		code2, manifest2 = self.getUrl(manifestUrl2)
		if code1 != code2:
			self.writeOutput('Error: got different status codes %s vs %s' % (code1, code2))
			return False
		if code1 != 200:
			self.writeOutput('Notice: got error status code %s, skipping comparison' % (code1))
			return True

		try:
			manifest1 = minidom.parseString(manifest1)
		except ExpatError:
			manifest1 = False
			
		try:
			manifest2 = minidom.parseString(manifest2)
		except ExpatError:
			manifest2 = False

		if manifest1 == False and manifest2 == False:
			self.writeOutput('Info: failed to parse both manifests')
			return True
		elif manifest1 == False and manifest2 != False:
			self.writeOutput('Error: failed to parse first manifest')
			return False
		elif manifest1 != False and manifest2 == False:
			self.writeOutput('Error: failed to parse second manifest')
			return False
		
		manifest1 = Manifest(manifest1)
		manifest2 = Manifest(manifest2)
			
		if not self.compareManifests(manifest1, manifest2):
			return False
		if not self.compareFragments(manifestUrl1, manifest1, manifestUrl2, manifest2):
			return False
		return True

if __name__ == '__main__':
	stress_base.main(TestThread, STOP_FILE)
