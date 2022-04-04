from httplib import BadStatusLine, IncompleteRead, InvalidURL
from StringIO import StringIO
from g2o_params import *
import urlparse
import hashlib
import urllib2
import base64
import socket
import shutil
import hmac
import time
import gzip

def parseHttpHeaders(headers):
	result = {}
	for header in headers:
		header = map(lambda y: y.strip(), header.split(':', 1))
		headerName = header[0].lower()
		headerValue = header[1] if len(header) > 1 else ''
		result.setdefault(headerName, [])
		result[headerName].append(headerValue)
	return result

def readDecode(f):
	body = f.read()
	if f.info().get('Content-Encoding') != 'gzip':
		return body

	gzipFile = gzip.GzipFile(fileobj=StringIO(body))
	try:
		return gzipFile.read()
	except IOError as e:
		return 'Error: failed to decode gzip'

def getUrl(url, extraHeaders={}):
	headers = getG2OHeaderFullUrl(url)
	headers.update(extraHeaders)
	request = urllib2.Request(url, headers=headers)
	try:
		f = urllib2.urlopen(request)
		body = readDecode(f)
	except urllib2.HTTPError as e:
		return e.getcode(), parseHttpHeaders(e.info().headers), readDecode(e)
	except urllib2.URLError as e:
		return 0, {}, 'Error: request failed %s %s' % (url, e)
	except BadStatusLine as e:
		return 0, {}, 'Error: request failed %s %s' % (url, e)
	except socket.error as e:
		return 0, {}, 'Error: got socket error %s %s' % (url, e)
	except IncompleteRead as e:
		return 0, {}, 'Error: got incomplete read error %s %s' % (url, e)
	except InvalidURL as e:
		return 0, {}, 'Error: got invalid url error %s %s' % (url, e)

	# validate content length
	contentLength = f.info().getheader('content-length')
	if contentLength != None and contentLength != '%s' % len(body):
		return 0, {}, 'Error: %s content-length %s is different than the resulting file size %s' % (url, contentLength, len(body))
		
	return f.getcode(), parseHttpHeaders(f.info().headers), body

def downloadUrl(url, fileName):
	r = urllib2.urlopen(urllib2.Request(url))
	with file(fileName, 'wb') as w:
		shutil.copyfileobj(r,w)
	r.close()
	
def getG2OHeaders(uri):
	if len(G2O_KEY) == 0:
		return {}

	expiry = '%s' % (int(time.time()) + G2O_WINDOW)
	dataFields = [G2O_VERSION, G2O_GHOST_IP, G2O_CLIENT_IP, expiry, G2O_UNIQUE_ID, G2O_NONCE]
	data = ', '.join(dataFields)
	dig = hmac.new(G2O_KEY, msg=data + uri, digestmod=hashlib.sha256).digest()
	sign = base64.b64encode(dig)
	return {
		G2O_DATA_HEADER_NAME: data, 
		G2O_SIGN_HEADER_NAME: sign,
		}

def getG2OHeaderFullUrl(url):
	parsedUrl = urlparse.urlsplit(url)
	uri = urlparse.urlunsplit(urlparse.SplitResult('', '', parsedUrl.path, parsedUrl.query, parsedUrl.fragment))
	return getG2OHeaders(uri)
