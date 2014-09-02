from KalturaClient import *
from KalturaClient.Plugins.Core import *
import logging
import urllib2
import struct
import shutil
import sys
import os
import re

logging.basicConfig(level = logging.DEBUG,
                    format = '%(asctime)s %(levelname)s %(message)s',
                    stream = sys.stdout)

# UPDATE THIS
PARTNER_ID = 437481
ADMIN_SECRET = "770d2fee3ad9d50ca22f62725ca7aea8"
SERVICE_URL = "http://www.kaltura.com"
USER_NAME = "admin"
TEST_FLAVOR_URL = 'http://www.kaltura.com/p/99/sp/9900/serveFlavor/entryId/0_ci8ffv43/v/1/flavorId/0_o2yj6rxx'
TEMP_DOWNLOAD_PATH = r'c:\temp\temp.mp4'
TEMP_CONVERT_PATH = r'c:\temp\convert.mp4'
FFMPEG_BIN = r'C:\Users\eran.kornblau\Downloads\ffmpeg-20140723-git-a613257-win64-static\bin\ffmpeg.exe'
QTFASTSTART_BIN = 'python -m qtfaststart'       # can install https://github.com/danielgtaylor/qtfaststart
REFERENCE_ID_PREFIX = 'NGINX_VOD_TESTS_'

# kaltura client functions
class KalturaLogger(IKalturaLogger):
    def log(self, msg):
        #logging.info(msg)
        pass

def GetConfig():
    config = KalturaConfiguration(PARTNER_ID)
    config.serviceUrl = SERVICE_URL
    config.setLogger(KalturaLogger())
    return config

# http utilities
def retrieveUrl(url, fileName):
    r = urllib2.urlopen(urllib2.Request(url))
    with file(fileName, 'wb') as w:
        shutil.copyfileobj(r,w)
    r.close()

# mp4 parsing
def isContainerAtom(inputData, startOffset, endOffset):
    if endOffset - startOffset < 8:
        return False
    if not re.match('^[0-9a-zA-Z]+$', inputData[(startOffset + 4):(startOffset + 8)]):
        return False
    atomSize = struct.unpack('>L', inputData[startOffset:(startOffset + 4)])[0]
    if atomSize > endOffset - startOffset:
        return False
    return True

def parseAtoms(inputData, startOffset, endOffset, indent):
    result = {}
    curPos = startOffset
    while curPos + 8 <= endOffset:
        # get atom size and atom name
        atomSize = struct.unpack('>L', inputData[curPos:(curPos + 4)])[0]
        atomName = inputData[(curPos + 4):(curPos + 8)]
        if atomSize == 1:
            atomSize = struct.unpack('>Q', inputData[(curPos + 8):(curPos + 16)])[0]
            atomHeaderSize = 16
        else:
            atomHeaderSize = 8
            if atomSize == 0:
                atomSize = endOffset - curPos

        # get child atoms
        childAtoms = {}
        if isContainerAtom(inputData, curPos + atomHeaderSize, curPos + atomSize):
            childAtoms = parseAtoms(inputData, curPos + atomHeaderSize, curPos + atomSize, indent + '  ')

        # add the result
        result[atomName] = (curPos, atomHeaderSize, curPos + atomSize, childAtoms)
        curPos += atomSize
    return result

def getAtomPos(name):
    return inputAtoms[name][0]

def getAtomEndPos(name):
    return inputAtoms[name][2]

# string reader (for uploading a string as a file)
class StringReader:
    def __init__(self, inputStr, name):
        self.inputStr = inputStr
        self.curPos = 0
        self.name = name

    def read(self, maxSize=None):
        if maxSize == None:
            maxSize = len(self.inputStr)
        result = self.inputStr[self.curPos:(self.curPos + maxSize)]
        self.curPos += len(result)
        return result

    def seek(self, offset, whence=os.SEEK_SET):
        if whence == os.SEEK_SET:
            self.curPos = 0
        elif whence == os.SEEK_END:
            self.curPos = len(self.inputStr)
        self.curPos += offset

    def tell(self):
        return self.curPos

# test file generators
def convertWithFfmpeg(params):
    cmdLine = '%s -i %s %s -f mp4 -y %s' % (FFMPEG_BIN, TEMP_DOWNLOAD_PATH, params, TEMP_CONVERT_PATH)
    os.system(cmdLine)
    os.system('%s %s' % (QTFASTSTART_BIN, TEMP_CONVERT_PATH))
    return file(TEMP_CONVERT_PATH, 'rb')

def applyPatch(offset, replacement):
    return StringReader(inputData[:offset] + replacement + inputData[(offset + len(replacement)):], 'testFile.mp4')

def truncateFile(size):
    return StringReader(inputData[:size], 'testFile.mp4')

# download and read the input file
if not os.path.exists(TEMP_DOWNLOAD_PATH):
    retrieveUrl(TEST_FLAVOR_URL, TEMP_DOWNLOAD_PATH)

inputData = file(TEMP_DOWNLOAD_PATH, 'rb').read()

inputAtoms = parseAtoms(inputData, 0, len(inputData), '')

# initialize the api client
client = KalturaClient(GetConfig())

ks = client.generateSession(ADMIN_SECRET, USER_NAME, KalturaSessionType.ADMIN, PARTNER_ID, 86400, "")
client.setKs(ks)

# get source only conversion profile
sourceOnlyConvProfileId = None
for conversionProfile in client.conversionProfile.list().objects:
    if conversionProfile.name.lower() == 'source only':
        sourceOnlyConvProfileId = conversionProfile.id

if sourceOnlyConvProfileId == None:
    print 'Failed to find a source only conversion profile'
    sys.exit(1)

# test definitions
TEST_CASES = [
    ('NON_AAC_AUDIO', lambda: convertWithFfmpeg('-vcodec copy -codec:a libmp3lame'), 'unsupported format - media type 2'),
    ('NON_H264_VIDEO', lambda: convertWithFfmpeg('-acodec copy -c:v mpeg4'), 'unsupported format - media type 1'),
    ('VIDEO_ONLY', lambda: convertWithFfmpeg('-vcodec copy -an'), None),
    ('AUDIO_ONLY', lambda: convertWithFfmpeg('-acodec copy -vn'), None),
    ('MOOV_TOO_BIG', lambda: applyPatch(getAtomPos('moov'), struct.pack('>L', 500000000)), 'moov size 499999992 exceeds the max'),
    ('TRUNCATED_MOOV', lambda: truncateFile(getAtomEndPos('moov') - 100), None),
    ('TRUNCATED_MDAT', lambda: truncateFile(getAtomPos('mdat') + 1000), None),
]

# upload missing test files
entryMap = {}
for refId, generator, message in TEST_CASES:
    refId = REFERENCE_ID_PREFIX + refId
    listResult = client.media.list(filter=KalturaMediaEntryFilter(referenceIdEqual=refId))
    if len(listResult.objects) > 0:
        entryMap[listResult.objects[0].id] = (message, refId)
        continue
    
    newEntry = client.media.add(entry=KalturaMediaEntry(name=refId, referenceId=refId, mediaType=KalturaMediaType.VIDEO, conversionProfileId=sourceOnlyConvProfileId))
    uploadToken = client.uploadToken.add(uploadToken=KalturaUploadToken())
    client.media.addContent(entryId=newEntry.id, resource=KalturaUploadedFileTokenResource(token=uploadToken.id))
    client.uploadToken.upload(uploadTokenId=uploadToken.id, fileData=generator())
    entryMap[newEntry.id] = (message, refId)

# print the test uris
print 'result:'
flavors = client.flavorAsset.list(filter=KalturaFlavorAssetFilter(flavorParamsIdEqual=0, entryIdIn=','.join(entryMap.keys()))).objects
for flavor in flavors:
    message, refId = entryMap[flavor.entryId]
    print '%s /p/%s/sp/%s00/serveFlavor/entryId/%s/v/%s/flavorId/%s %s' % (refId, PARTNER_ID, PARTNER_ID, flavor.entryId, flavor.version, flavor.id, message)
