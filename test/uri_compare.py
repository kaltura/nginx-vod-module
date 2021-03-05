import compare_utils
import stress_base
import http_utils
import random
import time
import re

from uri_compare_params import *

class TestThread(stress_base.TestThreadBase):

	def getURL(self, hostHeader, url, range = None):
		headers = {}
		headers.update(EXTRA_HEADERS)
		headers['Host'] = hostHeader
		headers['Accept-encoding'] = 'gzip'
		if range != None:
			headers['Range'] = 'bytes=%s' % range
		code, headers, body = http_utils.getUrl(url, headers)
		if code == 0:
			self.writeOutput(body)
		return code, headers, body
		
	def runTest(self, line):
		splittedLine = line.split(' ')
		if len(splittedLine) == 2:
			hostHeader, uri = splittedLine
			range = None
		elif len(splittedLine) == 3:
			range, hostHeader, uri = splittedLine
			range = range.split('/')[0]
		else:
			return True
	
		urlBase1 = random.choice(URL1_BASE)
		urlBase2 = random.choice(URL2_BASE)
		url1 = urlBase1 + uri
		url2 = urlBase2 + uri
		
		self.writeOutput('Info: testing %s %s' % (url1, url2))

		# avoid billing real partners
		useRealPartner = False
		for curPrefix in USE_REAL_PARTNER_PREFIXES:
			if uri.startswith(curPrefix):
				useRealPartner = True
		if not useRealPartner:
			url1 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url1)
			url2 = re.sub('/p/\d+/sp/\d+/', '/p/%s/sp/%s00/' % (TEST_PARTNER_ID, TEST_PARTNER_ID), url2)

		code1, headers1, body1 = self.getURL(hostHeader, url1, range)
		code2, headers2, body2 = self.getURL(hostHeader, url2, range)
		if code1 != code2:
			self.writeOutput('Error: got different status codes %s vs %s' % (code1, code2))
			return False
		
		headerCompare = compare_utils.compareHeaders(headers1, headers2)
		if headerCompare != None:
			self.writeOutput(headerCompare)
			return False
		
		if not str(code1) in ['200', '206']:
			self.writeOutput('Notice: got status code %s' % (code1))
			body1 = re.sub('nginx/\d+\.\d+\.\d+', 'nginx/0.0.0', body1)
			body2 = re.sub('nginx/\d+\.\d+\.\d+', 'nginx/0.0.0', body2)
		
		if (headers1.has_key('content-type') and 
			headers1['content-type'][0] in set(['application/vnd.apple.mpegurl', 'application/dash+xml'])):
			body1 = body1.replace(urlBase1, urlBase2)
			body1 = body1.replace('-a1-v1', '-v1-a1')
			body2 = body2.replace('-a1-v1', '-v1-a1')
			# must strip CF tokens since they sign the domain
			body1 = re.sub('&Signature=[^&]+', '&Signature=', re.sub('\?Policy=[^&]+', '?Policy=', body1))
			body2 = re.sub('&Signature=[^&]+', '&Signature=', re.sub('\?Policy=[^&]+', '?Policy=', body2))
		
		if body1.startswith('<?xml'):
			body1 = re.sub('<executionTime>[0-9\.]+<\/executionTime>', '', body1)
			body2 = re.sub('<executionTime>[0-9\.]+<\/executionTime>', '', body2)

		if body1.startswith('<html>'):
			body1 = body1.replace(' bgcolor="white"', '')
			body2 = body2.replace(' bgcolor="white"', '')

		if body1 != body2:
			self.writeOutput('Error: comparison failed - url1=%s, url2=%s' % (url1, url2))
			self.writeOutput(body1)
			self.writeOutput(body2)
			return False
			
		return True

if __name__ == '__main__':
	stress_base.main(TestThread, STOP_FILE)
