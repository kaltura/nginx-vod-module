from Crypto.Cipher import AES
from StringIO import StringIO
from construct import *
from httplib import BadStatusLine
import stress_base
import urlparse
import commands
import urllib2
import shutil
import random
import struct
import time
import gzip
import os
import re

from hls_compare_params import *

MAX_TS_SEGMENTS_TO_COMPARE = 10

TS_PACKET_LENGTH = 188

mpegTsHeader = BitStruct("mpegTsHeader",
	BitField("syncByte", 8),
	Flag("transportErrorIndicator"),
	Flag("payloadUnitStartIndicator"),
	Flag("transportPriority"),
	BitField("PID", 13),
	BitField("scramblingControl", 2),
	Flag("adaptationFieldExist"),
	Flag("containsPayload"),
	BitField("continuityCounter", 4),
	)

PTS_THRESHOLD = 1000		# 1/90 sec
DTS_THRESHOLD = 1000		# 1/90 sec
IGNORED_PREFIXES = ['pos=', 'pts_time=', 'dts_time=']

class TestThread(stress_base.TestThreadBase):
	def __init__(self, index, increment, stopFile):
		stress_base.TestThreadBase.__init__(self, index, increment, stopFile)
		uniqueId = '%s.%s' % (os.getpid(), index)
		self.tsFile1 = '%s.%s' % (TEMP_TS_FILE1, uniqueId)
		self.tsFile2 = '%s.%s' % (TEMP_TS_FILE2, uniqueId)
		self.ffprobeFile1 = '%s.%s' % (TEMP_FFPROBE_FILE1, uniqueId)
		self.ffprobeFile2 = '%s.%s' % (TEMP_FFPROBE_FILE2, uniqueId)

	def compareFfprobeOutputs(self, file1, file2, streamType, messages):
		lines1 = file(file1, 'rb').readlines()
		lines2 = file(file2, 'rb').readlines()

		if len(lines1) != len(lines2):
			os.system("""cat %s < /dev/null | grep -v ^pts= | grep -v ^dts= | grep -v ^pos= | grep -v ^pts_time= | grep -v ^dts_time= | sed 's/00000000: 0000 0001 09f0/00000000: 0000 0001 09e0/g' > %s.tmp""" % (file1, file1))
			os.system("""cat %s < /dev/null | grep -v ^pts= | grep -v ^dts= | grep -v ^pos= | grep -v ^pts_time= | grep -v ^dts_time= | sed 's/00000000: 0000 0001 09f0/00000000: 0000 0001 09e0/g' > %s.tmp""" % (file2, file2))
			diffResult = commands.getoutput('diff -bBu %s.tmp %s.tmp < /dev/null' % (file1, file2))
			os.remove('%s.tmp' % file1)
			os.remove('%s.tmp' % file2)
				
			if (streamType == 'a' and 
				not '\n-' in diffResult.replace('--- ','') and 
				diffResult.count('\n+[PACKET]') == 1 and 
				diffResult.count('\n+[/PACKET]') == 1 and 
				'ID3' in diffResult and 'TXXX' in diffResult):
				messages.append('Notice: a padding id3 packet was removed')
				return True

			if (not '\n+' in diffResult.replace('+++ ','') and 
				diffResult.count('\n-[PACKET]') == diffResult.count('\n-[/PACKET]') and 
				diffResult.count('\n-[PACKET]') > 0):
				messages.append('Notice: %s %s packets were added' % (diffResult.count('\n-[PACKET]'), 'audio' if streamType == 'a' else 'video'))
				return True
			
			diffResult = '\n'.join(diffResult.split('\n')[:1000])
			messages.append('Error: line count mismatch %s vs %s' % (len(lines1), len(lines2)))
			messages.append(diffResult)
			return False
		
		ptsDiff = None
		dtsDiff = None
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
				pts1 = int(line1.split('=')[1])
				pts2 = int(line2.split('=')[1])
				if ptsDiff is None:
					ptsDiff = pts1 - pts2
					continue
				curDiff = abs(pts1 - (pts2 + ptsDiff))
				if curDiff > PTS_THRESHOLD:
					messages.append('Error: pts diff %s exceeds threshold, pts1=%s pts2=%s diff=%s' % (curDiff, pts1, pts2, ptsDiff))
					return False
				continue
			
			# dts
			if line1.startswith('dts=') and line2.startswith('dts='):
				dts1 = int(line1.split('=')[1])
				dts2 = int(line2.split('=')[1])
				if dtsDiff is None:
					dtsDiff = dts1 - dts2
					continue
				curDiff = abs(dts1 - (dts2 + dtsDiff))
				if curDiff > DTS_THRESHOLD:
					messages.append('Error: dts diff %s exceeds threshold, dts1=%s dts2=%s diff=%s' % (curDiff, dts1, dts2, dtsDiff))
					return False
				continue
			
			# AUD packet
			line1 = line1.replace('00000000: 0000 0001 09f0', '00000000: 0000 0001 09e0')
			line2 = line2.replace('00000000: 0000 0001 09f0', '00000000: 0000 0001 09e0')
			if line1 == line2:
				continue
			
			messages.append('Error: test failed line1=%s line2=%s' % (line1, line2))
			return False
		return True

	def compareStream(self, streamType, messages):
		commands = [	
			'%s -i %s -show_packets -show_data -select_streams %s < /dev/null 2>/dev/null > %s' % (FFPROBE_BIN, self.tsFile1, streamType, self.ffprobeFile1),
			'%s -i %s -show_packets -show_data -select_streams %s < /dev/null 2>/dev/null > %s' % (FFPROBE_BIN, self.tsFile2, streamType, self.ffprobeFile2),
		]
		for command in commands:
			os.system(command)
		
		return self.compareFfprobeOutputs(self.ffprobeFile1, self.ffprobeFile2, streamType, messages)

	def testContinuity(self, tsFileName, continuityCounters):
		tsData = file(tsFileName, 'rb').read()
		okCounters = 0
		curPos = 0
		result = True
		while curPos < len(tsData):
			packetHeader = mpegTsHeader.parse(tsData[curPos:(curPos + TS_PACKET_LENGTH)])
			if continuityCounters.has_key(packetHeader.PID):
				lastValue = continuityCounters[packetHeader.PID]
				expectedValue = (lastValue + 1) % 16
				if packetHeader.continuityCounter != expectedValue:
					self.writeOutput('Error: bad continuity counter - pos=%s pid=%d exp=%s actual=%s' % 
						(curPos, packetHeader.PID, expectedValue, packetHeader.continuityCounter))
					result = False
				else:
					okCounters += 1
			continuityCounters[packetHeader.PID] = packetHeader.continuityCounter			
			curPos += TS_PACKET_LENGTH
		self.writeOutput('Info: validated %s counters' % okCounters)
		return result
		
	def retrieveUrl(self, url, fileName):
		startTime = time.time()
		try:
			r = urllib2.urlopen(urllib2.Request(url))
		except urllib2.HTTPError, e:
			return e.getcode()
		except urllib2.URLError, e:
			self.writeOutput('Error: request failed %s %s' % (url, e))
			return 0
		except BadStatusLine, e:
			self.writeOutput('Error: bad status line %s' % (url))
			return 0
			
		result = r.getcode()
		with file(fileName, 'wb') as w:
			shutil.copyfileobj(r,w)
		r.close()
		
		self.writeOutput('Info: get %s took %s, cksum %s' % (url, time.time() - startTime, commands.getoutput('cksum "%s"' % fileName)))
		fileSize = os.path.getsize(fileName)
		if r.info().getheader('content-length') != '%s' % fileSize:
			self.writeOutput('Error: %s content-length %s is different than the resulting file size %s' % (url, r.info().getheader('content-length'), fileSize))
			return 0
		return result

	def decryptTsSegment(self, fileName, aesKey, segmentIndex):
		cipher = AES.new(aesKey, AES.MODE_CBC, '\0' * 12 + struct.pack('>L', segmentIndex))
		try:
			decryptedTS = cipher.decrypt(file(fileName, 'rb').read())
		except ValueError:
			self.writeOutput('Error: cipher.decrypt failed')
			return False
		padLength = ord(decryptedTS[-1])
		if padLength > 16:
			self.writeOutput('Error: invalid pad length %s' % padLength)
			return False
		if decryptedTS[-padLength:] != chr(padLength) * padLength:
			self.writeOutput('Error: invalid padding')
			return False
		decryptedTS = decryptedTS[:-padLength]
		file(fileName, 'wb').write(decryptedTS)
		return True
		
	def compareTsUris(self, url1, url2, segmentIndex, aesKey, continuityCounters):
		result = True
		self.writeOutput('Info: comparing %s %s' % (url1, url2))

		code1 = self.retrieveUrl(url1, self.tsFile1)
		if code1 == 200:
			if aesKey != None:
				if not self.decryptTsSegment(self.tsFile1, aesKey, segmentIndex):
					return False
			
			if not self.testContinuity(self.tsFile1, continuityCounters):
				result = False
		else:
			continuityCounters.clear()
			
		for attempt in xrange(3):
			code2 = self.retrieveUrl(url2, self.tsFile2)
			
			if code1 == 200 and code2 == 404:
				self.writeOutput('Info: got different status codes %s vs %s (ts)' % (code1, code2))
				return True
			
			if code1 != code2:
				if code1 != 0 and code2 != 0:
					self.writeOutput('Error: got different status codes %s vs %s (ts)' % (code1, code2))
				return False
			
			messages = []
			curResult = True
			if not self.compareStream('a', messages):
				curResult = False
			if not self.compareStream('v', messages):
				curResult = False
			if curResult:
				break
			self.writeOutput('Info: got errors, retrying...')
		
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

	def getM3U8(self, url):
		request = urllib2.Request(url, headers={'Accept-encoding': 'gzip'})
		try:
			f = urllib2.urlopen(request)
		except urllib2.HTTPError, e:
			return e.getcode(), e.read()			
		except urllib2.URLError, e:
			self.writeOutput('Error: request failed %s %s' % (url, e))
			return 0, ''
		data = f.read()
		if f.info().get('Content-Encoding') == 'gzip':
			gzipFile = gzip.GzipFile(fileobj=StringIO(data))
			try:
				data = gzipFile.read()
			except IOError, e:
				self.writeOutput('Error: failed to decode gzip')
				data = ''
		return f.getcode(), data
		
	def runTest(self, uri):		
		url1 = URL1_BASE + random.choice(URL1_PREFIXES) + uri
		url2 = URL2_PREFIX + uri
		
		self.writeOutput('Info: testing %s %s' % (url1, url2))
		
		# avoid billing real partners
		url1 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url1)
		url2 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url2)
		
		# get the manifests
		code1, manifest1 = self.getM3U8(url1 + URL1_SUFFIX)
		code2, manifest2 = self.getM3U8(url2 + URL2_SUFFIX)
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
			except urllib2.HTTPError, e:
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

	def deleteIgnoreErrors(self, fileName):
		try:
			os.remove(fileName)
		except OSError:
			pass
			
	def cleanup(self):
		self.deleteIgnoreErrors(self.tsFile1)
		self.deleteIgnoreErrors(self.tsFile2)
		self.deleteIgnoreErrors(self.ffprobeFile1)
		self.deleteIgnoreErrors(self.ffprobeFile2)
		
if __name__ == '__main__':
	stress_base.main(TestThread, STOP_FILE)
