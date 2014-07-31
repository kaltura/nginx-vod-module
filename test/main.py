from threading import Thread
import urlparse
import urllib2
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
NGINX_LOCAL = NGINX_HOST + '/local'
NGINX_MAPPED = NGINX_HOST + '/mapped'
NGINX_REMOTE = NGINX_HOST + '/remote'
HLS_PLAYLIST_FILE = '/index.m3u8'
HLS_IFRAMES_FILE = '/iframes.m3u8'
HLS_SEGMENT_FILE = '/seg-1-a1-v1.ts'
M3U8_PREFIX = '''#EXTM3U
#EXT-X-TARGETDURATION:10
#EXT-X-ALLOW-CACHE:YES
#EXT-X-VERSION:2
#EXT-X-MEDIA-SEQUENCE:1
'''
M3U8_POSTFIX = '#EXT-X-ENDLIST\n'
M3U8_EXTINF = '#EXTINF:'
M3U8_SEGMENT_PREFIX = 'seg-'
M3U8_SEGMENT_POSTFIX = '.ts'
SERVER_NAME = 'localhost'

HLS_REQUESTS = [HLS_PLAYLIST_FILE, HLS_IFRAMES_FILE, HLS_SEGMENT_FILE, '']      # '' returns the full file

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

def assertRequestFails(url, statusCode, expectedBody = None, headers = {}):
    request = urllib2.Request(url, headers=headers)
    try:
        response = urllib2.urlopen(request)
        assert(False)
    except urllib2.HTTPError, e:
        assertEquals(e.getcode(), statusCode)
        if expectedBody != None:
            assertEquals(expectedBody, e.read())

def validatePlaylistM3U8(buffer):
    assertStartsWith(buffer, M3U8_PREFIX)
    assertEndsWith(buffer, M3U8_POSTFIX)
    expectExtInf = True
    for curLine in buffer[len(M3U8_PREFIX):-len(M3U8_POSTFIX)].split('\n'):
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

def serveFileHandler(s, path, headers):
    # handle head
    if headers.startswith('HEAD'):
        socketSendAndShutdown(s, getHttpResponse(length=os.path.getsize(path)))
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
    socketSendAndShutdown(s, getHttpResponse(body, status))

def serveFile(s, path):
    headers = socketReadHttpHeaders(s)
    serveFileHandler(s, path, headers)

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
    def __init__(self, getUrl, uri, setupServer):
        super(BasicTestSuite, self).__init__()
        self.getUrl = getUrl
        self.uri = uri
        self.setupServer = setupServer
    
    def testHeadRequestSanity(self):
        self.setupServer()
        for curRequest in HLS_REQUESTS:
            fullResponse = urllib2.urlopen(self.getUrl(self.uri, curRequest)).read()
            request = urllib2.Request(self.getUrl(self.uri, curRequest))
            request.get_method = lambda : 'HEAD'
            headResponse = urllib2.urlopen(request)
            assertEquals(int(headResponse.info().getheader('Content-Length')), len(fullResponse))

    def testRangeRequestSanity(self):
        self.setupServer()
        for curRequest in HLS_REQUESTS:
            fullResponse = urllib2.urlopen(self.getUrl(self.uri, curRequest)).read()
            for i in xrange(10):
                startOffset = random.randint(0, len(fullResponse) - 1)
                endOffset = random.randint(startOffset, len(fullResponse) - 1)
                request = urllib2.Request(self.getUrl(self.uri, curRequest), headers={'Range': 'bytes=%s-%s' % (startOffset, endOffset)})
                response = urllib2.urlopen(request).read()
                assertEquals(response, fullResponse[startOffset:(endOffset + 1)])

    def testSegmentIdTooBig(self):
        self.setupServer()
        assertRequestFails(self.getUrl(self.uri, '/seg-3600-a1-v1.ts'), 400)
        self.logTracker.assertContains('no frames were found')

    def testNonExistingTracks(self):
        self.setupServer()
        assertRequestFails(self.getUrl(self.uri, '/seg-1-a10-v10.ts'), 400)
        self.logTracker.assertContains('no matching streams were found')

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

class LocalTestSuite(TestSuite):
    def runChildSuites(self):
        BasicTestSuite(
            lambda uri, filePath: NGINX_LOCAL + uri + filePath,
            TEST_FLAVOR_FILE,
            lambda: None).run()
        FallbackUpstreamTestSuite(NGINX_LOCAL, TEST_NONEXISTING_FILE, FALLBACK_PORT).run()
        FileServeTestSuite(lambda uri: NGINX_LOCAL + uri).run()

    def testFileNotFound(self):
        assertRequestFails(NGINX_LOCAL + TEST_NONEXISTING_FILE, 502)    # 502 is due to failing to connect to fallback
        self.logTracker.assertContains('open() "%s" failed' % (TEST_FILES_ROOT + TEST_NONEXISTING_FILE))

class MappedTestSuite(TestSuite):
    def runChildSuites(self):
        BasicTestSuite(
            lambda uri, filePath: getUniqueUrl(NGINX_MAPPED, uri, filePath),
            TEST_FLAVOR_URI, 
            lambda: TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + TEST_FLAVOR_FILE)))).run()
        requestHandler = lambda s,h: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + TEST_FLAVOR_FILE))
        MemoryUpstreamTestSuite(NGINX_MAPPED, TEST_FLAVOR_URI, API_SERVER_PORT, '', requestHandler).run()
        suite = FallbackUpstreamTestSuite(NGINX_MAPPED, TEST_FLAVOR_URI, FALLBACK_PORT)
        suite.prepareTest = lambda: TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse('')))
        suite.run()
        FileServeTestSuite(MappedTestSuite.getServeUrl).run()

    @staticmethod
    def getServeUrl(uri):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + uri)))
        return getUniqueUrl(NGINX_MAPPED, TEST_FLAVOR_URI)

    def testEmptyPathMappingResponse(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getHttpResponse('')))
        assertRequestFails(getUniqueUrl(NGINX_MAPPED, TEST_FLAVOR_URI), 503)
        self.logTracker.assertContains('empty path mapping response')

    def testUnexpectedPathResponse(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getHttpResponse('abcde')))
        assertRequestFails(getUniqueUrl(NGINX_MAPPED, TEST_FLAVOR_URI), 503)
        self.logTracker.assertContains('unexpected path mapping response abcde')

    def testEmptyPathResponse(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse('')))
        assertRequestFails(getUniqueUrl(NGINX_MAPPED, TEST_FLAVOR_URI), 502)     # 502 is due to failing to connect to fallback
        self.logTracker.assertContains('empty path returned from upstream')

    def testFileNotFound(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_NONEXISTING_FILE)))
        assertRequestFails(getUniqueUrl(NGINX_MAPPED, TEST_FLAVOR_URI), 404)
        self.logTracker.assertContains('open() "%s" failed' % TEST_NONEXISTING_FILE)
    
class RemoteTestSuite(TestSuite):
    def runChildSuites(self):
        BasicTestSuite(
            lambda uri, filePath: getUniqueUrl(NGINX_REMOTE, uri, filePath),
            TEST_FLAVOR_URI, 
            lambda: TcpServer(API_SERVER_PORT, lambda s: serveFile(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE))).run()
        requestHandler = lambda s,h: serveFileHandler(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, h)
        MemoryUpstreamTestSuite(NGINX_REMOTE, TEST_FLAVOR_URI, API_SERVER_PORT, HLS_PLAYLIST_FILE, requestHandler).run()
        DumpUpstreamTestSuite(NGINX_REMOTE, TEST_FLAVOR_URI, API_SERVER_PORT).run()      # non HLS URL will just dump to upstream

class MainTestSuite(TestSuite):
    def runChildSuites(self):
        LocalTestSuite().run()
        MappedTestSuite().run()
        RemoteTestSuite().run()

socketSend = socketSendRegular
MainTestSuite().run()
socketSend = socketSendByteByByte
MainTestSuite().run()
