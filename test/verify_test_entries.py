import urllib2
import sys
import os

NGINX_LOG_PATH = '/var/log/nginx/error.log'
BASE_URL = 'http://localhost:8001/mapped/hls'

### Log tracker - used to verify certain lines appear in nginx log
class LogTracker:
    def __init__(self):
        self.initialSize = os.path.getsize(NGINX_LOG_PATH)

    def assertContains(self, logLine):
        f = file(NGINX_LOG_PATH, 'rb')
        f.seek(self.initialSize, os.SEEK_SET)
        buffer = f.read()
        f.close()
        if type(logLine) == list:
            found = False
            for curLine in logLine:
                if curLine in buffer:
                    found = True
                    break
            assert(found)
        else:
            assert(logLine in buffer)

for curLine in sys.stdin:
    curLine = curLine.strip()
    if len(curLine) == 0:
        break
    refId, uri, expectedStatusCode, message = curLine.split(' ', 3)
    if refId == 'TRUNCATED_MDAT':
        continue        # hangs
    print 'testing %s %s' % (refId, uri)
    logTracker = LogTracker()
    try:
        f = urllib2.urlopen(BASE_URL + uri)
        statusCode = f.getcode()
        f.read()
    except urllib2.HTTPError, e:
        statusCode = e.getcode()
    if message != 'None':
        logTracker.assertContains(message)
    assert(expectedStatusCode == str(statusCode))
