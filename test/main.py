from httplib import BadStatusLine
from threading import Thread
from Crypto.Cipher import AES
from test_base import *
import urlparse
import urllib2
import base64
import struct
import socket
import random
import time
import os

# note: debug logs must be enabled as well as metadata cache

# environment specific parameters
from main_params import *

MSS_BITRATE = 0x41000        # actual bitrate does not matter, only the file index & stream index (both 0)

# nginx config derivatives
NGINX_LOG_PATH = '/var/log/nginx/error.log'
NGINX_HOST = 'http://localhost:8001'
API_SERVER_PORT = 8002
FALLBACK_PORT = 8003
DRM_SERVER_PORT = 8004
ENCRYPTED_PREFIX = '/enc'
KEEPALIVE_PREFIX = '/self'
NGINX_LOCAL = NGINX_HOST + '/tlocal'
NGINX_MAPPED = NGINX_HOST + '/tmapped'
NGINX_REMOTE = NGINX_HOST + '/tremote'

DASH_PREFIX = '/dash'
DASH_MANIFEST_FILE = '/manifest.mpd'
DASH_INIT_FILE = '/init-v1.mp4'
DASH_FRAGMENT_FILE = '/fragment-1-v1.m4s'

HDS_PREFIX = '/hds'
HDS_MANIFEST_FILE = '/manifest.f4m'
HDS_FRAGMENT_FILE = '/frag-Seg1-Frag1'

HLS_PREFIX = '/hls'
HLS_MASTER_FILE = '/master.m3u8'
HLS_PLAYLIST_FILE = '/index.m3u8'
HLS_IFRAMES_FILE = '/iframes.m3u8'
HLS_SEGMENT_FILE = '/seg-1.ts'

MSS_PREFIX = '/mss'
MSS_MANIFEST_FILE = '/manifest'
MSS_FRAGMENT_FILE = '/QualityLevels(%s)/Fragments(video=0)' % MSS_BITRATE

EDASH_PREFIX = '/edash'
DRM_SERVICE_RESPONSE = '''[{
                "key_id": "%s",
                "pssh": [{
                                "data": "%s",
                                "uuid": "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed"
                }],
                "next_key_id": null,
                "key": "%s"
}]''' % (base64.b64encode('0' * 16), base64.b64encode('abcd'), base64.b64encode('1' * 16))

M3U8_PREFIX = '''#EXTM3U
#EXT-X-TARGETDURATION:10
#EXT-X-ALLOW-CACHE:YES
#EXT-X-PLAYLIST-TYPE:VOD
#EXT-X-VERSION:3
#EXT-X-MEDIA-SEQUENCE:1
'''

M3U8_PREFIX_ENCRYPTED_PART1 = '''#EXTM3U
#EXT-X-TARGETDURATION:10
#EXT-X-ALLOW-CACHE:YES
#EXT-X-PLAYLIST-TYPE:VOD
#EXT-X-KEY:METHOD=AES-128,URI="'''

M3U8_PREFIX_ENCRYPTED_PART2 = '''encryption.key"
#EXT-X-VERSION:3
#EXT-X-MEDIA-SEQUENCE:1
'''
M3U8_POSTFIX = '#EXT-X-ENDLIST\n'
M3U8_EXTINF = '#EXTINF:'
M3U8_SEGMENT_PREFIX = 'seg-'
M3U8_SEGMENT_POSTFIX = '.ts'
SERVER_NAME = 'localhost'

DASH_REQUESTS = [
    (DASH_PREFIX, DASH_MANIFEST_FILE, 'application/dash+xml'),
    (DASH_PREFIX, DASH_INIT_FILE, 'video/mp4'),
    (DASH_PREFIX, DASH_FRAGMENT_FILE, 'video/mp4')]

HDS_REQUESTS = [
    (HDS_PREFIX, HDS_MANIFEST_FILE, 'video/f4m'),
    (HDS_PREFIX, HDS_FRAGMENT_FILE, 'video/f4f')]

HLS_REQUESTS = [
    (HLS_PREFIX, HLS_MASTER_FILE, 'application/vnd.apple.mpegurl'),
    (HLS_PREFIX, HLS_PLAYLIST_FILE, 'application/vnd.apple.mpegurl'),
    (HLS_PREFIX, HLS_IFRAMES_FILE, 'application/vnd.apple.mpegurl'),
    (HLS_PREFIX, HLS_SEGMENT_FILE, 'video/MP2T')]

MSS_REQUESTS = [
    (MSS_PREFIX, MSS_MANIFEST_FILE, 'text/xml'),
    (MSS_PREFIX, MSS_FRAGMENT_FILE, 'video/mp4')]

VOD_REQUESTS = DASH_REQUESTS + HDS_REQUESTS + HLS_REQUESTS + MSS_REQUESTS

PD_REQUESTS = [
    ('', '', 'video/mp4'),      # '' returns the full file
    ('', '/clipTo/10000', 'video/mp4'),
    ('', '/clipFrom/10000', 'video/mp4'),
]

ALL_REQUESTS = VOD_REQUESTS + PD_REQUESTS

### Assertions

def validatePlaylistM3U8(buffer, expectedBaseUrl):
    expectedBaseUrl = expectedBaseUrl.rsplit('/', 1)[0]
    expectedBaseUrl = expectedBaseUrl.replace(KEEPALIVE_PREFIX, '')
    encryptedHeader = M3U8_PREFIX_ENCRYPTED_PART1 + expectedBaseUrl + '/' + M3U8_PREFIX_ENCRYPTED_PART2
    if buffer.startswith(encryptedHeader):
        buffer = buffer[len(encryptedHeader):]
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
            splittedLine = curLine.rsplit('/', 1)
            if len(splittedLine) > 1:
                baseUrl, fileName = splittedLine
            else:
                fileName = splittedLine[0]
                baseUrl = ''
            assertEquals(expectedBaseUrl, baseUrl)
            assertStartsWith(fileName, M3U8_SEGMENT_PREFIX)
            assertEndsWith(fileName, M3U8_SEGMENT_POSTFIX)
        expectExtInf = not expectExtInf

### Misc utility functions
def getHttpResponseRegular(body = '', status = '200 OK', length = None, headers = {}):
    if length == None:
        length = len(body)
    headersStr = ''
    if len(headers) > 0:
        for curHeader in headers.items():
            headersStr +='%s: %s\r\n' % curHeader
    return 'HTTP/1.1 %s\r\nContent-Length: %s\r\n%s\r\n%s' % (status, length, headersStr, body)

def getHttpResponseChunked(body = '', status = '200 OK', length = None, headers = {}):
    if length == None:
        length = len(body)
    headersStr = ''
    if len(headers) > 0:
        for curHeader in headers.items():
            headersStr +='%s: %s\r\n' % curHeader
    return 'HTTP/1.1 %s\r\nTransfer-Encoding: Chunked\r\n%s\r\n%x\r\n%s\r\n0\r\n' % (status, headersStr, length, body)

def getPathMappingResponse(path):
    return getHttpResponse('{"sequences":[{"clips":[{"type":"source","path":"%s"}]}]}' % path)

def getUniqueUrl(baseUrl, filePath = ''):
    return '%s/rand/%s%s' % (baseUrl, random.randint(0, 0x100000), filePath)

def createRandomSymLink(sourcePath):
    linkPath = TEST_LINK_BASE_PATH + '/testlink_%s.mp4' % random.randint(0, 0x100000)
    linkFullPath = TEST_FILES_ROOT + linkPath
    os.symlink(sourcePath, linkFullPath)
    cleanupStack.push(lambda: os.unlink(linkFullPath))
    return linkPath

### Socket functions
class SocketException(Exception):
    pass

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
            raise SocketException("socket connection broken")
        totalSent += sent

def socketSendByteByByte(s, msg):
    if len(msg) > 16 * 1024:     # we may send big buffers when we serve files, disable it there since it takes forever
        socketSendRegular(s, msg)
        return
    for curByte in msg:
        try:
            if s.send(curByte) == 0:
                raise SocketException("socket connection broken")
        except socket.error:        # the server may terminate the connection due to bad data in some tests
            break

def socketSendAndShutdown(s, msg):
    try:
        socketSend(s, msg)
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
            raise SocketException("socket connection broken")
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

def socketExpectHttpHeaderAndHandle(s, posHeader, negHeader, handler):
    headers = socketReadHttpHeaders(s)
    if posHeader != None:
        assertInIgnoreCase(posHeader, headers)
    if negHeader != None:
        assertNotInIgnoreCase(negHeader, headers)
    handler(s, headers)

def socketExpectHttpHeaderAndSend(s, header, msg):
    socketExpectHttpHeaderAndHandle(s, header, None, lambda s, h: socketSendAndShutdown(s, msg))

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
            raise SocketException("socket connection broken")
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

def serveFileHandler(s, path, mimeType, headers, maxSize = -1):
    if maxSize < 0:
        maxSize = os.path.getsize(path)

    # handle head
    if headers.startswith('HEAD'):
        socketSendAndShutdown(s, getHttpResponse(length=maxSize, headers={'Content-Type':mimeType}))
        return

    # get the request body
    f = file(path, 'rb')
    range = getHttpHeader(headers, 'range')
    if range != None:
        assertStartsWith(range, 'bytes=')
        range = range.split('=', 1)[1].strip()
        if range.endswith('-'):
            startOffset = int(range[:-1].strip())
            endOffset = maxSize
        elif range.startswith('-'):
            startOffset = maxSize - int(range[1:].strip())
            endOffset = maxSize
        else:
            startOffset, endOffset = map(lambda x: int(x.strip()), range.split('-', 1))
            endOffset += 1
        startOffset = min(startOffset, maxSize)
        endOffset = min(endOffset, maxSize)
        f.seek(startOffset, os.SEEK_SET)
        body = f.read(endOffset - startOffset)
        status = '206 Partial Content'
    else:
        body = f.read(maxSize)
        status = '200 OK'
    f.close()

    # build the response
    socketSendAndShutdown(s, getHttpResponse(body, status, headers={'Content-Type':mimeType}))

def serveFile(s, path, mimeType, maxSize = -1):
    try:
        headers = socketReadHttpHeaders(s)
        serveFileHandler(s, path, mimeType, headers, maxSize)
    except SocketException:
        pass
    except IOError:
        pass

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

### Test suites

class ProtocolTestSuite(TestSuite):
    def __init__(self, getUrl, protocolPrefix, setupServer):
        super(ProtocolTestSuite, self).__init__()
        self.getUrl = lambda filePath: getUrl(protocolPrefix, filePath)
        self.prepareTest = setupServer


class DashTestSuite(ProtocolTestSuite):
    def __init__(self, getUrl, setupServer):
        super(DashTestSuite, self).__init__(getUrl, DASH_PREFIX, setupServer)

    # bad requests
    def testUnrecognizedRequest(self):
        assertRequestFails(self.getUrl('/bla'), 400)
        self.logTracker.assertContains('unidentified request')

    def testBadStreamTypeManifest(self):
        assertRequestFails(self.getUrl('/manifest-a1-z1.mpd'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadStreamIndexManifest(self):
        assertRequestFails(self.getUrl('/manifest-aabc.mpd'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadStreamTypeInit(self):
        assertRequestFails(self.getUrl('/init-a1-z1.mp4'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadStreamIndexInit(self):
        assertRequestFails(self.getUrl('/init-aabc.mp4'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadFragmentIndex(self):
        assertRequestFails(self.getUrl('/fragment-x-a1-z1.m4s'), 400)
        self.logTracker.assertContains('failed to parse segment index')

    def testBadStreamTypeFragment(self):
        assertRequestFails(self.getUrl('/fragment-1-a1-z1.m4s'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadStreamIndexFragment(self):
        assertRequestFails(self.getUrl('/fragment-1-aabc.m4s'), 400)
        self.logTracker.assertContains('did not consume the whole name')

class HdsTestSuite(ProtocolTestSuite):
    def __init__(self, getUrl, setupServer):
        super(HdsTestSuite, self).__init__(getUrl, HDS_PREFIX, setupServer)

    # bad requests
    def testUnrecognizedRequest(self):
        assertRequestFails(self.getUrl('/bla'), 400)
        self.logTracker.assertContains('unidentified request')

    def testBadStreamTypeManifest(self):
        assertRequestFails(self.getUrl('/manifest-a1-z1.f4m'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadStreamIndexManifest(self):
        assertRequestFails(self.getUrl('/manifest-aabc.f4m'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testNoFragmentIndex(self):
        assertRequestFails(self.getUrl('/frag-'), 400)
        self.logTracker.assertContains('failed to parse fragment index')

    def testZeroFragmentIndex(self):
        assertRequestFails(self.getUrl('/frag-0'), 400)
        self.logTracker.assertContains('failed to parse fragment index')

    def testNoSegmentFragment(self):
        assertRequestFails(self.getUrl('/frag-1'), 400)
        self.logTracker.assertContains('invalid segment / fragment requested')

    def testBadStreamTypeFragment(self):
        assertRequestFails(self.getUrl('/frag-a1-z1-Seg1-Frag1'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadStreamIndexFragment(self):
        assertRequestFails(self.getUrl('/frag-aabc-Seg1-Frag1'), 400)
        self.logTracker.assertContains('did not consume the whole name')

class HlsTestSuite(ProtocolTestSuite):
    def __init__(self, getUrl, setupServer):
        super(HlsTestSuite, self).__init__(getUrl, HLS_PREFIX, setupServer)

    # bad requests
    def testBadSegmentIndex(self):
        assertRequestFails(self.getUrl('/seg-abc-a1-v1.ts'), 400)
        self.logTracker.assertContains('failed to parse segment index')

    def testBadStreamIndex(self):
        assertRequestFails(self.getUrl('/seg-1-aabc-v1.ts'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadStreamType(self):
        assertRequestFails(self.getUrl('/seg-1-a1-z1.ts'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testUnrecognizedTSRequest(self):
        assertRequestFails(self.getUrl('/bla-1-a1-v1.ts'), 400)
        self.logTracker.assertContains('unidentified request')

    def testUnrecognizedM3U8Request(self):
        assertRequestFails(self.getUrl('/bla.m3u8'), 400)
        self.logTracker.assertContains('unidentified m3u8 request')

class MssTestSuite(ProtocolTestSuite):
    def __init__(self, getUrl, setupServer):
        super(MssTestSuite, self).__init__(getUrl, MSS_PREFIX, setupServer)

    # bad requests
    def testUnrecognizedRequest(self):
        assertRequestFails(self.getUrl('/bla'), 400)
        self.logTracker.assertContains('unidentified request')

    def testBadStreamType(self):
        assertRequestFails(self.getUrl('/manifest-a1-z1'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadStreamIndex(self):
        assertRequestFails(self.getUrl('/manifest-aabc'), 400)
        self.logTracker.assertContains('did not consume the whole name')

    def testBadFragmentRequestQualityLevel(self):
        assertRequestFails(self.getUrl('/Quality-Levels(2364883)/Fragments(video=0)'), 400)
        self.logTracker.assertContains('ngx_http_vod_parse_string failed')

    def testBadFragmentRequestBitrate(self):
        assertRequestFails(self.getUrl('/QualityLevels(a)/Fragments(video=0)'), 400)
        self.logTracker.assertContains('ngx_http_vod_parse_string failed')

    def testBadFragmentRequestFragments(self):
        assertRequestFails(self.getUrl('/QualityLevels(2364883)/Frag-ments(video=0)'), 400)
        self.logTracker.assertContains('ngx_http_vod_parse_string failed')

    def testBadFragmentRequestNoDelim(self):
        assertRequestFails(self.getUrl('/QualityLevels(2364883)/Fragments(video)'), 400)
        self.logTracker.assertContains('ngx_http_vod_parse_string failed')

    def testBadFragmentRequestTimestamp(self):
        assertRequestFails(self.getUrl('/QualityLevels(2364883)/Fragments(video=a)'), 400)
        self.logTracker.assertContains('ngx_http_vod_parse_string failed')

    def testBadFragmentRequestMediaTypeLength(self):
        assertRequestFails(self.getUrl('/QualityLevels(2364883)/Fragments(videox=0)'), 400)
        self.logTracker.assertContains('invalid media type videox')

    def testBadFragmentRequestMediaType(self):
        assertRequestFails(self.getUrl('/QualityLevels(2364883)/Fragments(xideo=0)'), 400)
        self.logTracker.assertContains('invalid media type xideo')

class BasicTestSuite(TestSuite):
    def __init__(self, getUrl, setupServer):
        super(BasicTestSuite, self).__init__()
        self.getUrl = getUrl
        self.prepareTest = setupServer

    def runChildSuites(self):
        DashTestSuite(self.getUrl, self.prepareTest).run()
        HdsTestSuite(self.getUrl, self.prepareTest).run()
        HlsTestSuite(self.getUrl, self.prepareTest).run()
        MssTestSuite(self.getUrl, self.prepareTest).run()

    # sanity
    def testHeadRequestSanity(self):
        for curPrefix, curRequest, contentType in ALL_REQUESTS:
            url = self.getUrl(curPrefix, curRequest)
            if KEEPALIVE_PREFIX in url:
                continue
            fullResponse = urllib2.urlopen(url).read()
            request = urllib2.Request(url)
            request.get_method = lambda : 'HEAD'
            headResponse = urllib2.urlopen(request)
            contentLength = headResponse.info().getheader('Content-Length')
            assertEquals(headResponse.info().getheader('Content-Type'), contentType)
            if getHttpResponse == getHttpResponseChunked and contentLength == None:
                return
            assertEquals(int(contentLength), len(fullResponse))

    def testRangeRequestSanity(self):
        for curPrefix, curRequest, contentType in ALL_REQUESTS:
            # recreate the server since it may disconnect after many requests
            cleanupStack.resetAndDestroy()
            if self.prepareTest != None:
                self.prepareTest()

            url = self.getUrl(curPrefix, curRequest)
            if KEEPALIVE_PREFIX in url:
                continue
            response = urllib2.urlopen(url)
            assertEquals(response.info().getheader('Content-Type'), contentType)
            fullResponse = response.read()
            for i in xrange(30):
                rangeType = random.randint(0, 3)
                if rangeType == 0:      # prefix (132-)
                    startOffset = random.randint(0, len(fullResponse) - 1)
                    expectedResponse = fullResponse[startOffset:]
                    rangeHeader = '%s-' % startOffset
                elif rangeType == 1:    # suffix (-123)
                    startOffset = random.randint(0, len(fullResponse) - 1) + 1
                    expectedResponse = fullResponse[-startOffset:]
                    rangeHeader = '-%s' % startOffset
                else:                   # regular range (100-200)
                    startOffset = random.randint(0, len(fullResponse) - 1)
                    endOffset = random.randint(startOffset, len(fullResponse) - 1)
                    expectedResponse = fullResponse[startOffset:(endOffset + 1)]
                    rangeHeader = '%s-%s' % (startOffset, endOffset)
                request = urllib2.Request(url, headers={'Range': 'bytes=%s' % rangeHeader})
                response = urllib2.urlopen(request)
                assertEquals(response.info().getheader('Content-Type'), contentType)
                assert(response.read() == expectedResponse)

    def testEncryptionSanity(self):
        url = self.getUrl(HLS_PREFIX, HLS_PLAYLIST_FILE)
        response = urllib2.urlopen(url).read()
        keyUri = None
        for curLine in response.split('\n'):
            if curLine.startswith('#EXT-X-KEY'):
                keyUri = curLine.split('"')[1]
        if keyUri == None:
            return
        baseUrl = url[:url.rfind('/')] + '/'        # cannot use getUrl here since it will create a new URL with a new key
        aesKey = urllib2.urlopen(keyUri).read()

        encryptedSegment = urllib2.urlopen(baseUrl + HLS_SEGMENT_FILE).read()
        cipher = AES.new(aesKey, AES.MODE_CBC, '\0' * 12 + struct.pack('>L', 1))
        decryptedSegment = cipher.decrypt(encryptedSegment)
        padLength = ord(decryptedSegment[-1])
        decryptedSegment = decryptedSegment[:-padLength]

        clearSegment = urllib2.urlopen(self.getUrl(HLS_PREFIX, HLS_SEGMENT_FILE).replace(ENCRYPTED_PREFIX, '')).read()

        assert(clearSegment == decryptedSegment)

    def testClipToSanity(self):
        # index
        url = self.getUrl(HLS_PREFIX, '/clipTo/10000' + HLS_PLAYLIST_FILE)
        clippedResponse = urllib2.urlopen(url).read()
        validatePlaylistM3U8(clippedResponse, url)
        assertEquals(clippedResponse.count(M3U8_EXTINF), 1)

        # segment
        assertRequestFails(self.getUrl(HLS_PREFIX, '/clipTo/10000/seg-2-a1-v1.ts'), 404)

        # iframes
        fullResponse = urllib2.urlopen(self.getUrl(HLS_PREFIX, HLS_IFRAMES_FILE)).read()
        clippedResponse = urllib2.urlopen(self.getUrl(HLS_PREFIX, '/clipTo/10000' + HLS_IFRAMES_FILE)).read()
        assert(len(clippedResponse) < len(fullResponse))

    def testClipFromSanity(self):
        # index
        fullResponse = urllib2.urlopen(self.getUrl(HLS_PREFIX, HLS_PLAYLIST_FILE)).read()
        url = self.getUrl(HLS_PREFIX, '/clipFrom/10000' + HLS_PLAYLIST_FILE)
        clippedResponse = urllib2.urlopen(url).read()
        validatePlaylistM3U8(clippedResponse, url)
        assertEquals(clippedResponse.count(M3U8_EXTINF) + 1, fullResponse.count(M3U8_EXTINF))

        # segment
        assertRequestFails(self.getUrl(HLS_PREFIX, '/clipFrom/10000/seg-%s-a1-v1.ts' % fullResponse.count(M3U8_EXTINF)), 404)

        # iframes
        fullResponse = urllib2.urlopen(self.getUrl(HLS_PREFIX, HLS_IFRAMES_FILE)).read()
        clippedResponse = urllib2.urlopen(self.getUrl(HLS_PREFIX, '/clipFrom/10000' + HLS_IFRAMES_FILE)).read()
        assert(len(clippedResponse) < len(fullResponse))

    def testRequiredTracksOptional(self):
        for curPrefix, curRequest, contentType in VOD_REQUESTS:
            # request must not have track specification
            if '-v1' in curRequest:
                continue

            # request must have extension
            if curRequest.rfind('.') < curRequest.rfind('/'):
                continue

            # no tracks specification
            url = self.getUrl(curPrefix, curRequest)
            noTracksResponse = urllib2.urlopen(url).read()

            # with tracks specification
            url = '-a1-v1.'.join(url.rsplit('.', 1))    # replace only the last dot
            withTracksResponse = urllib2.urlopen(url).read()
            withTracksResponse = withTracksResponse.replace('-a1-v1.f4m</id>', '.f4m</id>')        # align the f4m id tag

            assert(noTracksResponse == withTracksResponse)

    # bad requests
    def testPostRequest(self):
        assertRequestFails(self.getUrl(HLS_PREFIX, '/seg-1-a1-v1.ts'), 405, postData='abcd')
        self.logTracker.assertContains('unsupported method')

    def testSegmentIdTooBig(self):
        assertRequestFails(self.getUrl(HLS_PREFIX, '/seg-3600-a1-v1.ts'), 404)
        self.logTracker.assertContains('no matching streams were found, probably invalid segment index')

    def testNonExistingTracksM3U8(self):
        assertRequestFails(self.getUrl(HLS_PREFIX, '/index-a10-v10.m3u8'), 400)
        self.logTracker.assertContains('no matching streams were found')

    def testNonExistingTracksTS(self):
        assertRequestFails(self.getUrl(HLS_PREFIX,  '/seg-1-a10-v10.ts'), 404)
        self.logTracker.assertContains('no matching streams were found, probably invalid segment index')

    def testClipToLargerThanClipFrom(self):
        assertRequestFails(self.getUrl(HLS_PREFIX, '/clipFrom/10000/clipTo/1000' + HLS_PLAYLIST_FILE), 400)
        self.logTracker.assertContains('clip from 10000 is larger than clip to 1000')

    def testClipFromLargerThanVideoDurationTS(self):
        assertRequestFails(self.getUrl(HLS_PREFIX, '/clipFrom/9999999' + HLS_SEGMENT_FILE), 404)
        self.logTracker.assertContains('no matching streams were found, probably invalid segment index')

    def testClipFromLargerThanVideoDurationM3U8(self):
        assertRequestFails(self.getUrl(HLS_PREFIX, '/clipFrom/9999999' + HLS_PLAYLIST_FILE), 400)
        self.logTracker.assertContains('no matching streams were found')

    def testBadClipTo(self):
        assertRequestFails(self.getUrl(HLS_PREFIX, '/clipTo/abcd' + HLS_PLAYLIST_FILE), 400)
        self.logTracker.assertContains('clip to parser failed')

    def testBadClipFrom(self):
        assertRequestFails(self.getUrl(HLS_PREFIX, '/clipFrom/abcd' + HLS_PLAYLIST_FILE), 400)
        self.logTracker.assertContains('clip from parser failed')

class UpstreamTestSuite(TestSuite):
    def __init__(self, baseUrl, urlFile, serverPort):
        super(UpstreamTestSuite, self).__init__()
        self.baseUrl = baseUrl
        self.urlFile = urlFile
        self.serverPort = serverPort

    def testServerNotListening(self):
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), 502)
        self.logTracker.assertContains('connect() failed (111: Connection refused)')

    def testServerNotAccepting(self):
        s = createTcpServer(self.serverPort)
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), [502, 504])
        self.logTracker.assertContains('upstream timed out')

    def testServerNotResponding(self):
        TcpServer(self.serverPort, lambda s: socketSendAndWait(s, 'HTTP/1.1 200 OK', 10))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), [502, 504])
        self.logTracker.assertContains('upstream timed out')

    def testBadStatusLine(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'BAD STATUS LINE\r\n'))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.urlFile))
        try:
            response = urllib2.urlopen(request).read()
            assertEquals(response.strip(), 'BAD STATUS LINE')
        except urllib2.HTTPError as e:
            assertEquals(e.getcode(), 502)        # returns 502 in case the request was 'in memory'

        self.logTracker.assertContains('upstream sent no valid HTTP/1.0 header')

    def testBadContentLength(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nContent-Length: abcd\r\n'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), 502)
        self.logTracker.assertContains('upstream prematurely closed connection')

    def testInvalidHeader(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nBad\rHeader\r\n'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), 502)
        self.logTracker.assertContains('upstream sent invalid header')

    def testSocketShutdownWhileReadingHeaders(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nContent'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), 502)
        self.logTracker.assertContains('upstream prematurely closed connection')

class MemoryUpstreamTestSuite(UpstreamTestSuite):
    def __init__(self, baseUrl, urlFile, serverPort, upstreamHandler, validateResponse=validatePlaylistM3U8):
        super(MemoryUpstreamTestSuite, self).__init__(baseUrl, urlFile, serverPort)
        self.upstreamHandler = upstreamHandler
        self.validateResponse = validateResponse

    def testContentLengthTooBig(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nContent-Length: 99999999\r\n\r\n' + 'x' * 99999999))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), 502)
        self.logTracker.assertContains('upstream buffer is too small to read response')

    def testSocketShutdownWhileReadingBody(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, 'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc'))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), 502)
        self.logTracker.assertContains('upstream connection was closed with 7 bytes left to read')

    def testErrorHttpStatus(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, getHttpResponse('x', '400 Bad Request')))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), 502)
        self.logTracker.assertContains('upstream returned a bad status 400')

    def testHeadersForwardedToUpstream(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndHandle(s, 'mukka: ukk', None, self.upstreamHandler))
        url = getUniqueUrl(self.baseUrl, self.urlFile)
        request = urllib2.Request(url, headers={'mukka':'ukk'})
        response = urllib2.urlopen(request)
        self.validateResponse(response.read(), url)

    def testHideHeadersNotForwardedToUpstream(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndHandle(s, None, 'if-range', self.upstreamHandler))
        url = getUniqueUrl(self.baseUrl, self.urlFile)
        request = urllib2.Request(url, headers={'if-range':'ukk'})
        response = urllib2.urlopen(request)
        self.validateResponse(response.read(), url)

    def testUpstreamHostHeader(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndHandle(s, 'Host: blabla.com', None, self.upstreamHandler))
        url = getUniqueUrl(self.baseUrl, self.urlFile)
        request = urllib2.Request(url, headers={'Host': 'blabla.com'})
        response = urllib2.urlopen(request)
        self.validateResponse(response.read(), url.replace(NGINX_HOST, 'http://blabla.com'))

    def testUpstreamHostHeaderHttp10(self):
        if KEEPALIVE_PREFIX in self.baseUrl:
            return        # cannot test HTTP/1.0 with keepalive
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndHandle(s, 'http/1.0', 'Host:', self.upstreamHandler))
        url = getUniqueUrl(self.baseUrl, self.urlFile)
        body = sendHttp10Request(url)
        self.validateResponse(body, '')

class DumpUpstreamTestSuite(UpstreamTestSuite):
    def testRangeForwarded(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndSend(s, 'Range: bytes=5-10', getHttpResponse('abcde', '206 Partial Content')))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.urlFile), headers={'Range': 'bytes=5-10'})
        assertEquals(urllib2.urlopen(request).read(), 'abcde')

    def testHeadForwarded(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpVerbAndSend(s, 'HEAD', getHttpResponse(length=123)))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.urlFile))
        request.get_method = lambda : 'HEAD'
        headResponse = urllib2.urlopen(request)
        contentLength = headResponse.info().getheader('Content-Length')
        if getHttpResponse == getHttpResponseChunked and contentLength == None:
            return
        assertEquals(int(contentLength), 123)

    def testHeadersForwarded(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndSend(s, 'Shuki: tuki', getHttpResponse('abcde')))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.urlFile), headers={'Shuki': 'tuki'})
        assertEquals(urllib2.urlopen(request).read(), 'abcde')

    def testHeadersReturned(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, getHttpResponse('abcde', headers={'Kuki': 'Ukki'})))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.urlFile))
        response = urllib2.urlopen(request)
        assertEquals(response.info().getheader('Kuki'), 'Ukki')

    def testErrorHttpStatus(self):
        TcpServer(self.serverPort, lambda s: socketSendAndShutdown(s, getHttpResponse('blabla', status='402 Payment Required')))
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), 402, 'blabla')

    def testHideHeadersForwardedToUpstream(self):
        handler = lambda s, headers: socketSendAndShutdown(s, getHttpResponse('abcde'))
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndHandle(s, 'if-range: ukk', None, handler))
        url = getUniqueUrl(self.baseUrl, self.urlFile)
        request = urllib2.Request(url, headers={'if-range':'ukk'})
        assertEquals(urllib2.urlopen(request).read(), 'abcde')

class FallbackUpstreamTestSuite(DumpUpstreamTestSuite):
    def testLoopPreventionHeaderSent(self):
        TcpServer(self.serverPort, lambda s: socketExpectHttpHeaderAndSend(s, 'X-Kaltura-Proxy: dumpApiRequest', getHttpResponse('abcde')))
        request = urllib2.Request(getUniqueUrl(self.baseUrl, self.urlFile))
        assertEquals(urllib2.urlopen(request).read(), 'abcde')

    def testLoopPreventionHeaderReceived(self):
        assertRequestFails(getUniqueUrl(self.baseUrl, self.urlFile), 404, headers={'X-Kaltura-Proxy': 'dumpApiRequest'})
        self.logTracker.assertContains('proxy header exists')

class FileServeTestSuite(TestSuite):
    def __init__(self, getServeUrl):
        super(FileServeTestSuite, self).__init__()
        self.getServeUrl = getServeUrl

    def testNoReadAccessFile(self):
        assertRequestFails(self.getServeUrl(HLS_PREFIX, TEST_NO_READ_ACCESS_FILE) + HLS_PLAYLIST_FILE, 403)
        self.logTracker.assertContains('open() "%s" failed' % (TEST_FILES_ROOT + TEST_NO_READ_ACCESS_FILE))

    def testOpenFolder(self):
        assertRequestFails(self.getServeUrl(HLS_PREFIX, TEST_FOLDER) + HLS_PLAYLIST_FILE, 403)
        self.logTracker.assertContains('"%s" is not a file' % (TEST_FILES_ROOT + TEST_FOLDER))

    def testMetadataCache(self):
        for curPrefix, curRequest, _ in VOD_REQUESTS:
            linkPath = createRandomSymLink(TEST_FILES_ROOT + TEST_FLAVOR_FILE)
            url = self.getServeUrl(curPrefix, linkPath) + curRequest

            logTracker = LogTracker()
            uncachedResponse = urllib2.urlopen(url).read()
            logTracker.assertContains('metadata cache miss')

            logTracker = LogTracker()
            cachedResponse = urllib2.urlopen(url).read()
            logTracker.assertContains(['metadata cache hit', 'response cache hit'])

            assert(cachedResponse == uncachedResponse)

            cleanupStack.resetAndDestroy()

class ModeTestSuite(TestSuite):
    def __init__(self, baseUrl, encryptionPrefix=''):
        super(ModeTestSuite, self).__init__()
        self.baseUrl = baseUrl
        self.encryptionPrefix = encryptionPrefix

    def getBaseUrl(self, filePath):
        if filePath in map(lambda x: x[1], PD_REQUESTS):
            baseUrl = self.baseUrl.replace(HLS_PREFIX, '').replace(ENCRYPTED_PREFIX, '')
        else:
            baseUrl = self.baseUrl
        return baseUrl

    def getUrl(self, protocolPrefix, filePath):
        # encryption is supported only for hls
        encryptionPrefix = ''
        if protocolPrefix == HLS_PREFIX:
            encryptionPrefix = self.encryptionPrefix
        return getUniqueUrl(self.getBaseUrl(filePath) + protocolPrefix + encryptionPrefix + TEST_FLAVOR_URI, filePath)

class LocalTestSuite(ModeTestSuite):
    def runChildSuites(self):
        BasicTestSuite(
            self.getUrl,
            lambda: None).run()
        FallbackUpstreamTestSuite(self.baseUrl + HLS_PREFIX + TEST_NONEXISTING_FILE, HLS_PLAYLIST_FILE, FALLBACK_PORT).run()
        FileServeTestSuite(lambda protocolPrefix, uri: self.baseUrl + protocolPrefix + uri).run()

    def testFileNotFound(self):
        assertRequestFails(self.baseUrl + HLS_PREFIX + TEST_NONEXISTING_FILE + HLS_PLAYLIST_FILE, 502)    # 502 is due to failing to connect to fallback
        self.logTracker.assertContains(['open() "%s" failed' % (TEST_FILES_ROOT + TEST_NONEXISTING_FILE), 'stat() "%s" failed' % (TEST_FILES_ROOT + TEST_NONEXISTING_FILE)])

    def getUrl(self, protocolPrefix, filePath):
        # encryption is supported only for hls
        encryptionPrefix = ''
        if protocolPrefix == HLS_PREFIX:
            encryptionPrefix = self.encryptionPrefix
        return self.getBaseUrl(filePath) + protocolPrefix + encryptionPrefix + TEST_FLAVOR_FILE + filePath

class MappedTestSuite(ModeTestSuite):
    def runChildSuites(self):
        BasicTestSuite(
            self.getUrl,
            lambda: TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + TEST_FLAVOR_FILE)))).run()
        requestHandler = lambda s,h: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + TEST_FLAVOR_FILE))
        MemoryUpstreamTestSuite(self.baseUrl + HLS_PREFIX + TEST_FLAVOR_URI, HLS_PLAYLIST_FILE, API_SERVER_PORT, requestHandler).run()
        suite = FallbackUpstreamTestSuite(self.baseUrl + HLS_PREFIX + TEST_FLAVOR_URI, HLS_PLAYLIST_FILE, FALLBACK_PORT)
        suite.prepareTest = lambda: TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse('')))
        suite.run()
        FileServeTestSuite(self.getServeUrl).run()

    def getServeUrl(self, protocolPrefix, uri):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + uri)))
        return self.getUrl(protocolPrefix, uri)

    def testNoReadAccessFileMappingCached(self):
        url = self.getServeUrl(HLS_PREFIX, TEST_NO_READ_ACCESS_FILE) + HLS_PLAYLIST_FILE
        assertRequestFails(url, 403)
        assertRequestFails(url, 403)
        self.logTracker.assertContains('open() "%s" failed' % (TEST_FILES_ROOT + TEST_NO_READ_ACCESS_FILE))

    def testOpenFolderMappingCached(self):
        url = self.getServeUrl(HLS_PREFIX, TEST_FOLDER) + HLS_PLAYLIST_FILE
        assertRequestFails(url, 403)
        assertRequestFails(url, 403)
        self.logTracker.assertContains('"%s" is not a file' % (TEST_FILES_ROOT + TEST_FOLDER))

    def testEmptyPathMappingResponse(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getHttpResponse('')))
        assertRequestFails(self.getUrl(HLS_PREFIX, HLS_PLAYLIST_FILE), 404)
        self.logTracker.assertContains('empty mapping response')

    def testUnexpectedPathResponse(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getHttpResponse('abcde')))
        assertRequestFails(self.getUrl(HLS_PREFIX, HLS_PLAYLIST_FILE), 503)
        self.logTracker.assertContains('failed to parse json')

    def testEmptyPathResponse(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse('')))
        assertRequestFails(self.getUrl(HLS_PREFIX, HLS_PLAYLIST_FILE), 502)     # 502 is due to failing to connect to fallback
        self.logTracker.assertContains('empty path returned from upstream')

    def testFileNotFound(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_NONEXISTING_FILE)))
        assertRequestFails(self.getUrl(HLS_PREFIX, HLS_PLAYLIST_FILE), 404)
        self.logTracker.assertContains(['open() "%s" failed' % TEST_NONEXISTING_FILE, 'stat() "%s" failed' % TEST_NONEXISTING_FILE])

    def testPathMappingCache(self):
        TcpServer(API_SERVER_PORT, lambda s: socketSendAndShutdown(s, getPathMappingResponse(TEST_FILES_ROOT + TEST_FLAVOR_FILE)))
        for curPrefix, curRequest, contentType in ALL_REQUESTS:
            url = self.getUrl(curPrefix, curRequest)

            # uncached
            logTracker = LogTracker()
            response = urllib2.urlopen(url)
            assertEquals(response.info().getheader('Content-Type'), contentType)
            uncachedResponse = response.read()
            logTracker.assertContains('mapping cache miss')

            #cached
            logTracker = LogTracker()
            response = urllib2.urlopen(url)
            assertEquals(response.info().getheader('Content-Type'), contentType)
            cachedResponse = response.read()
            logTracker.assertContains(['mapping cache hit', 'response cache hit'])

            assert(cachedResponse == uncachedResponse)

class RemoteTestSuite(ModeTestSuite):
    def runChildSuites(self):
        BasicTestSuite(
            self.getUrl,
            lambda: TcpServer(API_SERVER_PORT, lambda s: serveFile(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE))).run()
        requestHandler = lambda s,h: serveFileHandler(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE, h)
        MemoryUpstreamTestSuite(self.baseUrl + HLS_PREFIX + TEST_FLAVOR_URI, HLS_PLAYLIST_FILE, API_SERVER_PORT, requestHandler).run()
        DumpUpstreamTestSuite(self.getBaseUrl('') + TEST_FLAVOR_URI, '', API_SERVER_PORT).run()      # non HLS URL will just dump to upstream

    def testMetadataCache(self):
        TcpServer(API_SERVER_PORT, lambda s: serveFile(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE))
        for curPrefix, curRequest, _ in VOD_REQUESTS:
            url = self.getUrl(curPrefix, curRequest)

            logTracker = LogTracker()
            uncachedResponse = urllib2.urlopen(url).read()
            logTracker.assertContains('metadata cache miss')

            logTracker = LogTracker()
            cachedResponse = urllib2.urlopen(url).read()
            logTracker.assertContains(['metadata cache hit', 'response cache hit'])

            assert(cachedResponse == uncachedResponse)

    def testErrorWhileProcessingFrames(self):
        # get the metadata into cache
        TcpServer(API_SERVER_PORT, lambda s: serveFile(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE))
        url = self.getUrl(HLS_PREFIX, HLS_SEGMENT_FILE)
        urllib2.urlopen(url).read()

        cleanupStack.resetAndDestroy()  # terminate the server
        try:
            urllib2.urlopen(url).read()
        except BadStatusLine:
            pass        # the error may be handled before the headers buffer is flushed
        except urllib2.HTTPError:
            pass        # this error is received when testing with keepalive
        self.logTracker.assertContains('read failed')

    def testZeroBytesRead(self):
        TcpServer(API_SERVER_PORT, lambda s: serveFile(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE, 4096))
        assertRequestFails(self.getUrl(HLS_PREFIX, HLS_PLAYLIST_FILE), 404)
        self.logTracker.assertContains('bytes read is zero')

class DrmTestSuite(ModeTestSuite):
    def runChildSuites(self):
        requestHandler = lambda s,h: socketSendAndShutdown(s, getHttpResponse(DRM_SERVICE_RESPONSE))
        suite = MemoryUpstreamTestSuite(self.baseUrl + EDASH_PREFIX + TEST_FLAVOR_URI, DASH_MANIFEST_FILE, DRM_SERVER_PORT, requestHandler, lambda buffer, url: None)
        suite.prepareTest = lambda: TcpServer(API_SERVER_PORT, lambda s: serveFile(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE))
        suite.run()

    def testInfoParseError(self):
        TcpServer(DRM_SERVER_PORT, lambda s: socketSendAndShutdown(s, getHttpResponse('null')))
        assertRequestFails(self.getUrl(EDASH_PREFIX, DASH_MANIFEST_FILE), 503)
        self.logTracker.assertContains('invalid drm info response null')

    def testDrmInfoCache(self):
        TcpServer(API_SERVER_PORT, lambda s: serveFile(s, TEST_FILES_ROOT + TEST_FLAVOR_FILE, TEST_FILE_TYPE))
        TcpServer(DRM_SERVER_PORT, lambda s: socketSendAndShutdown(s, getHttpResponse(DRM_SERVICE_RESPONSE)))
        url = self.getUrl(EDASH_PREFIX, DASH_MANIFEST_FILE)

        logTracker = LogTracker()
        missResponse = urllib2.urlopen(url).read()
        logTracker.assertContains('drm info cache miss')

        logTracker = LogTracker()
        hitResponse = urllib2.urlopen(url.replace(DASH_MANIFEST_FILE, '/manifest-v1-a1.mpd')).read()
        logTracker.assertContains('drm info cache hit')

        assert(missResponse == hitResponse)

class MainTestSuite(TestSuite):
    def runChildSuites(self):
        DrmTestSuite(NGINX_REMOTE).run()

        # all combinations of (encrypted, non encrypted) x (keep alive, no keep alive)
        for encryptionPrefix in [ENCRYPTED_PREFIX, '']:
            for keepAlivePrefix in [KEEPALIVE_PREFIX, '']:
                print('-- keepAlivePrefix=%s, encryptionPrefix=%s' % (keepAlivePrefix, encryptionPrefix))
                LocalTestSuite(NGINX_HOST + keepAlivePrefix + '/tlocal', encryptionPrefix).run()
                MappedTestSuite(NGINX_HOST + keepAlivePrefix + '/tmapped', encryptionPrefix).run()
                RemoteTestSuite(NGINX_HOST + keepAlivePrefix + '/tremote', encryptionPrefix).run()

for getHttpResponse in [getHttpResponseChunked, getHttpResponseRegular]:
    for socketSend in [socketSendRegular, socketSendByteByByte]:
        print('-- socketSend=%s, getHttpResponse=%s' % (
            'regular' if socketSend == socketSendRegular else 'byteByByte',
            'chunked' if getHttpResponse == getHttpResponseChunked else 'regular'))
        MainTestSuite().run()
