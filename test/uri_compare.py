from httplib import BadStatusLine
import stress_base
import urllib2
import socket
import re

from uri_compare_params import *

class TestThread(stress_base.TestThreadBase):
	def __init__(self, index, increment, stopFile):
		stress_base.TestThreadBase.__init__(self, index, increment, stopFile)

	def getURL(self, url):
		request = urllib2.Request(url)
		try:
			f = urllib2.urlopen(request)
			return f.getcode(), f.read()
		except urllib2.HTTPError, e:
			return e.getcode(), e.read()			
		except urllib2.URLError, e:
			self.writeOutput('Error: request failed %s %s' % (url, e))
			return 0, ''
		except BadStatusLine, e:
			self.writeOutput('Error: request failed %s %s' % (url, e))
			return 0, ''
		except socket.error, e:
			self.writeOutput('Error: got socket error %s %s' % (url, e))
			return 0, ''
		
	def runTest(self, uri):		
		url1 = URL1_BASE + uri
		url2 = URL2_BASE + uri
		
		self.writeOutput('Info: testing %s %s' % (url1, url2))

		# avoid billing real partners
		url1 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url1)
		url2 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url2)

		code1, body1 = self.getURL(url1)
		code2, body2 = self.getURL(url2)
		if code1 != code2:
			self.writeOutput('Error: got different status codes %s vs %s' % (code1, code2))
			return False
		
		if url1.split('?')[0].rsplit('.', 1)[-1] in set(['m3u8']):
			body1 = body1.replace(URL1_BASE, URL2_BASE)
			body1 = body1.replace('-a1-v1', '-v1-a1')
			body2 = body2.replace('-a1-v1', '-v1-a1')
			
		if body1 != body2:
			self.writeOutput('Error: comparison failed - url1=%s, url2=%s' % (url1, url2))
			return False
			
		return True

if __name__ == '__main__':
	stress_base.main(TestThread, STOP_FILE)
