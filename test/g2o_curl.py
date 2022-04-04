from __future__ import print_function
import http_utils
import urlparse
import sys
import os

if len(sys.argv) < 2:
	print('Usage:\n\tpython %s [curl options] <url>' % os.path.basename(__file__))
	sys.exit(1)

switches = sys.argv[1:-1]
url = sys.argv[-1]

parsedUrl = urlparse.urlsplit(url)
if parsedUrl.scheme not in ['http', 'https']:
	url = 'http://' + url
	parsedUrl = urlparse.urlsplit(url)

uri = urlparse.urlunsplit(urlparse.SplitResult('','',parsedUrl.path,parsedUrl.query,parsedUrl.fragment))

headers = http_utils.getG2OHeaders(uri)
headers = map(lambda x: '-H "%s: %s"' % x, headers.items())

cmdLine = 'curl %s %s "%s"' % (' '.join(map(lambda x: "'%s'" % x, switches)), ' '.join(headers), url)
print('%s\n' % cmdLine)
os.system(cmdLine)
