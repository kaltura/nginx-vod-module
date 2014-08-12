import stress_base
import commands
import urllib
import sys
import os

from validate_iframes_params import *

SERVER_URL = 'http://localhost:8001/mapped/hls'
IFRAMES_URI = '/iframes.m3u8'

class TestThread(stress_base.TestThreadBase):
	def __init__(self, index, increment, stopFile):
		stress_base.TestThreadBase.__init__(self, index, increment, stopFile)
		uniqueId = '%s.%s' % (os.getpid(), index)
		self.tempFileName = '/tmp/testSeg.ts.%s' % uniqueId

	def getFfprobeKeyframes(self, output, fileSize):
		iframePositions = []
		allPositions = []
		curFrame = {}
		for curLine in output.split('\n'):
			curLine = curLine.strip()
			if curLine == '[FRAME]':
				curFrame = {}
			elif curLine == '[/FRAME]':
				if curFrame['pkt_pos'] != 'N/A':
					allPositions.append(int(curFrame['pkt_pos']))
				if curFrame['media_type'] == 'video' and curFrame['pict_type'] == 'I':
					iframePositions.append(int(curFrame['pkt_pos']))
			else:
				splittedLine = curLine.split('=', 1)
				if len(splittedLine) == 2:
					curFrame[splittedLine[0]] = splittedLine[1]
		allPositions.append(fileSize)
		allPositions.sort()
		result = []
		for iframePosition in iframePositions:
			iframeIndex = allPositions.index(iframePosition)
			iframeSize = allPositions[iframeIndex + 1] - iframePosition
			result.append((iframePosition, iframeSize))
		return result

	def runTest(self, serveFlavorUri):
		iframesUrl = '%s%s%s' % (SERVER_URL, serveFlavorUri, IFRAMES_URI)

		f = urllib.urlopen(iframesUrl)
		if f.getcode() != 200:
			self.writeOutput('Notice: %s returned status %s' % (iframesUrl, f.getcode()))
			return True
		iframes = f.read()
		
		if not iframes.startswith('#EXTM3U'):
			self.writeOutput('Error: %s unexpected response %s' % (iframesUrl, iframes))
			return False

		segIframeRanges = {}
		for curLine in iframes.split('\n'):
			curLine = curLine.strip()
			if len(curLine) == 0:
				continue
			if curLine.startswith('#EXT-X-BYTERANGE:'):
				range = curLine[len('#EXT-X-BYTERANGE:'):]
			if curLine.startswith('seg-'):
				splittedRange = range.split('@')[::-1]
				segIframeRanges.setdefault(curLine, [])
				segIframeRanges[curLine].append(tuple(map(lambda x: int(x), splittedRange)))

		result = True
		for curSegmentFile in sorted(segIframeRanges.keys()):
			iframeRanges = segIframeRanges[curSegmentFile]
			segmentUrl = '%s%s/%s' % (SERVER_URL, serveFlavorUri, curSegmentFile)
			os.system("curl -s '%s' > %s" % (segmentUrl, self.tempFileName))
			actualRanges = self.getFfprobeKeyframes(
				commands.getoutput('%s -i %s -show_frames' % (FFPROBE_BIN, self.tempFileName)),
				os.path.getsize(self.tempFileName))
			os.remove(self.tempFileName)

			self.writeOutput('Info: reported ranges: %s' % iframeRanges)
			self.writeOutput('Info: actual ranges: %s' % actualRanges)
			if len(set(iframeRanges) - set(actualRanges)) == 0:
				self.writeOutput('Info: ok %s' % segmentUrl)
			else:
				# note: we may get a larger packet size from ffprobe
				realError = False
				actualRangesDict = dict(actualRanges)
				for curPos, curSize in iframeRanges:
					if not curPos in actualRangesDict or actualRangesDict[curPos] < curSize:
						realError = True
				if realError:
					self.writeOutput('Error: %s' % segmentUrl)
				else:
					self.writeOutput('Warning: %s' % segmentUrl)
				result = False
		return result
		
if __name__ == '__main__':
	stress_base.main(TestThread, STOP_FILE)
