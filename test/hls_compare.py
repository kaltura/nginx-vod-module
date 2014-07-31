from threading import Thread
from threading import Lock
from Crypto.Cipher import AES
import commands
import urllib
import time
import sys
import os
import re

# to generate the input file:
# 	zgrep serveFlavor /data/logs/investigate/2014/07/14/??-front-origin*access_log* | grep kalturavod-vh | awk '{print $7}' | sort -u > /tmp/serveFlavorUris.txt

URL1_PREFIX = 'http://localhost:4321'					# nginx vod
URL1_SUFFIX = '/index.m3u8'
URL1_ENCRYPTION_KEY_SUFFIX = '' #'/encryption.key'
URL2_PREFIX = 'http://kalturavod-i.akamaihd.net/i'		# akamai
URL2_SUFFIX = '/index_0_av.m3u8'
FFPROBE_BIN = '/web/content/shared/bin/ffmpeg-2.1.3-bin/ffprobe-2.1.3.sh'
TEST_PARTNER_ID = '437481'

STOP_FILE = '/tmp/compare_stop'
TEMP_TS_FILE1 = '/tmp/1.ts'
TEMP_TS_FILE2 = '/tmp/2.ts'
TEMP_FFPROBE_FILE1 = '/tmp/1.ts.ffp'
TEMP_FFPROBE_FILE2 = '/tmp/2.ts.ffp'

PTS_THRESHOLD = 1000		# 1/90 sec
DTS_THRESHOLD = 1000		# 1/90 sec
IGNORED_PREFIXES = ['pos=', 'pts_time=', 'dts_time=']

outputLock = Lock()

def writeOutput(msg, index = -1):
	outputLock.acquire()
	sys.stdout.write('[TID%s] %s\n' % (index, msg))
	sys.stdout.flush()
	outputLock.release()

class TestThread(Thread):
	def __init__(self, index, increment):
		Thread.__init__(self)
		self.index = index
		self.increment = increment
		self.tsFile1 = '%s.%s' % (TEMP_TS_FILE1, index)
		self.tsFile2 = '%s.%s' % (TEMP_TS_FILE2, index)
		self.ffprobeFile1 = '%s.%s' % (TEMP_FFPROBE_FILE1, index)
		self.ffprobeFile2 = '%s.%s' % (TEMP_FFPROBE_FILE2, index)

	def run(self):
		self.writeOutput('Started')
		index = self.index
		while not os.path.exists(STOP_FILE) and index < len(requests):
			if not self.compareM3U8Uris(requests[index]):
				pass		#break		# change to debug issues that dont reproduce
			index += self.increment
		self.writeOutput('Done')
		
	def writeOutput(self, msg):
		writeOutput(msg, self.index)
		
	def compareFfprobeOutputs(self, file1, file2):
		lines1 = file(file1, 'rb').readlines()
		lines2 = file(file2, 'rb').readlines()

		if len(lines1) != len(lines2):
			self.writeOutput('line count mismatch %s vs %s' % (len(lines1), len(lines2)))
			os.system("""cat %s < /dev/null | grep -v ^pts= | grep -v ^dts= | grep -v ^pos= | grep -v ^pts_time= | grep -v ^dts_time= | sed 's/00000000: 0000 0001 09f0/00000000: 0000 0001 09e0/g' > %s.tmp""" % (file1, file1))
			os.system("""cat %s < /dev/null | grep -v ^pts= | grep -v ^dts= | grep -v ^pos= | grep -v ^pts_time= | grep -v ^dts_time= | sed 's/00000000: 0000 0001 09f0/00000000: 0000 0001 09e0/g' > %s.tmp""" % (file2, file2))
			self.writeOutput(commands.getoutput('diff -bBu %s.tmp %s.tmp < /dev/null | head -1000' % (file1, file2)))
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
					self.writeOutput('pts diff %s exceeds threshold, pts1=%s pts2=%s diff=%s' % (curDiff, pts1, pts2, ptsDiff))
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
					self.writeOutput('dts diff %s exceeds threshold, dts1=%s dts2=%s diff=%s' % (curDiff, dts1, dts2, dtsDiff))
					return False
				continue
			
			# AUD packet
			line1 = line1.replace('00000000: 0000 0001 09f0', '00000000: 0000 0001 09e0')
			line2 = line2.replace('00000000: 0000 0001 09f0', '00000000: 0000 0001 09e0')
			if line1 == line2:
				continue
			
			self.writeOutput('test failed line1=%s line2=%s' % (line1, line2))
			return False
		return True

	def compareStream(self, streamType):
		commands = [	
			'%s -i %s -show_packets -show_data -select_streams %s < /dev/null 2>/dev/null > %s' % (FFPROBE_BIN, self.tsFile1, streamType, self.ffprobeFile1),
			'%s -i %s -show_packets -show_data -select_streams %s < /dev/null 2>/dev/null > %s' % (FFPROBE_BIN, self.tsFile2, streamType, self.ffprobeFile2),
		]
		for command in commands:
			os.system(command)
		
		return self.compareFfprobeOutputs(self.ffprobeFile1, self.ffprobeFile2)

	def compareTsUris(self, uri1, uri2, segmentIndex):
		self.writeOutput('comparing %s %s' % (uri1, uri2))

		# avoid billing any real partners
		uri1 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), uri1)
		uri2 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), uri2)

		startTime = time.time()
		os.system("""curl -s '%s%s' < /dev/null > %s""" % (URL1_PREFIX, uri1, self.tsFile1))
		self.writeOutput('get uri1 took %s' % (time.time() - startTime))

		startTime = time.time()
		os.system("""curl -s '%s%s' < /dev/null > %s""" % (URL2_PREFIX, uri2, self.tsFile2))
		self.writeOutput('get uri2 took %s' % (time.time() - startTime))		
		
		if len(URL1_ENCRYPTION_KEY_SUFFIX) != 0:
			encryptionKeyUrl = '%s%s%s' % (URL1_PREFIX, url1[:url1.rfind('/')], URL1_ENCRYPTION_KEY_SUFFIX)
			encryptionKey = urllib.urlopen(encryptionKeyUrl).read()
			cipher = AES.new(encryptionKey, AES.MODE_CBC, '\0' * 12 + struct.pack('>L', segmentIndex))
			decryptedTS = cipher.decrypt(file(self.tsFile1, 'rb').read())
			padLength = ord(decryptedTS[-1])
			decryptedTS = decryptedTS[:-padLength]
			file(self.tsFile1, 'wb').write(decryptedTS)
		
		return self.compareStream('a') and self.compareStream('v')

	@staticmethod		
	def getTsUris(manifest):
		result = []
		for curLine in manifest.split('\n'):
			curLine = curLine.strip()
			if len(curLine) > 0 and curLine[0] != '#':
				result.append(curLine)
		return result

	def compareM3U8Uris(self, uri):
		self.writeOutput('Testing, uri=%s' % uri)
		manifest1 = urllib.urlopen(URL1_PREFIX + uri + URL1_SUFFIX).read()
		manifest2 = urllib.urlopen(URL2_PREFIX + uri + URL2_SUFFIX).read()
		tsUris1 = self.getTsUris(manifest1)
		tsUris2 = self.getTsUris(manifest2)
		if len(tsUris1) != len(tsUris2):
			self.writeOutput('TS segment count mismatch %s vs %s' % (len(tsUris1), len(tsUris2)))
			self.writeOutput('manifest1=%s' % manifest1)
			self.writeOutput('manifest2=%s' % manifest2)
			return False
		self.writeOutput('segmentCount=%s' % len(tsUris1))
		for curIndex in xrange(len(tsUris1)):
			if os.path.exists(STOP_FILE):
				return True
			uri1 = '%s/%s' % (uri, tsUris1[curIndex])
			uri2 = '%s/%s' % (uri, tsUris2[curIndex])
			if not self.compareTsUris(uri1, uri2, curIndex + 1):
				self.writeOutput('Error, uri1=%s, uri2=%s' % (uri1, uri2))
				return False
		self.writeOutput('Success')
		return True

if __name__ == '__main__':
	if len(sys.argv) != 3:
		writeOutput('Usage:\n\t%s <uri file> <thread count>' % os.path.basename(__file__))
		sys.exit(1)
	
	(_, inputFile, threadCount) = sys.argv
	threadCount = int(threadCount)
	
	sys.stderr.write('touch %s to stop\n' % STOP_FILE)
	try:
		os.remove(STOP_FILE)
	except OSError:
		pass
	
	# read all the request
	requests = []
	for inputLine in file(inputFile):
		inputLine = inputLine.strip()
		if len(inputLine) == 0 or inputLine[0] == '#':
			continue
		requests.append(inputLine)
	
	# create the threads
	threads = []
	for curThreadIdx in range(threadCount):
		curThread = TestThread(curThreadIdx, threadCount)
		threads.append(curThread)

	# start the threads
	for curThread in threads:
		curThread.start()

	writeOutput('Finished launching %s threads' % threadCount)

	# wait on the threads
	threadAlive = True
	while threadAlive:
		threadAlive = False
		for thread in threads:
			if thread.isAlive():
				threadAlive = True
		time.sleep(5)

	writeOutput('Done !')
	