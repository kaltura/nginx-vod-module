from Crypto.Cipher import AES
import urllib
import sys
import os

# to generate the input file:
# 	zgrep serveFlavor /data/logs/investigate/2014/07/14/??-front-origin*access_log* | grep kalturavod-vh | awk '{print $7}' | sort -u > /tmp/serveFlavorUris.txt

URL1_PREFIX = 'http://localhost:4321'					# nginx vod
URL1_SUFFIX = '/index.m3u8'
URL1_ENCRYPTION_KEY_SUFFIX = '' #'/encryption.key'
URL2_PREFIX = 'http://kalturavod-i.akamaihd.net/i'		# akamai
URL2_SUFFIX = '/index_0_av.m3u8'
FFPROBE_BIN = '/web/content/shared/bin/ffmpeg-2.1.3-bin/ffprobe-2.1.3.sh'

TEMP_TS_FILE1 = '/tmp/1.ts'
TEMP_TS_FILE2 = '/tmp/2.ts'
TEMP_TS_FILE1_DEC = '/tmp/1.ts.dec'
TEMP_FFPROBE_FILE1 = '/tmp/1.ts.ffp'
TEMP_FFPROBE_FILE2 = '/tmp/2.ts.ffp'

PTS_THRESHOLD = 1000		# 1/90 sec
DTS_THRESHOLD = 1000		# 1/90 sec
IGNORED_PREFIXES = ['pos=', 'pts_time=', 'dts_time=']

def compareFfprobeOutputs(file1, file2):
	lines1 = file(file1, 'rb').readlines()
	lines2 = file(file2, 'rb').readlines()

	if len(lines1) != len(lines2):
		print 'line count mismatch %s vs %s' % (len(lines1), len(lines2))
		os.system("""cat %s | grep -v ^pts= | grep -v ^dts= | grep -v ^pos= | grep -v ^pts_time= | grep -v ^dts_time= | sed 's/00000000: 0000 0001 09f0/00000000: 0000 0001 09e0/g' > %s""" % (file1, file1))
		os.system("""cat %s | grep -v ^pts= | grep -v ^dts= | grep -v ^pos= | grep -v ^pts_time= | grep -v ^dts_time= | sed 's/00000000: 0000 0001 09f0/00000000: 0000 0001 09e0/g' > %s""" % (file2, file2))
		os.system('diff -bBu %s %s' % (file1, file2))
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
				print 'pts diff %s exceeds threshold, pts1=%s pts2=%s diff=%s' % (curDiff, pts1, pts2, ptsDiff)
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
				print 'dts diff %s exceeds threshold, dts1=%s dts2=%s diff=%s' % (curDiff, dts1, dts2, dtsDiff)
				return False
			continue
		
		# AUD packet
		line1 = line1.replace('00000000: 0000 0001 09f0', '00000000: 0000 0001 09e0')
		line2 = line2.replace('00000000: 0000 0001 09f0', '00000000: 0000 0001 09e0')
		if line1 == line2:
			continue
		
		print 'test failed line1=%s line2=%s' % (line1, line2)
		return False
	return True

def compareTsUris(uri1, uri2, segmentIndex):
	print 'comparing %s %s' % (uri1, uri2)
	commands = [
		'curl -s %s%s > %s' % (URL1_PREFIX, uri1, TEMP_TS_FILE1),
		'curl -s %s%s > %s' % (URL2_PREFIX, uri2, TEMP_TS_FILE2),
	]
	for command in commands:
		os.system(command)
	
	if len(URL1_ENCRYPTION_KEY_SUFFIX) != 0:
		encryptionKeyUrl = '%s%s%s' % (URL1_PREFIX, url1[:url1.rfind('/')], URL1_ENCRYPTION_KEY_SUFFIX)
		encryptionKey = urllib.urlopen(encryptionKeyUrl).read()
		cipher = AES.new(encryptionKey, AES.MODE_CBC, '\0' * 12 + struct.pack('>L', segmentIndex))
		decryptedTS = cipher.decrypt(file(TEMP_TS_FILE1, 'rb').read())
		padLength = ord(decryptedTS[-1])
		decryptedTS = decryptedTS[:-padLength]
		file(TEMP_TS_FILE1, 'wb').write(decryptedTS)
		
	commands = [	
		'%s -i %s -show_packets -show_data -select_streams a 2>/dev/null > %s' % (FFPROBE_BIN, TEMP_TS_FILE1, TEMP_FFPROBE_FILE1),
		'%s -i %s -show_packets -show_data -select_streams a 2>/dev/null > %s' % (FFPROBE_BIN, TEMP_TS_FILE2, TEMP_FFPROBE_FILE2),
	]
	for command in commands:
		os.system(command)
	
	return compareFfprobeOutputs(TEMP_FFPROBE_FILE1, TEMP_FFPROBE_FILE2)
	
def getTsUris(manifest):
	result = []
	for curLine in manifest.split('\n'):
		curLine = curLine.strip()
		if len(curLine) > 0 and curLine[0] != '#':
			result.append(curLine)
	return result

def compareM3U8Uris(uri):
	print 'Testing, uri=%s' % uri,
	manifest1 = urllib.urlopen(URL1_PREFIX + uri + URL1_SUFFIX).read()
	manifest2 = urllib.urlopen(URL2_PREFIX + uri + URL2_SUFFIX).read()
	tsUris1 = getTsUris(manifest1)
	tsUris2 = getTsUris(manifest2)
	if len(tsUris1) != len(tsUris2):
		print 'TS segment count mismatch %s vs %s' % (len(tsUris1), len(tsUris2))
		print 'manifest1=%s' % manifest1
		print 'manifest2=%s' % manifest2
		return False
	print ' segmentCount=%s' % len(tsUris1)
	for curIndex in xrange(len(tsUris1)):
		uri1 = '%s/%s' % (uri, tsUris1[curIndex])
		uri2 = '%s/%s' % (uri, tsUris2[curIndex])
		if not compareTsUris(uri1, uri2, curIndex + 1):
			print 'Error, uri1=%s, uri2=%s' % (uri1, uri2)
			return False
	print 'Success'
	return True

if __name__ == '__main__':
	if len(sys.argv) != 2:
		print 'Usage:\n\t%s <uri file>' % os.path.basename(__file__)
		sys.exit(1)
	
	for inputLine in file(sys.argv[1]):
		inputLine = inputLine.strip()
		if len(inputLine) == 0:
			continue
		compareM3U8Uris(inputLine)
