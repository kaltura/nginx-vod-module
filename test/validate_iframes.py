from httplib import BadStatusLine
from mpeg_ts_defs import *
import stress_base
import http_utils
import commands
import urllib2
import socket
import time
import sys
import os

from validate_iframes_params import *

SERVER_URL = 'http://localhost:8001/mapped/hls'
IFRAMES_URI = '/iframes.m3u8'

## MPEG TS processing code

MPEGTS_TIMESCALE = 90000

WAIT_MARKER = 0
WAIT_NAL_TYPE = 1

NAL_IDR_SLICE = 5
NAL_AUD = 9

def getPmtPidFromPat(curPacket, curPos):
	thePat = pat.parse(curPacket[curPos:])
	curPos += pat.sizeof()
	entryCount = (thePat.sectionLength - 9) / patEntry.sizeof()
	for i in range(entryCount):
		curEntry = patEntry.parse(curPacket[curPos:])
		curPos += patEntry.sizeof()
		if curEntry.programNumber == 1:
			return curEntry.programPID
	return None
	
def getVideoPidsFromPmt(curPacket, curPos):
	thePmt = pmt.parse(curPacket[curPos:])
	curPos += pmt.sizeof()
	curPos += thePmt.programInfoLength
	endPos = curPos + thePmt.sectionLength - 13
	result = []
	while curPos < endPos:
		curEntry = pmtEntry.parse(curPacket[curPos:])
		if curPacket[curPos] == '\x1b':
			result.append(curEntry.elementaryPID)
		curPos += pmtEntry.sizeof() + curEntry.esInfoLength
	return result
			
def getKeyFramesInfo(inputData, initialPts):
	
	programPID = None
	videoPids = []
		
	packetStartBuffer = []
	markerBuffer = ''
	state = WAIT_MARKER
	lastPesPos = None
	curStartPos = None
	keyFrame = None
	
	result = []
	for packetStart in xrange(0, len(inputData), TS_PACKET_LENGTH):
		curPacket = buffer(inputData, packetStart, TS_PACKET_LENGTH)
		
		# skip the TS header
		curPos = 4	# sizeof ts header
		
		# skip the adaptation field
		if ord(curPacket[3]) & 0x20: # adaptationFieldExist
			curPos += 1 + ord(curPacket[curPos])
			
		pid = ((ord(curPacket[1]) & 0x1f) << 8) | ord(curPacket[2])
		if pid == PAT_PID:
			programPID = getPmtPidFromPat(curPacket, curPos)
		elif pid == programPID:
			videoPids = getVideoPidsFromPmt(curPacket, curPos)
		elif pid in videoPids:
			if ord(curPacket[1]) & 0x40:	# payloadUnitStartIndicator
				# get the pts value
				lastPesPos = packetStart
				lastPtsValue = None
				curPos += 6
				thePesOptHeader = pesOptionalHeader.parse(curPacket[curPos:])
				curPos += pesOptionalHeader.sizeof()
				if thePesOptHeader.ptsFlag:
					ptsStruct = pts.parse(curPacket[curPos:])
					lastPtsValue = getPts(ptsStruct)
					curPos += pts.sizeof()
					if thePesOptHeader.dtsFlag:
						curPos += pts.sizeof()
				
			if len(curPacket[curPos:]) == 0:
				continue
					
			# process the video stream
			curBuffer = markerBuffer + curPacket[curPos:]
			splittedBuffer = curBuffer.split('\x00\x00\x01')
			for curIndex in xrange(1, len(splittedBuffer)):
				if len(splittedBuffer[curIndex]) == 0:
					continue
				nalType = ord(splittedBuffer[curIndex][0]) & 0x1f
				if nalType == NAL_IDR_SLICE:
					keyFrame = True
				if nalType == NAL_AUD:
					if not curPacket[curPos:].startswith('\x00\x00\x00\x01\x09'):
						if curIndex == 1 and len(splittedBuffer[0]) < len(packetStartBuffer):
							curEndPos = packetStartBuffer[len(splittedBuffer[0])] + TS_PACKET_LENGTH
						else:
							curEndPos = packetStart + TS_PACKET_LENGTH
					if keyFrame:
						if initialPts == None:
							initialPts = curPts
						result.append((curPts - initialPts, curStartPos, curEndPos - curStartPos))
					curStartPos = lastPesPos
					curPts = lastPtsValue
					keyFrame = False
					
			markerBuffer = curBuffer[-3:]
			packetStartBuffer = packetStartBuffer + [packetStart] * min(len(curPacket[curPos:]), 4)
			packetStartBuffer = packetStartBuffer[-4:]

			curEndPos = packetStart + TS_PACKET_LENGTH

	if keyFrame:
		if initialPts == None:
			initialPts = curPts
		result.append((curPts - initialPts, curStartPos, curEndPos - curStartPos))
				
	return (initialPts, result)

## Test thread

class TestThread(stress_base.TestThreadBase):
	def parseIframesM3u8(self, iframesUrl, iframes):
		if not iframes.startswith('#EXTM3U'):
			self.writeOutput('Error: %s unexpected response %s' % (iframesUrl, iframes))
			return False

		result = {}
		startOffset = 0
		duration = 0
		for curLine in iframes.split('\n'):
			curLine = curLine.strip()
			if len(curLine) == 0:
				continue
			if curLine.startswith('#EXT-X-BYTERANGE:'):
				range = curLine[len('#EXT-X-BYTERANGE:'):]
				range = range.split('@')[::-1]
				range = map(lambda x: int(x), range)
			elif curLine.startswith('#EXTINF:'):
				startOffset += duration
				duration = curLine[len('#EXTINF:'):].split(',')[0]
				duration = float(duration)
			elif curLine.startswith('seg-'):
				result.setdefault(curLine, [])
				result[curLine].append((int(startOffset * MPEGTS_TIMESCALE), range[0], range[1]))
		return result
		
	@staticmethod
	def compareIframes(reportedIframes, actualIframes, initialPts):
		actualIframesDict = {}
		for pts, pos, size in actualIframes:
			actualIframesDict[(pos, size)] = pts
		
		for pts1, pos, size in reportedIframes:
			if not (pos, size) in actualIframesDict:
				return False
			pts2 = actualIframesDict[(pos, size)]
			if abs(pts1 - pts2) > 2000:
				return False
		return True
		
	def getUrl(self, url):
		startTime = time.time()
		code, headers, body = http_utils.getUrl(url)
		if code == 0:
			self.writeOutput(body)
		self.writeOutput('Info: get %s took %s' % (url, time.time() - startTime))
		return (code, body)
		
	def runTest(self, serveFlavorUri):
		iframesUrl = '%s%s%s' % (SERVER_URL, serveFlavorUri, IFRAMES_URI)

		(code, iframes) = self.getUrl(iframesUrl)
		if code == 0:
			return False
			
		if code != 200:
			self.writeOutput('Notice: %s returned status %s' % (iframesUrl, code))
			return True

		segIframeRanges = self.parseIframesM3u8(iframesUrl, iframes)
		
		result = True
		initialPts = None
		for curSegmentFile in sorted(segIframeRanges.keys()):
			reportedIframes = segIframeRanges[curSegmentFile]
			segmentUrl = '%s%s/%s' % (SERVER_URL, serveFlavorUri, curSegmentFile)
			
			(code, segmentData) = self.getUrl(segmentUrl)
			if code == 0:
				return False
				
			if code != 200:
				self.writeOutput('Notice: %s returned status %s' % (segmentUrl, code))
				continue
			
			initialPts, actualIframes = getKeyFramesInfo(segmentData, initialPts)
			
			self.writeOutput('Info: reported ranges: %s' % reportedIframes)
			self.writeOutput('Info: actual ranges: %s' % actualIframes)

			if not self.compareIframes(reportedIframes, actualIframes, initialPts):
				self.writeOutput('Error: %s' % segmentUrl)
				result = False
		return result
		
if __name__ == '__main__':
	stress_base.main(TestThread, STOP_FILE)

#print getKeyFramesInfo(urllib.urlopen('http://localhost:8001/mapped/hls/p/513551/sp/51355100/serveFlavor/entryId/0_rn8sdtst/v/2/flavorId/0_k2k8kd8e/name/a.mp4/seg-1-v1-a1.ts').read(), None)
#print getKeyFramesInfo(urllib.urlopen('http://localhost:8001/mapped/hls/p/1067292/sp/106729200/serveFlavor/entryId/0_d0sxlp48/v/1/pv/2/flavorId/0_n2du4e5l/name/a.mp4/seg-12-v1-a1.ts').read(), None)
#print getKeyFramesInfo(urllib.urlopen('http://localhost:8001/mapped/hls/p/1612851/sp/161285100/serveFlavor/entryId/1_stvs5gw9/v/1/flavorId/1_t84gc4ia/name/a.mp4/seg-3-v1-a1.ts').read(), None)
