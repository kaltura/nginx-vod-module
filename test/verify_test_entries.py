import urllib2
import httplib
import sys
import os

# Note: need to disable the moov atom cache and the response cache when running the tests

NGINX_LOG_PATH = '/var/log/nginx/error.log'
BASE_URL = 'http://localhost:8001/mapped'

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
    splittedLine = curLine.split(' ', 3)
    if len(splittedLine) < 4:
        continue
    refId, uri, expectedStatusCode, message = splittedLine
    print 'testing %s %s' % (refId, uri)
    logTracker = LogTracker()
    try:
        f = urllib2.urlopen(BASE_URL + uri)
        statusCode = f.getcode()
        f.read()
    except urllib2.HTTPError, e:
        statusCode = e.getcode()
    except httplib.BadStatusLine:
        statusCode = 0
    if message != 'None':
        logTracker.assertContains(message)
    assert(expectedStatusCode == str(statusCode))
