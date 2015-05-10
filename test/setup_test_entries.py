from KalturaClient import *
from KalturaClient.Plugins.Core import *
import logging
import urllib2
import struct
import shutil
import sys
import os
import re

from setup_test_entries_params import *

REFERENCE_ID_PREFIX = 'NGINX_VOD_TESTS_'

logging.basicConfig(level = logging.DEBUG,
                    format = '%(asctime)s %(levelname)s %(message)s',
                    stream = sys.stdout)

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
        result.setdefault(atomName, [])
        result[atomName].append((curPos, atomHeaderSize, curPos + atomSize, childAtoms))
        curPos += atomSize
    return result

def getAtomInternal(splittedPath, baseAtom):
    if not baseAtom.has_key(splittedPath[0]):
        return None
    for curAtom in baseAtom[splittedPath[0]][::-1]:
        if len(splittedPath) == 1:
            return curAtom
        result = getAtomInternal(splittedPath[1:], curAtom[3])
        if result != None:
            return result
    return None

def getAtom(path):
    return getAtomInternal(path.split('.'), inputAtoms)

def atomExists(path):
    return getAtom(path) != None

def getAtomPos(path):
    return getAtom(path)[0]

def getAtomEndPos(path):
    return getAtom(path)[2]

def getAtomDataPos(path):
    atom = getAtom(path)
    return atom[0] + atom[1]

# string reader (for uploading a string as a file)
class StringReader:
    def __init__(self, inputStr, name='testFile.mp4'):
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
def convertWithFfmpeg(params, fastStart=True):
    cmdLine = '%s -i %s %s -f mp4 -y %s' % (FFMPEG_BIN, TEMP_DOWNLOAD_PATH, params, TEMP_CONVERT_PATH)
    os.system(cmdLine)
    if fastStart:
        os.system('%s %s' % (QTFASTSTART_BIN, TEMP_CONVERT_PATH))
    return file(TEMP_CONVERT_PATH, 'rb')

def applyPatch(*args):
    result = inputData
    for offset, replacement in zip(args[::2], args[1::2]):
        result = result[:offset] + replacement + result[(offset + len(replacement)):]
    return StringReader(result)

def truncateFile(size):
    return StringReader(inputData[:size])

def truncateAtom(name, newSize):
    atomPos, atomHeaderSize, atomEndPos, _ = getAtom(name)
    if atomHeaderSize != 8:
        raise Exception('only supported for atoms with 32 bit size')
    newAtomEndPos = atomPos + atomHeaderSize + newSize
    if newAtomEndPos + 8 > atomEndPos:
        raise Exception('not enough room to create padding atom')
    return StringReader(inputData[:atomPos] + struct.pack('>L', atomHeaderSize + newSize) + inputData[(atomPos + 4):newAtomEndPos] +
        struct.pack('>L', atomEndPos - newAtomEndPos) + 'padd' + inputData[(newAtomEndPos + 8):])

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

def getTimeScaleOffset():
    return getAtomDataPos('moov.trak.mdia.mdhd') + 12

# test definitions
TEST_CASES = [
    ('VIDEO_ONLY', lambda: convertWithFfmpeg('-vcodec copy -an'), None, 'index.m3u8', 200),
    ('AUDIO_ONLY', lambda: convertWithFfmpeg('-acodec copy -vn'), None, 'index.m3u8', 200),
    ('NON_FAST_START', lambda: convertWithFfmpeg('-acodec copy -vcodec copy', False), None, 'index.m3u8', 200),
    ('NON_AAC_AUDIO', lambda: convertWithFfmpeg('-vcodec copy -codec:a libmp3lame'), 'unsupported format - media type 1', 'index.m3u8', 200),
    ('NON_H264_VIDEO', lambda: convertWithFfmpeg('-acodec copy -c:v mpeg4'), 'unsupported format - media type 0', 'index.m3u8', 200),
    ('CANT_FIND_MOOV', lambda: applyPatch(getAtomPos('moov') + 4, 'blah'), ' is smaller than the atom start offset ', 'index.m3u8', 404),
    ('MOOV_TOO_BIG', lambda: applyPatch(getAtomPos('moov'), struct.pack('>L', 500000000)), 'moov size 499999992 exceeds the max', 'index.m3u8', 404),
    ('TRUNCATED_MOOV', lambda: truncateFile(getAtomEndPos('moov') - 100), 'is smaller than moov end offset', 'index.m3u8', 404),
    ('TRUNCATED_MDAT', lambda: truncateFile(getAtomPos('mdat') + 1000), 'no data was handled, probably a truncated file', 'seg-1.ts', 0),
    ('NO_EXTRA_DATA', lambda: StringReader(inputData.replace('esds', 'blah').replace('avcC', 'blah')), 'no extra data was parsed for track', 'seg-1.ts', 404),
    ('MOOV_READ_ATTEMPTS', lambda: StringReader((struct.pack('>L', 0x1008) + 'blah' + '\0' * 0x1000) * 5), 'exhausted all moov read attempts', 'seg-1.ts', 404),
    ('NO_ATOM_PARSED', lambda: StringReader(struct.pack('>L', 7) + 'moov'), 'failed to parse any atoms', 'seg-1.ts', 404),
    
    # atom parsing
    ('MISSING_64BIT_ATOM_SIZE', lambda: StringReader(struct.pack('>L', 1) + 'moov' + ('\0' * 7)), 'atom size is 1 but there is not enough room for the 64 bit size', 'index.m3u8', 404),
    ('ATOM_SIZE_LESS_THAN_HEADER', lambda: StringReader(struct.pack('>L', 7) + 'moov'), 'atom size 7 is less than the atom header size', 'index.m3u8', 404),
    ('ATOM_SIZE_OVERFLOW', lambda: StringReader(struct.pack('>L', 100) + 'ftyp'), 'atom size 92 overflows the input stream size', 'index.m3u8', 404),
    # hdlr
    ('HDLR_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.hdlr', 4), 'mp4_parser_parse_hdlr_atom: atom size 4 too small', 'index.m3u8', 404),
    ('BAD_MEDIA_TYPE', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.hdlr') + 8, 'blah'), 'unsupported format - media type 3', 'index.m3u8', 200),
    # mdhd
    ('MDHD_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.mdhd', 4), 'mp4_parser_parse_mdhd_atom: atom size 4 too small', 'index.m3u8', 404),
    ('ZERO_TIMESCALE', lambda: applyPatch(getTimeScaleOffset(), struct.pack('>L', 0)), 'time scale is zero', 'index.m3u8', 404),
    # stts
    ('STTS_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stts', 4), 'mp4_parser_parse_stts_atom: atom size 4 too small', 'seg-1.ts', 404),
    ('STTS_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stts') + 4, struct.pack('>L', 1000000000)), 'mp4_parser_parse_stts_atom: number of entries 1000000000 too big', 'seg-1.ts', 404),
    ('STTS_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stts') + 4, struct.pack('>L', 100000000)), 'too small to hold 100000000 entries', 'seg-1.ts', 404),
    ('STTS_INITIAL_ALLOC_BIG', lambda: applyPatch(
        getAtomDataPos('moov.trak.mdia.minf.stbl.stts') + 4, struct.pack('>LLL', 1, 100, 1, 404),
        getTimeScaleOffset(), struct.pack('>L', 100000)), 'initial alloc size 1000001 exceeds the max frame count', 'seg-1.ts', 404),
    # stco
    ('STCO_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stco', 4), 'mp4_parser_parse_stco_atom: atom size 4 too small', 'seg-1.ts', 404),
    ('STCO_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stco') + 4, struct.pack('>L', 1000000000)), 'mp4_parser_parse_stco_atom: number of entries 1000000000 too big', 'seg-1.ts', 404),
    ('STCO_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stco') + 4, struct.pack('>L', 100000000)), 'too small to hold 100000000 entries', 'seg-1.ts', 404),
    ('STCO_TOO_FEW_ENTRIES', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stco') + 4, struct.pack('>L', 0)), 'number of entries 0 smaller than last', 'seg-1.ts', 404),
    # stsc
    ('STSC_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stsc', 4), 'mp4_parser_parse_stsc_atom: atom size 4 too small', 'seg-1.ts', 404),
    ('STSC_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsc') + 4, struct.pack('>L', 1000000000)), 'mp4_parser_parse_stsc_atom: number of entries 1000000000 too big', 'seg-1.ts', 404),
    ('STSC_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsc') + 4, struct.pack('>L', 100000000)), 'too small to hold 100000000 entries', 'seg-1.ts', 404),
    ('STSC_ZERO_CHUNK_INDEX', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsc') + 8, struct.pack('>L', 0)), 'chunk index is zero', 'seg-1.ts', 404),
    ('STSC_INVALID_SPC', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsc') + 4, struct.pack('>LLL', 1, 1, 0)), 'samples per chunk is zero', 'seg-1.ts', 404),
    # stsz
    ('STSZ_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stsz', 4), 'mp4_parser_validate_stsz_atom: atom size 4 too small', 'seg-1.ts', 404),
    ('STSZ_UNIFORM_SIZE_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 4, struct.pack('>L', 100000000)), 'uniform size 100000000 is too big', 'seg-1.ts', 404),
    ('STSZ_ENTRIES_TOO_SMALL', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 8, struct.pack('>L', 1)), 'number of entries 1 smaller than last frame', 'seg-1.ts', 404),
    ('STSZ_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 8, struct.pack('>L', 1000000000)), 'mp4_parser_validate_stsz_atom: number of entries 1000000000 too big', 'seg-1.ts', 404),
    ('STSZ_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 8, struct.pack('>L', 50000000)), 'too small to hold 50000000 entries', 'seg-1.ts', 404),
    ('STSZ_FRAME_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 12, struct.pack('>L', 100000000)), 'frame size 100000000 too big', 'seg-1.ts', 404),
    # stss
    ('STSS_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stss', 4), 'mp4_parser_validate_stss_atom: atom size 4 too small', 'seg-1.ts', 404),
    ('STSS_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stss') + 4, struct.pack('>L', 1000000000)), 'mp4_parser_validate_stss_atom: number of entries 1000000000 too big', 'seg-1.ts', 404),
    ('STSS_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stss') + 4, struct.pack('>L', 100000000)), 'too small to hold 100000000 entries', 'seg-1.ts', 404),
    # stsd
    ('STSD_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stsd', 4), 'mp4_parser_parse_stsd_atom: atom size 4 too small', 'seg-1.ts', 404),
    ('STSD_NO_ROOM_FOR_ENTRY', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stsd', 12), 'not enough room for stsd entry', 'seg-1.ts', 404),
]

if atomExists('moov.trak.mdia.minf.stbl.ctts'):
    TEST_CASES += [
        # ctts
        ('CTTS_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.ctts', 4), 'mp4_parser_parse_ctts_atom: atom size 4 too small', 'seg-1.ts', 404),
        ('CTTS_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.ctts') + 4, struct.pack('>L', 1000000000)), 'mp4_parser_parse_ctts_atom: number of entries 1000000000 too big', 'seg-1.ts', 404),
        ('CTTS_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.ctts') + 4, struct.pack('>L', 100000000)), 'too small to hold 100000000 entries', 'seg-1.ts', 404),
    ]

def getTestEntry(refId, generator):
    refId = REFERENCE_ID_PREFIX + refId
    listResult = client.media.list(filter=KalturaMediaEntryFilter(referenceIdEqual=refId))
    if len(listResult.objects) > 0:
        return listResult.objects[0].id

    print 'uploading %s' % refId
    newEntry = client.media.add(entry=KalturaMediaEntry(name=refId, referenceId=refId, mediaType=KalturaMediaType.VIDEO, conversionProfileId=sourceOnlyConvProfileId))
    uploadToken = client.uploadToken.add(uploadToken=KalturaUploadToken())
    client.media.addContent(entryId=newEntry.id, resource=KalturaUploadedFileTokenResource(token=uploadToken.id))
    client.uploadToken.upload(uploadTokenId=uploadToken.id, fileData=generator())
    return newEntry.id
    

# upload missing test files
entryMap = {}
for refId, generator, message, fileName, statusCode in TEST_CASES:
    entryId = getTestEntry(refId, generator)
    entryMap[entryId] = (refId, fileName, message, statusCode)

# print the test uris
print 'result:'
flavors = client.flavorAsset.list(filter=KalturaFlavorAssetFilter(flavorParamsIdEqual=0, entryIdIn=','.join(entryMap.keys())),
                                  pager=KalturaFilterPager(pageSize=500)).objects
for flavor in flavors:
    refId, fileName, message, statusCode = entryMap[flavor.entryId]
    print '%s /p/%s/sp/%s00/serveFlavor/entryId/%s/v/%s/flavorId/%s/%s %s %s' % (refId, PARTNER_ID, PARTNER_ID, flavor.entryId, flavor.version, flavor.id, fileName, statusCode, message)
