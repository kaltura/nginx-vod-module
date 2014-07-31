import commands
import urllib
import sys
import os

# zgrep serveFlavor /data/logs/investigate/2014/06/27/pa-front-origin1-access_log-2014-06-27.gz | grep kalturavod-i.origin.kaltura.com | awk '{print $7}' | sort -u > /tmp/serveFlavorUris.txt

FFPROBE_BIN = '/web/content/shared/bin/ffmpeg-2.1.3-bin/ffprobe-2.1.3.sh'
HOSTNAME = 'localhost:4321'

def getFfprobeKeyframes(output, fileSize):
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

def testUri(serveFlavorUri):
	iframesUrl = 'http://%s%s/iframes.m3u8' % (HOSTNAME, serveFlavorUri)

	f = urllib.urlopen(iframesUrl)
	if f.getcode() != '200':
		print 'Notice: %s returned status %s' % (iframesUrl, f.getcode())
		return
	iframes = f.read()
	
	if not iframes.startswith('#EXTM3U'):
		print 'Error: %s unexpected response %s' % (iframesUrl, iframes)
		return

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

	for curSegmentFile in sorted(segIframeRanges.keys()):
		iframeRanges = segIframeRanges[curSegmentFile]
		segmentUrl = 'http://%s%s/%s' % (HOSTNAME, serveFlavorUri, curSegmentFile)
		os.system("curl -s '%s' > /tmp/testSeg.ts" % segmentUrl)
		actualRanges = getFfprobeKeyframes(
			commands.getoutput('%s -i /tmp/testSeg.ts -show_frames' % FFPROBE_BIN),
			os.path.getsize('/tmp/testSeg.ts'))

		print 'Reported ranges: %s' % iframeRanges
		print 'Actual ranges: %s' % actualRanges
		if len(set(iframeRanges) - set(actualRanges)) != 0:
			print 'Error: %s' % segmentUrl
		else:
			print 'Ok: %s' % segmentUrl
		
if __name__ == '__main__':
	if len(sys.argv) != 2:
		print 'Usage: %s <serveFlavor uris file>' % os.path.basename(sys.argv[0])
		sys.exit(1)
	
	for curLine in file(sys.argv[1], 'rb'):
		curLine = curLine.strip()
		testUri(curLine)
