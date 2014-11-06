import urllib2
import time
import sys

# zgrep /hls /data/logs/investigate/2014/10/30/vod/??-nginx-*-access_log* | awk '{print $7}' > /tmp/nginxVodUris.txt

BASE_URL = 'http://localhost:8001/mapped'

def getUrl(url):
	request = urllib2.Request(url)
	try:
		f = urllib2.urlopen(request)
	except urllib2.HTTPError, e:
		return e.getcode()			
	except urllib2.URLError, e:
		return 0
	return f.getcode()

limit = int(sys.argv[2])
count = 0
countByStatus = {}
startTime = time.time()
for curLine in file(sys.argv[1]):
	if count >= limit:
		break
	count += 1
	statusCode = getUrl(BASE_URL + curLine.strip())
	countByStatus.setdefault(statusCode, 0)
	countByStatus[statusCode] += 1
print 'Done, took %s' % (time.time() - startTime)
print countByStatus
