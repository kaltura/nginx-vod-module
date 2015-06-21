from httplib import BadStatusLine
import email.utils as eut
import stress_base
import urlparse
import hashlib
import urllib2
import random
import base64
import socket
import time
import hmac
import re

from uri_compare_params import *

INGORED_HEADERS = set([
	'x-vod-me','x-vod-session',
	'x-me', 'x-kaltura-session', 
	'x-varnish', 
])

IGNORE_HEADER_VALUES = set([
	'server',
	'date',
	'content-length',		# ignore content length since we compare the buffer, the content length may be different due to different host name lengths
	'etag',					# derived from content length
])


class TestThread(stress_base.TestThreadBase):
	def __init__(self, index, increment, stopFile):
		stress_base.TestThreadBase.__init__(self, index, increment, stopFile)

	def getG2OHeaders(self, url):
		if not 'G2O_KEY' in globals():
			return {}
	
		parsedUrl = urlparse.urlsplit(url)
		uri = urlparse.urlunsplit(urlparse.SplitResult('', '', parsedUrl.path, parsedUrl.query, parsedUrl.fragment))
		
		expiry = '%s' % (int(time.time()) + 29)
		dataFields = [G2O_VERSION, G2O_GHOST_IP, G2O_CLIENT_IP, expiry, G2O_UNIQUE_ID, G2O_NONCE]
		data = ', '.join(dataFields)
		dig = hmac.new(G2O_KEY, msg=data + uri, digestmod=hashlib.sha256).digest()
		sign = base64.b64encode(dig)
		return {
			G2O_DATA_HEADER_NAME: data, 
			G2O_SIGN_HEADER_NAME: sign,
			}

	def parseHttpHeaders(self, headers):
		result = {}
		for header in headers:
			header = map(lambda y: y.strip(), header.split(':', 1))
			headerName = header[0].lower()
			headerValue = header[1] if len(header) > 1 else ''
			result.setdefault(headerName, [])
			result[headerName].append(headerValue)
		return result
		
	@staticmethod
	def parseHttpTime(timeStr):
		return time.mktime(eut.parsedate(timeStr))
		
	def compareHeaders(self, headers1, headers2):
		for headerName in INGORED_HEADERS:
			headers1.pop(headerName, None)
			headers2.pop(headerName, None)
		
		onlyIn1 = set(headers1.keys()) - set(headers2.keys())
		if len(onlyIn1) != 0:
			self.writeOutput('Error: headers %s found only in headers1' % (','.join(onlyIn1)))
			return False
		onlyIn2 = set(headers2.keys()) - set(headers1.keys())
		if len(onlyIn2) != 0 and onlyIn2 != set(['access-control-allow-origin']):		# allow CORS to be added
			self.writeOutput('Error: headers %s found only in headers2' % (','.join(onlyIn2)))
			return False
		for curHeader in headers1.keys():
			if curHeader in IGNORE_HEADER_VALUES:
				continue
			value1 = headers1[curHeader]
			value2 = headers2[curHeader]
			if value1 == value2:
				continue
			if curHeader in set(['expires', 'last-modified']):
				if abs(self.parseHttpTime(value1[0]) - self.parseHttpTime(value2[0])) < 10:
					continue
			self.writeOutput('Error: different value for header %s - %s vs %s' % (curHeader, value1, value2))
			return False
		return True

	def getURL(self, url):
		request = urllib2.Request(url, headers=self.getG2OHeaders(url))
		try:
			f = urllib2.urlopen(request)
			return f.getcode(), self.parseHttpHeaders(f.info().headers), f.read()
		except urllib2.HTTPError, e:
			return e.getcode(), self.parseHttpHeaders(e.info().headers), e.read()			
		except urllib2.URLError, e:
			self.writeOutput('Error: request failed %s %s' % (url, e))
			return 0, {}, ''
		except BadStatusLine, e:
			self.writeOutput('Error: request failed %s %s' % (url, e))
			return 0, {}, ''
		except socket.error, e:
			self.writeOutput('Error: got socket error %s %s' % (url, e))
			return 0, {}, ''
		
	def runTest(self, uri):
		urlBase1 = random.choice(URL1_BASE)
		urlBase2 = random.choice(URL2_BASE)
		url1 = urlBase1 + uri
		url2 = urlBase2 + uri
		
		self.writeOutput('Info: testing %s %s' % (url1, url2))

		# avoid billing real partners
		url1 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url1)
		url2 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url2)

		code1, headers1, body1 = self.getURL(url1)
		code2, headers2, body2 = self.getURL(url2)
		if code1 != code2:
			self.writeOutput('Error: got different status codes %s vs %s' % (code1, code2))
			return False
		
		if not self.compareHeaders(headers1, headers2):
			self.writeOutput('Error: got different headers %s vs %s' % (headers1, headers2))
			return False			
		
		if str(code1) != '200':
			self.writeOutput('Notice: got status code %s' % (code1))
			body1 = re.sub('nginx/\d+\.\d+\.\d+', 'nginx/0.0.0', body1)
			body2 = re.sub('nginx/\d+\.\d+\.\d+', 'nginx/0.0.0', body2)
		
		if url1.split('?')[0].rsplit('.', 1)[-1] in set(['m3u8', 'mpd']):
			body1 = body1.replace(urlBase1, urlBase2)
			body1 = body1.replace('-a1-v1', '-v1-a1')
			body2 = body2.replace('-a1-v1', '-v1-a1')
			# must strip CF tokens since they sign the domain
			body1 = re.sub('&Signature=[^&]+', '&Signature=', re.sub('\?Policy=[^&]+', '?Policy=', body1))
			body2 = re.sub('&Signature=[^&]+', '&Signature=', re.sub('\?Policy=[^&]+', '?Policy=', body2))
		
		if body1.startswith('<?xml'):
			body1 = re.sub('<executionTime>[0-9\.]+<\/executionTime>', '', body1)
			body2 = re.sub('<executionTime>[0-9\.]+<\/executionTime>', '', body2)
			
		if body1 != body2:
			self.writeOutput('Error: comparison failed - url1=%s, url2=%s' % (url1, url2))
			return False
			
		return True

if __name__ == '__main__':
	stress_base.main(TestThread, STOP_FILE)
