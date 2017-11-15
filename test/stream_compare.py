from urlparse import urlparse
import manifest_utils
import compare_utils
import stress_base
import http_utils
import random
import time
import re

from stream_compare_params import *

manifest_utils.CHUNK_LIST_ITEMS_TO_COMPARE = CHUNK_LIST_ITEMS_TO_COMPARE
def convertBody(body):
	try:
		return body.decode('ascii')
	except UnicodeDecodeError:
		return body[:100].encode('hex')


class TestThread(stress_base.TestThreadBase):

	def getURL(self, hostHeader, url):
		headers = {}
		headers.update(EXTRA_HEADERS)
		headers['Host'] = hostHeader
		code, headers, body = http_utils.getUrl(url, headers)
		if code == 0:
			self.writeOutput(body)
		return code, headers, body


	def compareUrls(self, hostHeader, url1, url2):

		for retry in xrange(URL_COMPARE_RETRIES):
			if retry != 0:
				time.sleep(URL_COMPARE_RETRIES_SLEEP_INTERVAL)

			if LOG_LEVEL['UrlCompareLog']:
				self.writeOutput('Compare %s with %s  (retry %d)' % (url1, url2, retry))

			code1, headers1, body1 = self.getURL(hostHeader, url1)
			code2, headers2, body2 = self.getURL(hostHeader, url2)
			if code1 != code2:
				self.writeOutput('Error: got different status codes %s vs %s, url1=%s, url2=%s' % (code1, code2, url1, url2))
				continue

			headerCompare = compare_utils.compareHeaders(headers1, headers2)
			if headerCompare != None:
				self.writeOutput(headerCompare)
				continue

			if str(code1) != '200':
				self.writeOutput('Notice: got status code %s, url1=%s, url2=%s' % (code1, url1, url2))

			if body1 != body2:
				if retry >= URL_COMPARE_RETRIES-1:
					severity = "Error"
				else:
					severity = "Notice"
				self.writeOutput('%s: comparison failed, url1=%s, url2=%s\n%s\n%s' % (severity, url1, url2, convertBody(body1), convertBody(body2)))
				continue

			return code1, headers1, body1

		return False
		
	def runTest(self, uri):
		hostHeader, uri = uri.split(' ')
	
		uri = uri.replace('@time@', str(int(time.time())))
		urlBase1 = random.choice(URL1_BASE)
		urlBase2 = random.choice(URL2_BASE)
		url1 = urlBase1 + uri
		url2 = urlBase2 + uri
		
		self.writeOutput('Info: testing %s %s' % (url1, url2))
		compareResult = self.compareUrls(hostHeader, url1, url2)
		if compareResult == False:
			return False
		code, headers, body = compareResult
		if str(code) != '200':
			return True
		
		mimeType = headers['content-type'][0]
		urls = manifest_utils.getManifestUrls(url1.rsplit('/', 1)[0], body, mimeType, {'Host':hostHeader})
		urls = map(lambda x: urlBase1 + urlparse(x).path, urls)		# the urls may contain the host header

		result = True
		for url in urls:
			if not self.compareUrls(hostHeader, url, url.replace(urlBase1, urlBase2)):
				result = False
		return result

if __name__ == '__main__':
	stress_base.main(TestThread, STOP_FILE)
