from threading import Thread
from Crypto.Cipher import AES
import urlparse
import urllib2
import struct
import socket
import random
import time
import os

# environment specific parameters
from main_params import *

RUN_ONLY_PATH = ''       # use for running only some tests, e.g. MainTestSuite.RemoteTestSuite.MemoryUpstreamTestSuite

# nginx config derivatives
NGINX_LOG_PATH = '/var/log/nginx/error.log'
NGINX_HOST = 'http://localhost:8001'
API_SERVER_PORT = 8002
FALLBACK_PORT = 8003
NGINX_LOCAL = NGINX_HOST + '/tlocal'
NGINX_MAPPED = NGINX_HOST + '/tmapped'
NGINX_REMOTE = NGINX_HOST + '/tremote'
ENCRYPTED_PREFIX = '/enc'
HLS_PLAYLIST_FILE = '/index.m3u8'
HLS_IFRAMES_FILE = '/iframes.m3u8'
HLS_SEGMENT_FILE = '/seg-1-a1-v1.ts'
M3U8_PREFIX = '''#EXTM3U
#EXT-X-TARGETDURATION:10
#EXT-X-ALLOW-CACHE:YES
#EXT-X-VERSION:2
#EXT-X-MEDIA-SEQUENCE:1
'''

M3U8_PREFIX_ENCRYPTED = '''#EXTM3U
#EXT-X-TARGETDURATION:10
#EXT-X-ALLOW-CACHE:YES
#EXT-X-KEY:METHOD=AES-128,URI="encryption.key"
#EXT-X-VERSION:2
#EXT-X-MEDIA-SEQUENCE:1
'''
M3U8_POSTFIX = '#EXT-X-ENDLIST\n'
M3U8_EXTINF = '#EXTINF:'
M3U8_SEGMENT_PREFIX = 'seg-'
M3U8_SEGMENT_POSTFIX = '.ts'
SERVER_NAME = 'localhost'

HLS_REQUESTS = [
    (HLS_PLAYLIST_FILE, 'application/vnd.apple.mpegurl'),
    (HLS_IFRAMES_FILE, 'application/vnd.apple.mpegurl'),
    (HLS_SEGMENT_FILE, 'video/MP2T')]

VOD_REQUESTS = HLS_REQUESTS + [
    ('', 'video/mp4')]      # '' returns the full file

### Assertions
def assertEquals(v1, v2):
    if v1 == v2:
        return
    print 'Assertion failed - %s != %s' % (v1, v2)
    assert(False)

def assertIn(needle, haystack):
    if needle in haystack:
        return
    print 'Assertion failed - %s in %s' % (needle, haystack)
    assert(False)

def assertInIgnoreCase(needle, haystack):
    assertIn(needle.lower(), haystack.lower())

def assertStartsWith(buffer, prefix):
    if buffer.startswith(prefix):
        return
    print 'Assertion failed - %s.startswith(%s)' % (buffer, prefix)
    assert(False)
    
def assertEndsWith(buffer, postfix):
    if buffer.endswith(postfix):
        return
    print 'Assertion failed - %s.endswith(%s)' % (buffer, postfix)
    assert(False)

def assertRequestFails(url, statusCode, expectedBody = None, headers = {}, postData = None):
    request = urllib2.Request(url, headers=headers)
    try:
        response = urllib2.urlopen(request, data=postData)
        assert(False)
    except urllib2.HTTPError, e:
        assertEquals(e.getcode(), statusCode)
        if expectedBody != None:
            assertEquals(expectedBody, e.read())

def validatePlaylistM3U8(buffer):
    if buffer.startswith(M3U8_PREFIX_ENCRYPTED):
        buffer = buffer[len(M3U8_PREFIX_ENCRYPTED):]
    else:
        assertStartsWith(buffer, M3U8_PREFIX)
        buffer = buffer[len(M3U8_PREFIX):]
    assertEndsWith(buffer, M3U8_POSTFIX)
    expectExtInf = True
    for curLine in buffer[:-len(M3U8_POSTFIX)].split('\n'):
        if len(curLine) == 0:
            continue
        if expectExtInf:
            assertStartsWith(curLine, M3U8_EXTINF)
        else:
            assertStartsWith(curLine, M3U8_SEGMENT_PREFIX)
            assertEndsWith(curLine, M3U8_SEGMENT_POSTFIX)
        expectExtInf = not expectExtInf    

### Cleanup stack - enables automatic cleanup after a test is run
class CleanupStack:
    def __init__(self):
        self.items = []
        
    def push(self, callback):
        self.items.append(callback)

    def resetAndDestroy(self):
        for i in xrange(len(self.items), 0 , -1):
            self.items[i - 1]()
        self.items = []

cleanupStack = CleanupStack()

### Log tracker - used to verify certain lines appear in nginx log
class LogTracker:
    def __init__(self):
        self.initialSize = os.path.getsize(NGINX_LOG_PATH)

    def assertContains(self, logLine):
        f = file(NGINX_LOG_PATH, 'rb')
        f.seek(self.initialSize, os.SEEK_SET)
        buffer = f.read()
        f.close()
        assert(logLine in buffer)

### Misc utility functions
def getHttpResponse(body = '', status = '200 OK', length = None, headers = {}):
    if length == None:
        length = len(body)
    headersStr = ''
    if len(headers) > 0:
        for curHeader in headers.items():
            headersStr +='%s: %s\r\n' % curHeader
    return 'HTTP/1.1 %s\r\nContent-Length: %s\r\n%s\r\n%s' % (status, length, headersStr, body)

def getPathMappingResponse(path):
    return getHttpResponse('<?xml version="1.0" encoding="utf-8"?><xml><result>%s</result></xml>' % path)

def getUniqueUrl(baseUrl, path1, path2 = ''):
    return '%s%s/rand/%s%s' % (baseUrl, path1, random.randint(0, 0x100000), path2)

def createRandomSymLink(sourcePath):
    linkPath = TEST_LINK_BASE_PATH + '/testlink_%s.mp4' % random.randint(0, 0x100000)
    linkFullPath = TEST_FILES_ROOT + linkPath
    os.symlink(sourcePath, linkFullPath)
    cleanupStack.push(lambda: os.unlink(linkFullPath))
    return linkPath

### Socket functions
def createTcpServer(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)     # prevent Address already in use errors
    s.bind(('0.0.0.0', port))
    s.listen(5)
    cleanupStack.push(s.close)
    return s

def socketSendRegular(s, msg):
    totalSent = 0
    while totalSent < len(msg):
        sent = s.send(msg[totalSent:])
        if sent == 0:
            raise RuntimeError("socket connection broken")
        totalSent += sent

def socketSendByteByByte(s, msg):
    if len(msg) > 16 * 1024:     # we may send big buffers when we serve files, disable it there since it takes forever
        socketSendRegular(s, msg)
        return
    for curByte in msg:
        try:
            if s.send(curByte) == 0:
                raise RuntimeError("socket connection broken")
        except socket.error:        # the server may terminate the connection due to bad data in some tests
            break

def socketSendAndShutdown(s, msg):
    socketSend(s, msg)
    try:
        s.shutdown(socket.SHUT_WR)
    except socket.error:        # the server may terminate the connection due to bad data in some tests
        pass

def socketSendAndWait(s, msg, sleep):
    socketSend(s, msg)
    time.sleep(sleep)

def socketReadHttpHeaders(s):
    buffer = ''
    while not '\r\n\r\n' in buffer:
        curChunk = s.recv(1024)
        if len(curChunk) == 0:
            raise RuntimeError("socket connection broken")
        buffer += curChunk
    return buffer

def getHttpHeader(headers, header):
    header = header.lower()
    for curHeader in headers.split('\n'):
        splittedHeader = curHeader.split(':')
        if splittedHeader[0].strip().lower() == header:
            return splittedHeader[1].strip()
    return None

def socketExpectHttpVerbAndSend(s, verb, msg):
    buffer = socketReadHttpHeaders(s)
    assertStartsWith(buffer, verb)
    socketSendAndShutdown(s, msg)

def socketExpectHttpHeaderAndHandle(s, header, handler):
    headers = socketReadHttpHeaders(s)
    assertInIgnoreCase(header, headers)
    handler(s, headers)

def socketExpectHttpHeaderAndSend(s, header, msg):
    socketExpectHttpHeaderAndHandle(s, header, lambda s, h: socketSendAndShutdown(s, msg))

def socketReadHttpResponse(s):
    # get content length
    headers = socketReadHttpHeaders(s)
    contentLength = getHttpHeader(headers, 'content-length')
    assert(contentLength != None)
    contentLength = int(contentLength)

    # read body
    body = headers.split('\r\n\r\n', 1)[1]
    while len(body) < contentLength:
        curChunk = s.recv(1024)
        if len(curChunk) == 0:
            raise RuntimeError("socket connection broken")
        body += curChunk
    return body

def sendHttp10Request(url):
    parsedUrl = urlparse.urlparse(url)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((parsedUrl.hostname, parsedUrl.port if parsedUrl.port else 80))
    socketSend(s, 'GET %s HTTP/1.0\r\n\r\n' % parsedUrl.path)
    result = socketReadHttpResponse(s)
    s.close()
    return result

def serveFileHandler(s, path, mimeType, headers):
    # handle head
    if headers.startswith('HEAD'):
        socketSendAndShutdown(s, getHttpResponse(length=os.path.getsize(path), headers={'Content-Type':mimeType}))
        return
    
    # get the request body
    f = file(path, 'rb')
    range = getHttpHeader(headers, 'range')
    if range != None:
        assertStartsWith(range, 'bytes=')
        range = range.split('=', 1)[1]
        startOffset, endOffset = map(lambda x: int(x.strip()), range.split('-', 1))
        f.seek(startOffset, os.SEEK_SET)
        body = f.read(endOffset - startOffset + 1)
        status = '206 Partial Content'
    else:
        body = f.read()
        status = '200 OK'
    f.close()

    # build the response
    socketSendAndShutdown(s, getHttpResponse(body, status, headers={'Content-Type':mimeType}))

def serveFile(s, path, mimeType):
    headers = socketReadHttpHeaders(s)
    serveFileHandler(s, path, mimeType, headers)

### TCP server
class TcpServer(Thread):

    def __init__(self, port, callback):
        Thread.__init__(self)
        self.port = port
        self.callback = callback
        self.keepRunning = True
        self.serverSocket = createTcpServer(port)
        self.start()
        cleanupStack.push(self.stopServer)

    def run(self):
        while self.keepRunning:
            (clientsocket, address) = self.serverSocket.accept()
            if self.keepRunning:
                self.callback(clientsocket)
            clientsocket.close()

    def stopServer(self):
        self.keepRunning = False
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(('127.0.0.1', self.port))     # release the accept call
        s.close()
        while self.isAlive():
            time.sleep(.1)

### Test suite base class
class TestSuite(object):
    level = 0
    curPath = ''
    
    def __init__(self):
        self.prepareTest = None

    @staticmethod
    def getIndent():
        return '  ' * TestSuite.level

    def run(self):
        print '%sRunning suite %s' % (TestSuite.getIndent(), self.__class__.__name__)
        TestSuite.level += 1
        TestSuite.curPath += self.__class__.__name__ + '.'
        self.runChildSuites()
        for curFunc in dir(self):
            if not curFunc.startswith('test'):
                continue
            curTestPath = TestSuite.curPath + curFunc
            if len(RUN_ONLY_PATH) != 0 and not curTestPath.startswith(RUN_ONLY_PATH):
                continue
            if self.prepareTest != None:
                self.prepareTest()
            self.logTracker = LogTracker()
            print '%sRunning %s' % (TestSuite.getIndent(), curFunc)
            getattr(self, curFunc)()
            cleanupStack.resetAndDestroy()
        TestSuite.curPath = TestSuite.curPath[:-(len(self.__class__.__name__) + 1)]
        TestSuite.level -= 1

    def runChildSuites(self):
        pass

### Test suites
class BasicTestSuite(TestSuite):
    def __init__(self, getUrl, setupServer):
        super(BasicTestSuite, self).__init__()
        self.getUrl = getUrl
        self.prepareTest = setupServer

    # sanity
    def testHeadRequestSanity(self):
        for curRequest, contentType in VOD_REQUESTS:
            fullResponse = urllib2.urlopen(self.getUrl(curRequest)).read()
            request = urllib2.Request(self.getUrl(curRequest))
            request.get_method = lambda : 'HEAD'
            headResponse = urllib2.urlopen(request)
            assertEquals(int(headResponse.info().getheader('Content-Length')), len(fullResponse))
            assertEquals(headResponse.info().getheader('Content-Type'), contentType)

    def testRangeRequestSanity(self):
        for curRequest, contentType in VOD_REQUESTS:
            url = self.getUrl(curRequest)
            response = urllib2.urlopen(url)
            assertEquals(response.info().getheader('Content-Type'), contentType)
            fullResponse = response.read()
            for i in xrange(10):
                startOffset = random.randint(0, len(fullResponse) - 1)
                endOffset = random.randint(startOffset, len(fullResponse) - 1)
                if not ENCRYPTED_PREFIX in url:         # the url must not change when enryption is enabled, since the key will change
                    url = self.getUrl(curRequest)
                request = urllib2.Request(url, headers={'Range': 'bytes=%s-%s' % (startOffset, endOffset)})
                response = urllib2.urlopen(request)
                assertEquals(response.info().getheader('Content-Type'), contentType)
                assert(response.read() == fullResponse[startOffset:(endOffset + 1)])

    def testEncryptionSanity(self):
        url = self.getUrl(HLS_PLAYLIST_FILE)
        response = urllib2.urlopen(url).read()
        keyUri = None
        for curLine in response.split('\n'):
            if curLine.startswith('#EXT-X-KEY'):
                keyUri = curLine.split('"')[1]
        if keyUri == None:
            return
        baseUrl = url[:url.rfind('/')] + '/'        # cannot use getUrl here since it will create a new URL with a new key
        aesKey = urllib2.urlopen(baseUrl + keyUri).read()

        encryptedSegment = urllib2.urlopen(baseUrl + HLS_SEGMENT_FILE).read()
        cipher = AES.new(aesKey, AES.MODE_CBC, '\0' * 12 + struct.pack('>L', 1))
        decryptedSegment = cipher.decrypt(encryptedSegment)
        padLength = ord(decryptedSegment[-1])
        decryptedSegment = decryptedSegment[:-padLength]

        clearSegment = urllib2.urlopen(self.getUrl(HLS_SEGMENT_FILE).replace(ENCRYPTED_PREFIX, '')).read()

        assert(clearSegment == decryptedSegment)
        

    # bad requests    
    def testPostRequest(self):
        assertRequestFails(self.getUrl('/seg-1-a1-v1.ts'), 405, postData='abcd')
        self.logTracker.assertContains('unsupported method')

    def testSegmentIdTooBig(self):
        assertRequestFails(self.getUrl('/seg-3600-a1-v1.ts'), 404)
        self.logTracker.assertContains('requested segment index 3599 exceeds the segment count')

    def testNonExistingTracks(self):
        assertRequestFails(self.getUrl('/seg-1-a10-v10.ts'), 400)
        self.logTracker.assertContains('no matching streams were found')

    def testClipToLargerThanClipFrom(self):
        assertRequestFails(self.getUrl('/clipFrom/10000/clipTo/1000/index.m3u8'), 400)
        self.logTracker.assertContains('clip from 10000 is larger than clip to 1000')

    def testBadSegmentIndex(self):
        assertRequestFails(self.getUrl('/seg-abc-a1-v1.ts'), 400)
        self.logTracker.assertContains('failed to parse the segment index')
    
    def testBadStreamIndex(self):
        assertRequestFails(self.getUrl('/seg-1-aabc-v1.ts'), 400)
        self.logTracker.assertContains('failed to parse a stream index')
    
    def testBadStreamType(self):
        assertRequestFails(self.getUrl('/seg-1-a1-x1.ts'), 400)
        self.logTracker.assertContains('failed to parse stream type')

    def testUnrecognizedTSRequest(self):
        assertRequestFails(self.getUrl('/bla-1-a1-v1.ts'), 400)
        self.logTracker.assertContains('unidentified ts request')

    def testUnrecognizedTSRequest(self):
        assertRequestFails(self.getUrl('/bla.m3u8'), 400)
        self.logTracker.assertContains('unidentified m3u8 request')

    # XXXXXXXXXX move out of local - remote/mapped only
    def testBadClipTo(self):        # the error should be ignored
        testBody = urllib2.urlopen(self.getUrl('/clipTo/abcd' + HLS_PLAYLIST_FILE)).read()
        refBody = urllib2.urlopen(self.getUrl(HLS_PLAYLIST_FILE)).read()
        assert(testBody == refBody)

    def testBadClipFrom(self):        # the error should be ignored
        testBody = urllib2.urlopen(self.getUrl('/clipFrom/abcd' + HLS_PLAYLIST_FILE)).read()
        refBody = urllib2.urlopen(self.getUrl(HLS_PLAYLIST_FILE)).read()
        assert(testBody == refBody)

class UpstreamTestSuite(TestSuite):
    def __init__(self, baseUrl, uri, serverPort, urlFile = ''):
        super(UpstreamTestSuite, self).__init__()
        self.baseUrl = baseUrl
        self.uri = uri
        self.urlFile = urlFile
        self.serverPort = serverPort
    
    def testServerNotListening(self):
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 502)
        self.logTracker.assertContains('connect() failed (111: Connection refused)')
    
    def testServerNotAccepting(self):
        s = createTcpServer(self.serverPort)
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 504)        
        self.logTracker.assertContains('upstream timed out')

    def testServerNotResponding(self):
        TcpServer(self.serverPort, lambda s: socketSendAndWait(s, 'HTTP/1.1 200 OK', 10))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 504)
        self.logTracker.assertContains('upstream timed out')
    
    def testBadStatusLine(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'BAD STATUS LINE\r\n'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 500)
        self.logTracker.assertContains('failed to parse status line')

    def testBadContentLength(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nContent-Length: abcd\r\n'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 502)
        self.logTracker.assertContains('failed to parse content length')

    def testInvalidHeader(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nBad\rHeader\r\n'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 502)
        self.logTracker.assertContains('failed to parse header line')

    def testSocketShutdownWhileReadingHeaders(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nContent'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 502)
        self.logTracker.assertContains('upstream prematurely closed connection')

class MemoryUpstreamTestSuite(UpstreamTestSuite):
    def __init__(self, baseUrl, uri, serverPort, urlFile, upstreamHandler):
        super(MemoryUpstreamTestSuite, self).__init__(baseUrl, uri, serverPort, urlFile)
        self.upstreamHandler = upstreamHandler

    def testContentLengthTooBig(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nContent-Length: 999999999\r\n'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 502)
        self.logTracker.assertContains('content length 999999999 exceeds the limit')

    def testNoContentLength(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\n\r\n'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 502)
        self.logTracker.assertContains('got no content-length header')

    def testSocketShutdownWhileReadingBody(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 502)
        self.logTracker.assertContains('upstream connection was closed with 7 bytes left to read')

    def testErrorHttpStatus(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, getHttpResponse('x', '400 Bad Request')))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 502)
        self.logTracker.assertContains('upstream returned a bad status 400')

    def testHeadersForwardedToUpstream(self):
        TcpServer(API_SERVER_PORT, lambda s: socketExpectHttpHeaderAndHandle(s, 'mukka: ukk', self.upstreamHandler))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.uri, HLS_PLAYLIST_FILE), headers={'mukka':'ukk'})
        response = urllib2.urlopen(request)
        validatePlaylistM3U8(response.read())

    def testUpstreamHostHeader(self):
        TcpServer(API_SERVER_PORT, lambda s: socketExpectHttpHeaderAndHandle(s, 'Host: blabla.com', self.upstreamHandler))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.uri, HLS_PLAYLIST_FILE), headers={'Host': 'blabla.com'})
        response = urllib2.urlopen(request)
        validatePlaylistM3U8(response.read())

    def testUpstreamHostHeaderHttp10(self):
        TcpServer(API_SERVER_PORT, lambda s: socketExpectHttpHeaderAndHandle(s, 'Host: %s' % SERVER_NAME, self.upstreamHandler))
        body = sendHttp10Request(getUniqueUrl(self.baseUrl, self.uri, HLS_PLAYLIST_FILE))
        validatePlaylistM3U8(body)

class DumpUpstreamTestSuite(UpstreamTestSuite):
    def testRangeForwarded(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndSend(s, 'Range: bytes=5-10', getHttpResponse('abcde', '206 Partial Content')))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), headers={'Range': 'bytes=5-10'})
        assertEquals(urllib2.urlopen(request).read(), 'abcde')

    def testHeadForwarded(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpVerbAndSend(s, 'HEAD', getHttpResponse(length=123)))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.uri, self.urlFile))
        request.get_method = lambda : 'HEAD'
        headResponse = urllib2.urlopen(request)
        assertEquals(int(headResponse.info().getheader('Content-Length')), 123)

    def testHeadersForwarded(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndSend(s, 'Shuki: tuki', getHttpResponse('abcde')))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), headers={'Shuki': 'tuki'})
        assertEquals(urllib2.urlopen(request).read(), 'abcde')

    def testHeadersReturned(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, getHttpResponse('abcde', headers={'Kuki': 'Ukki'})))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.uri, self.urlFile))
        response = urllib2.urlopen(request)
        assertEquals(response.info().getheader('Kuki'), 'Ukki')

    def testErrorHttpStatus(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, getHttpResponse('blabla', status='402 Payment Required')))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 402, 'blabla')

class FallbackUpstreamTestSuite(DumpUpstreamTestSuite):
    def testLoopPreventionHeaderSent(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndSend(s, 'X-Kaltura-Proxy: dumpApiRequest', getHttpResponse('abcde')))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.uri, self.urlFile))
        assertEquals(urllib2.urlopen(request).read(), 'abcde')

    def testLoopPreventionHeaderReceived(self):
        assertRequestFails(getUniqueUrl(self.baseUrl, self.uri, self.urlFile), 404, headers={'X-Kaltura-Proxy': 'dumpApiRequest'})
        self.logTracker.assertContains('proxy header exists')

class FileServeTestSuite(TestSuite):
    def __init__(self, getServeUrl):
        super(FileServeTestSuite, self).__init__()
        self.getServeUrl = getServeUrl

    def testNoReadAccessFile(self):
        assertRequestFails(self.getServeUrl(TEST_NO_READ_ACCESS_FILE), 403)
        self.logTracker.assertContains('open() "%s" failed' % (TEST_FILES_ROOT + TEST_NO_READ_ACCESS_FILE))

    def testOpenFolder(self):
        assertRequestFails(self.getServeUrl(TEST_FOLDER), 403)
        self.logTracker.assertContains('"%s" is not a file' % (TEST_FILES_ROOT + TEST_FOLDER))

    def testMoovAtomCache(self):
        for curRequest, _ in HLS_REQUESTS:
            linkPath = createRandomSymLink(TEST_FILES_ROOT + TEST_FLAVOR_FILE)
            url = self.getServeUrl(linkPath) + curRequest

            logTracker = LogTracker()
            uncachedResponse = urllib2.urlopen(url).read()
            logTracker.assertContains('moov atom cache miss')
            
            logTracker = LogTracker()
            cachedResponse = urllib2.urlopen(url).read()
            logTracker.assertContains('moov atom cache hit')

            assert(cachedResponse == uncachedResponse)

            cleanupStack.resetAndDestroy()

class LocalTestSuite(TestSuite):
    def __init__(self, baseUrl):
        super(LocalTestSuite, self).__init__()
        self.baseUrl = baseUrl
            
    def runChildSuites(self):
        BasicTestSuite(
            lambda filePath: self.baseUrl + TEST_FLAVOR_FILE + filePath,
            lambda: None).run()
        FallbackUpstreamTestSuite(self.baseUrl, TEST_NONEXISTING_FILE, FALLBACK_PORT).run()
        FileServeTestSuite(lambda uri: self.baseUrl + uri).run()

    def testFileNotFound(self):
        assertRequestFails(self.baseUrl + TEST_NONEXISTING_FILE, 502)    # 502 is due to failing to connect to fallback
        self.logTracker.assertContains('open() "%s" failed' % (TEST_FILES_ROOT + TEST_NONEXISTING_FILE))

class MappedTestSuite(TestSuite):
    def __init__(self, baseUrl):
        super(MappedTestSuite, self).__init__()
        self.baseUrl = baseUrl
            
    def runChildSuites(self):
        BasicTestSuite(
            lambda filePath: getUniqueUrl(self.baseUrl, TEST_FLAVOR_URI, filePath),
            lambda: TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + TEST_FLAVOR_FILE)))).run()
        requestHandler = lambda s,h: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + TEST_FLAVOR_FILE))
        MemoryUpstreamTestSuite(self.baseUrl, TEST_FLAVOR_URI, API_SERVER_PORT, '', requestHandler).run()
        suite = FallbackUpstreamTestSuite(self.baseUrl, TEST_FLAVOR_URI, FALLBACK_PORT)
        suite.prepareTest = lambda: TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse('')))
        suite.run()
        FileServeTestSuite(self.getServeUrl).run()

    def getServeUrl(self, uri):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + uri)))
        return getUniqueUrl(self.baseUrl, TEST_FLAVOR_URI)

    def testEmptyPathMappingResponse(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getHttpResponse('')))
        assertRequestFails(getUniqueUrl(self.baseUrl, TEST_FLAVOR_URI), 404)
        self.logTracker.assertContains('empty path mapping response')

    def testUnexpectedPathResponse(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getHttpResponse('abcde')))
        assertRequestFails(getUniqueUrl(self.baseUrl, TEST_FLAVOR_URI), 503)
        self.logTracker.assertContains('unexpected path mapping response abcde')

    def testEmptyPathResponse(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse('')))
        assertRequestFails(getUniqueUrl(self.baseUrl, TEST_FLAVOR_URI), 502)     # 502 is due to failing to connect to fallback
        self.logTracker.assertContains('empty path returned from upstream')

    def testFileNotFound(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_NONEXISTING_FILE)))
        assertRequestFails(getUniqueUrl(self.baseUrl, TEST_FLAVOR_URI), 404)
        self.logTracker.assertContains('open() "%s" failed' % TEST_NONEXISTING_FILE)

    def testPathMappingCache(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + TEST_FLAVOR_FILE)))
        for curRequest, contentType in VOD_REQUESTS:
            url = getUniqueUrl(self.baseUrl, TEST_FLAVOR_URI, curRequest)
            
            # uncached
            logTracker = LogTracker()
            response = urllib2.urlopen(url)            
            assertEquals(response.info().getheader('Content-Type'), contentType)            
            uncachedResponse = response.read()
            logTracker.assertContains('path mapping cache miss')

            #cached
            logTracker = LogTracker()
            response = urllib2.urlopen(url)
            assertEquals(response.info().getheader('Content-Type'), contentType)            
            cachedResponse = response.read()
            logTracker.assertContains('path mapping cache hit')

            assert(cachedResponse == uncachedResponse)
                   
class RemoteTestSuite(TestSuite):
    def __init__(self, baseUrl):
        super(RemoteTestSuite, self).__init__()
        self.baseUrl = baseUrl
            
    def runChildSuites(self):
        BasicTestSuite(
            lambda filePath: getUniqueUrl(self.baseUrl, TEST_FLAVOR_URI, filePath),
            lambda: TcpServer(API_SERVER_PORT, lambda s: serveFile(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE))).run()
        requestHandler = lambda s,h: serveFileHandler(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE, h)
        MemoryUpstreamTestSuite(self.baseUrl, TEST_FLAVOR_URI, API_SERVER_PORT, HLS_PLAYLIST_FILE, requestHandler).run()
        DumpUpstreamTestSuite(self.baseUrl, TEST_FLAVOR_URI, API_SERVER_PORT).run()      # non HLS URL will just dump to upstream

    def testMoovAtomCache(self):
        TcpServer(API_SERVER_PORT, lambda s: serveFile(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE))
        for curRequest, _ in HLS_REQUESTS:
            url = getUniqueUrl(self.baseUrl, TEST_FLAVOR_URI, curRequest)

            logTracker = LogTracker()
            uncachedResponse = urllib2.urlopen(url).read()
            logTracker.assertContains('moov atom cache miss')
            
            logTracker = LogTracker()
            cachedResponse = urllib2.urlopen(url).read()
            logTracker.assertContains('moov atom cache hit')

            assert(cachedResponse == uncachedResponse)

class MainTestSuite(TestSuite):
    def runChildSuites(self):
        # non encrypted
        #LocalTestSuite(NGINX_LOCAL).run()
        #MappedTestSuite(NGINX_MAPPED).run()
        #RemoteTestSuite(NGINX_REMOTE).run()

        # encrypted
        LocalTestSuite(NGINX_LOCAL + ENCRYPTED_PREFIX).run()
        MappedTestSuite(NGINX_MAPPED + ENCRYPTED_PREFIX).run()
        RemoteTestSuite(NGINX_REMOTE + ENCRYPTED_PREFIX).run()

socketSend = socketSendRegular
MainTestSuite().run()
socketSend = socketSendByteByByte
MainTestSuite().run()
