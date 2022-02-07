from ts_utils import decryptTsSegment
import stress_base
import http_utils
import urlparse
import commands
import tempfile
import urllib2
import hashlib
import shutil
import random
import struct
import socket
import time
import gzip
import os
import re

from hls_compare_params import *

MAX_TS_SEGMENTS_TO_COMPARE = 10

TS_PACKET_LENGTH = 188

PTS_THRESHOLD = 1000		# 1/90 sec
DTS_THRESHOLD = 1000		# 1/90 sec
IGNORED_PREFIXES = ['pos=', 'pts_time=', 'dts_time=']
IGNORED_PREFIXES_FULL = ['pos=', 'pts_time=', 'dts_time=', 'pts=', 'dts=']

TEMP_DIR = '/mnt/vodtest/'

RETRIES = 1

def parseTimestamp(value):
	if value == 'N/A':
		return -1
	return int(value)

class TestThread(stress_base.TestThreadBase):
	def writeToTempFile(self, data):
		f = tempfile.NamedTemporaryFile(delete=False, dir=TEMP_DIR)
		f.write(data)
		f.close()
		return f.name
		
	def shouldCompareLine(self, line):
		for ignoredPrefix in IGNORED_PREFIXES_FULL:
			if line.startswith(ignoredPrefix):
				return False
		return True
		
	def writeLinesToDiff(self, lines):
		lines = filter(self.shouldCompareLine, lines)
		return self.writeToTempFile('\n'.join(lines))

	def compareFfprobeOutputs(self, ffprobeData1, ffprobeData2, streamType, messages):
		lines1 = ffprobeData1.split('\n')
		lines2 = ffprobeData2.split('\n')

		if len(lines1) != len(lines2):
			fileName1 = self.writeLinesToDiff(lines1)
			fileName2 = self.writeLinesToDiff(lines2)
			diffResult = commands.getoutput('diff -bBu %s.tmp %s.tmp < /dev/null' % (fileName1, fileName2))
			os.remove(fileName1)
			os.remove(fileName2)

			diffResult = '\n'.join(diffResult.split('\n')[:1000])
			messages.append('Error: line count mismatch %s vs %s' % (len(lines1), len(lines2)))
			messages.append(diffResult)
			return False
		
		#ptsDiff = None
		#dtsDiff = None
		result = True
		for curIndex in xrange(len(lines1)):
			line1 = lines1[curIndex].strip()
			line2 = lines2[curIndex].strip()
			if line1 == line2:
				continue
			
			skipLine = False
			for ignoredPrefix in IGNORED_PREFIXES:
				if line1.startswith(ignoredPrefix) and line2.startswith(ignoredPrefix):
					skipLine = True
					break
			if skipLine:
				continue

			# pts
			if line1.startswith('pts=') and line2.startswith('pts='):
				pts1 = parseTimestamp(line1.split('=')[1])
				pts2 = parseTimestamp(line2.split('=')[1])
				#if ptsDiff is None:
				#	ptsDiff = pts1 - pts2
				#	continue
				curDiff = abs(pts1 - pts2) #(pts2 + ptsDiff))
				if curDiff > PTS_THRESHOLD:
					messages.append('Error: pts diff exceeds threshold - pts1 %s pts2 %s streamType %s' % (pts1, pts2, streamType))
					result = False
				continue

			# dts
			if line1.startswith('dts=') and line2.startswith('dts='):
				dts1 = parseTimestamp(line1.split('=')[1])
				dts2 = parseTimestamp(line2.split('=')[1])
				#if dtsDiff is None:
				#	dtsDiff = dts1 - dts2
				#	continue
				curDiff = abs(dts1 - dts2) #(dts2 + dtsDiff))
				if curDiff > DTS_THRESHOLD:
					messages.append('Error: dts diff exceeds threshold - dts1 %s dts2 %s streamType %s' % (dts1, dts2, streamType))
					result = False
				continue				
				
			messages.append('Error: test failed line1=%s line2=%s' % (line1, line2))
			result = False
		return result

	def runFfprobe(self, tsData, streamType):
		tempFilename = self.writeToTempFile(tsData)
		ffprobeData = commands.getoutput('%s -i %s -show_packets -show_data -select_streams %s 2>/dev/null' % (FFPROBE_BIN, tempFilename, streamType))
		os.remove(tempFilename)
		return ffprobeData
		
	def compareStream(self, tsData1, tsData2, streamType, messages):
		ffprobeData1 = self.runFfprobe(tsData1, streamType)
		ffprobeData2 = self.runFfprobe(tsData2, streamType)
		return self.compareFfprobeOutputs(ffprobeData1, ffprobeData2, streamType, messages)

	def testContinuity(self, tsData, continuityCounters):
		okCounters = 0
		result = True
		for curPos in xrange(0, len(tsData), TS_PACKET_LENGTH):
			pid = ((ord(tsData[curPos + 1]) & 0x1f) << 8) | ord(tsData[curPos + 2])
			cc = ord(tsData[curPos + 3]) & 0x0f
			
			if continuityCounters.has_key(pid):
				lastValue = continuityCounters[pid]
				expectedValue = (lastValue + 1) & 0x0f
				if cc != expectedValue:
					self.writeOutput('Error: bad continuity counter - pos=%s pid=%d exp=%s actual=%s' % 
						(curPos, pid, expectedValue, cc))
					result = False
				else:
					okCounters += 1
			continuityCounters[pid] = cc
		self.writeOutput('Info: validated %s counters' % okCounters)
		return result

	def md5sum(self, data):
		m = hashlib.md5()
		m.update(data)
		return m.hexdigest()
		
	def retrieveUrl(self, url):
		startTime = time.time()
		code, headers, data = http_utils.getUrl(url)
		if code == 0:
			self.writeOutput(data)
		self.writeOutput('Info: get %s took %s, size %s cksum %s' % (url, time.time() - startTime, len(data), self.md5sum(data)))
		return code, data

	def compareTsUris(self, url1, url2, segmentIndex, aesKey, continuityCounters):
		result = True
		self.writeOutput('Info: comparing %s %s' % (url1, url2))

		code1, tsData1 = self.retrieveUrl(url1)
		if code1 == 200:
			if aesKey != None:
				tsData1, error = decryptTsSegment(self.tsData1, aesKey, segmentIndex)
				if tsData1 == None:
					self.writeOutput(error)
					return False
			
			if not self.testContinuity(tsData1, continuityCounters):
				result = False
		else:
			continuityCounters.clear()
			
		for attempt in xrange(RETRIES):
			code2, tsData2 = self.retrieveUrl(url2)
			
			if code1 != code2:
				if code1 != 0 and code2 != 0:
					self.writeOutput('Error: got different status codes %s vs %s (ts)' % (code1, code2))
				return False
			
			messages = []
			curResult = True
			if not self.compareStream(tsData1, tsData2, 'a', messages):
				curResult = False
			if not self.compareStream(tsData1, tsData2, 'v', messages):
				curResult = False
			if curResult or attempt + 1 >= RETRIES:
				break
			self.writeOutput('Info: got errors, retrying...')
		
		self.writeOutput('Info: size diff is %s' % (len(tsData2) - len(tsData1)))
		
		for message in messages:
			self.writeOutput(message)
		return result and curResult

	@staticmethod		
	def getTsSegments(manifest):
		result = []
		duration = None
		for curLine in manifest.split('\n'):
			curLine = curLine.strip()
			if curLine.startswith('#EXTINF:'):
				duration = float(curLine[len('#EXTINF:'):].split(',')[0])
			if len(curLine) > 0 and curLine[0] != '#':
				result.append((curLine, duration))
		return result

	def runTest(self, uri):		
		url1 = URL1_BASE + random.choice(URL1_PREFIXES) + uri
		url2 = URL2_PREFIX + uri
		
		self.writeOutput('Info: testing %s %s' % (url1, url2))
		
		# avoid billing real partners
		url1 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url1)
		url2 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url2)
		
		# get the manifests
		code1, manifest1 = self.retrieveUrl(url1 + URL1_SUFFIX)
		code2, manifest2 = self.retrieveUrl(url2 + URL2_SUFFIX)
		if code1 != code2:
			self.writeOutput('Error: got different status codes %s vs %s (m3u8)' % (code1, code2))
			return False

		if code1 == 404 and code2 == 404:
			self.writeOutput('Notice: both servers returned 404')
			return True
		
		if not manifest1.startswith('#EXTM3U') or not manifest2.startswith('#EXTM3U'):
			if not manifest1.startswith('#EXTM3U') and not manifest2.startswith('#EXTM3U'):
				self.writeOutput('Notice: both servers returned invalid manifests')
				return True
			if not manifest1.startswith('#EXTM3U'):
				self.writeOutput('Error: server1 returned invalid manifest')
				self.writeOutput('manifest1=%s' % manifest1)
				return True
			if not manifest2.startswith('#EXTM3U'):
				self.writeOutput('Error: server2 returned invalid manifest')
				self.writeOutput('manifest2=%s' % manifest2)
				return True
		
		# extract the ts uris
		tsUris1 = self.getTsSegments(manifest1)
		tsUris2 = self.getTsSegments(manifest2)
		if len(tsUris1) != len(tsUris2):
			if len(tsUris1) < len(tsUris2) and '/clipTo/' in uri:
				clipToValue = uri.split('/clipTo/')[1].split('/')[0]
				self.writeOutput('Notice: ignoring TS count mismatch (%s vs %s) due to clipTo %s' % (len(tsUris1), len(tsUris2), clipToValue))
				tsUris2 = tsUris2[:len(tsUris1)]
			else:
				self.writeOutput('Error: TS segment count mismatch %s vs %s' % (len(tsUris1), len(tsUris2)))

		# check the durations
		minCount = min(len(tsUris1), len(tsUris2))
		for curIndex in xrange(minCount):
			duration1 = tsUris1[curIndex][1]
			duration2 = tsUris2[curIndex][1]
			if abs(duration1 - duration2) > 0.01:
				self.writeOutput('Error: TS durations mismatch at index %s - %s vs %s' % (curIndex, duration1, duration2))
				return False
				
		# get the encryption key, if exists
		keyUri = None
		for curLine in manifest1.split('\n'):
			if curLine.startswith('#EXT-X-KEY'):
				keyUri = curLine.split('"')[1]
		if keyUri != None:
			url = url1 + URL1_SUFFIX
			baseUrl = url[:url.rfind('/')] + '/'
			try:
				aesKey = urllib2.urlopen(baseUrl + keyUri).read()
			except urllib2.HTTPError as e:
				self.writeOutput('Error: failed to get the encryption key, code=%s' % e.getcode())
				return False
		else:
			aesKey = None

		# compare the ts segments
		result = True
		continuityCounters = {}
		self.writeOutput('Info: segmentCount=%s' % minCount)
		for curIndex in xrange(max(minCount - MAX_TS_SEGMENTS_TO_COMPARE, 0), minCount, 1):
			if os.path.exists(STOP_FILE):
				return True
			tsUrl1 = urlparse.urljoin(url1 + '/', tsUris1[curIndex][0])
			tsUrl2 = urlparse.urljoin(url2 + '/', tsUris2[curIndex][0])
			if not self.compareTsUris(tsUrl1, tsUrl2, curIndex + 1, aesKey, continuityCounters):
				self.writeOutput('Error: ts comparison failed - url1=%s, url2=%s' % (tsUrl1, tsUrl2))
				result = False
		self.writeOutput('Info: success')
		return result
		
if __name__ == '__main__':
	# delete temp files
	for curFile in os.listdir(TEMP_DIR):
		fullPath = os.path.join(TEMP_DIR, curFile)
		os.remove(fullPath)

	# run the stress main
	stress_base.main(TestThread, STOP_FILE)
