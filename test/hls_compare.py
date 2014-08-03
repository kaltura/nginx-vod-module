from threading import Thread
from threading import Lock
from Crypto.Cipher import AES
from StringIO import StringIO
import commands
import urllib2
import random
import struct
import time
import gzip
import sys
import os
import re

from hls_compare_params import *

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

	def compareTsUris(self, url1, url2, segmentIndex, aesKey):
		self.writeOutput('comparing %s %s' % (url1, url2))

		startTime = time.time()
		os.system("""curl -s '%s' < /dev/null > %s""" % (url1, self.tsFile1))
		self.writeOutput('get url1 took %s' % (time.time() - startTime))

		startTime = time.time()
		os.system("""curl -s '%s' < /dev/null > %s""" % (url2, self.tsFile2))
		self.writeOutput('get url2 took %s' % (time.time() - startTime))		
		
		if aesKey != None:
			cipher = AES.new(aesKey, AES.MODE_CBC, '\0' * 12 + struct.pack('>L', segmentIndex))
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

	def getM3U8(self, url):
		request = urllib2.Request(url, headers={'Accept-encoding': 'gzip'})
		f = urllib2.urlopen(request)
		data = f.read()
		if f.info().get('Content-Encoding') == 'gzip':
			gzipFile = gzip.GzipFile(fileobj=StringIO(data))
			try:
				data = gzipFile.read()
			except IOError, e:
				self.writeOutput('Failed to decode gzip')
				data = ''
		return f.getcode(), data
		
	def compareM3U8Uris(self, uri):
		self.writeOutput('Testing, uri=%s' % uri)
		
		# avoid billing real partners
		uri = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), uri)

		# get the manifests
		url1 = URL1_BASE + random.choice(URL1_PREFIXES) + uri
		url2 = URL2_PREFIX + uri
		
		code1, manifest1 = self.getM3U8(url1 + URL1_SUFFIX)
		code2, manifest2 = self.getM3U8(url2 + URL2_SUFFIX)
		if code1 != code2:
			self.writeOutput('got different status codes %s vs %s' % (code1, code2))
			return True

		if code1 == '404' and code2 == '404':
			self.writeOutput('both servers returned 404')
			return True
		
		# extract the ts uris
		tsUris1 = self.getTsUris(manifest1)
		tsUris2 = self.getTsUris(manifest2)
		if len(tsUris1) != len(tsUris2):
			if len(tsUris1) < len(tsUris2) and '/clipTo/' in uri:
				self.writeOutput('ignoring TS count mismatch (%s vs %s) due to clipTo parameter' % (len(tsUris1), len(tsUris2)))
				tsUris2 = tsUris2[:len(tsUris1)]
			else:
				self.writeOutput('TS segment count mismatch %s vs %s' % (len(tsUris1), len(tsUris2)))
				self.writeOutput('manifest1=%s' % manifest1)
				self.writeOutput('manifest2=%s' % manifest2)
				return False

		# get the encryption key, if exists
		keyUri = None
		for curLine in manifest1.split('\n'):
			if curLine.startswith('#EXT-X-KEY'):
				keyUri = curLine.split('"')[1]
		if keyUri != None:
			url = url1 + URL1_SUFFIX
			baseUrl = url[:url.rfind('/')] + '/'
			aesKey = urllib2.urlopen(baseUrl + keyUri).read()
		else:
			aesKey = None

		# compare the ts segments
		self.writeOutput('segmentCount=%s' % len(tsUris1))
		for curIndex in xrange(len(tsUris1)):
			if os.path.exists(STOP_FILE):
				return True
			tsUrl1 = '%s/%s' % (url1, tsUris1[curIndex])
			tsUrl2 = '%s/%s' % (url2, tsUris2[curIndex])
			if not self.compareTsUris(tsUrl1, tsUrl2, curIndex + 1, aesKey):
				self.writeOutput('Error, url1=%s, url2=%s' % (url1, url2))
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
	