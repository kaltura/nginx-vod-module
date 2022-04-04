from __future__ import print_function
from KalturaClient import *
from KalturaClient.Plugins.Core import *
from mp4_utils import *
import http_utils
import logging
import struct
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

def atomExists(path):
    return getAtom(inputAtoms, path) != None

def getAtomPos(path):
    return getAtom(inputAtoms, path)[0]

def getAtomEndPos(path):
    return getAtom(inputAtoms, path)[2]

def getAtomDataPos(path):
    atom = getAtom(inputAtoms, path)
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
    atomPos, atomHeaderSize, atomEndPos, _ = getAtom(inputAtoms, name)
    if atomHeaderSize != 8:
        raise Exception('only supported for atoms with 32 bit size')
    newAtomEndPos = atomPos + atomHeaderSize + newSize
    if newAtomEndPos + 8 > atomEndPos:
        raise Exception('not enough room to create padding atom')
    return StringReader(inputData[:atomPos] + struct.pack('>L', atomHeaderSize + newSize) + inputData[(atomPos + 4):newAtomEndPos] +
        struct.pack('>L', atomEndPos - newAtomEndPos) + 'padd' + inputData[(newAtomEndPos + 8):])

# download and read the input file
if not os.path.exists(TEMP_DOWNLOAD_PATH):
    http_utils.downloadUrl(TEST_FLAVOR_URL, TEMP_DOWNLOAD_PATH)

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
    print('Failed to find a source only conversion profile')
    sys.exit(1)

def getTimeScaleOffset():
    return getAtomDataPos('moov.trak.mdia.mdhd') + 12

# test definitions
TEST_CASES = [
    ('VIDEO_ONLY', lambda: convertWithFfmpeg('-vcodec copy -an'), [
        ('/hls', 'index.m3u8', 200, None),
        ('', 'clipTo/10000/a.mp4', 200, None),
    ]),
    ('AUDIO_ONLY', lambda: convertWithFfmpeg('-acodec copy -vn'), [
        ('/hls', 'index.m3u8', 200, None),
        ('', 'clipTo/10000/a.mp4', 200, None),
    ]),
    ('NON_FAST_START', lambda: convertWithFfmpeg('-acodec copy -vcodec copy', False), [
        ('/hls', 'index.m3u8', 200, None),
        ('', 'clipTo/10000/a.mp4', 200, None),
    ]),
    ('NON_H264_VIDEO', lambda: convertWithFfmpeg('-acodec copy -c:v mpeg4'), [
        ('/hls', 'index.m3u8', 200, 'unsupported format - media type 0'),
        ('', 'clipTo/10000/a.mp4', 200, None),
    ]),
    ('CANT_FIND_FTYP', lambda: applyPatch(getAtomPos('ftyp') + 4, 'blah'), [
        ('/hls', 'index.m3u8', 200, 'ftyp atom not found'),
        ('', 'clipTo/10000/a.mp4', 200, 'ftyp atom not found'),
    ]),
    ('CANT_FIND_MOOV', lambda: applyPatch(getAtomPos('moov') + 4, 'blah'), [
        ('/hls', 'index.m3u8', 404, 'failed to parse any atoms'),
        ('', 'clipTo/10000/a.mp4', 404, 'failed to parse any atoms'),
    ]),
    ('MOOV_TOO_BIG', lambda: applyPatch(getAtomPos('moov'), struct.pack('>L', 500000000)), [
        ('/hls', 'index.m3u8', 404, 'moov size 499999992 exceeds the max'),
        ('', 'clipTo/10000/a.mp4', 404, 'moov size 499999992 exceeds the max'),
    ]),
    ('TRUNCATED_MOOV', lambda: truncateFile(getAtomEndPos('moov') - 100), [
        ('/hls', 'index.m3u8', 404, 'is smaller than moov size'),
        ('', 'clipTo/10000/a.mp4', 404, 'is smaller than moov size'),
    ]),
    ('TRUNCATED_MDAT', lambda: truncateFile(getAtomPos('mdat') + 1000), [
        ('/hls', 'seg-1.ts', 0, 'no data was handled, probably a truncated file'),
        ('', 'clipTo/10000/a.mp4', 200, 'probably a truncated file'),
    ]),
    ('NO_EXTRA_DATA', lambda: StringReader(inputData.replace('esds', 'blah').replace('avcC', 'blah')), [
        ('/hls', 'seg-1.ts', 404, 'no extra data was parsed for track'),
        ('', 'clipTo/10000/a.mp4', 200, None),
    ]),
    ('UNRECOGNIZED_FORMAT', lambda: StringReader('x' * 0x5000), [
        ('/hls', 'seg-1.ts', 404, 'failed to identify the file format'),
        ('', 'clipTo/10000/a.mp4', 404, 'failed to identify the file format'),
    ]),
    ('MOOV_READ_ATTEMPTS', lambda: StringReader((struct.pack('>L', 0x1008) + 'mdat' + '\0' * 0x1000) * 5), [
        ('/hls', 'seg-1.ts', 404, 'exhausted all moov read attempts'),
        ('', 'clipTo/10000/a.mp4', 404, 'exhausted all moov read attempts'),
    ]),
    ('NO_ATOM_PARSED', lambda: StringReader(struct.pack('>L', 8) + 'ftyp' + struct.pack('>L', 7) + 'moov'), [
        ('/hls', 'seg-1.ts', 404, 'is smaller than the buffer size'),
        ('', 'clipTo/10000/a.mp4', 404, 'is smaller than the buffer size'),
    ]),
    
    # atom parsing
    ('MISSING_64BIT_ATOM_SIZE', lambda: StringReader(struct.pack('>L', 8) + 'ftyp' + struct.pack('>L', 1) + 'moov' + ('\0' * 7)), [
        ('/hls', 'index.m3u8', 404, 'is smaller than the buffer size'),
        ('', 'clipTo/10000/a.mp4', 404, 'is smaller than the buffer size'),
    ]),
    ('ATOM_SIZE_LESS_THAN_HEADER', lambda: StringReader(struct.pack('>L', 8) + 'ftyp' + struct.pack('>L', 16) + 'moov' + struct.pack('>L', 7) + 'trak'), [
        ('/hls', 'index.m3u8', 404, 'atom size 7 is less than the atom header size'),
        ('', 'clipTo/10000/a.mp4', 404, 'atom size 7 is less than the atom header size'),
    ]),
    ('ATOM_SIZE_OVERFLOW', lambda: StringReader(struct.pack('>L', 100) + 'ftyp'), [
        ('/hls', 'index.m3u8', 404, 'atom size 92 overflows the input stream size'),
        ('', 'clipTo/10000/a.mp4', 404, 'atom size 92 overflows the input stream size'),
    ]),
    # hdlr
    ('HDLR_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.hdlr', 4), [
        ('/hls', 'index.m3u8', 404, 'mp4_parser_parse_hdlr_atom: atom size 4 too small'),
        ('', 'clipTo/10000/a.mp4', 200, None),
    ]),
    ('BAD_MEDIA_TYPE', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.hdlr') + 8, 'blah'), [
        ('/hls', 'index.m3u8', 200, 'unsupported format - media type 4'),
        ('', 'clipTo/10000/a.mp4', 200, None),
    ]),
    # mdhd
    ('MDHD_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.mdhd', 4), [
        ('/hls', 'index.m3u8', 404, 'mp4_parser_parse_mdhd_atom: atom size 4 too small'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_clipper_mdhd_clip_data: atom size 4 too small'),
    ]),
    ('ZERO_TIMESCALE', lambda: applyPatch(getTimeScaleOffset(), struct.pack('>L', 0)), [
        ('/hls', 'index.m3u8', 404, 'timescale is zero'),
        ('', 'clipTo/10000/a.mp4', 404, 'timescale is zero'),
    ]),
    # stts
    ('STTS_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stts', 4), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stts_data: atom size 4 too small'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_parser_validate_stts_data: atom size 4 too small'),
    ]),
    ('STTS_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stts') + 4, struct.pack('>L', 1000000000)), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stts_data: number of entries 1000000000 too big'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_parser_validate_stts_data: number of entries 1000000000 too big'),
    ]),
    ('STTS_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stts') + 4, struct.pack('>L', 100000000)), [
        ('/hls', 'seg-1.ts', 404, 'too small to hold 100000000 entries'),
        ('', 'clipTo/10000/a.mp4', 404, 'too small to hold 100000000 entries'),
    ]),
    ('STTS_INITIAL_ALLOC_BIG', lambda: applyPatch(
        getAtomDataPos('moov.trak.mdia.minf.stbl.stts') + 4, struct.pack('>LLL', 1, 10000000, 1),
        getTimeScaleOffset(), struct.pack('>L', 100000)), [
        ('/hls', 'seg-1.ts', 404, 'initial alloc size 1000001 exceeds the max frame count'),
    ]),
    # stco
    ('STCO_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stco', 4), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stco_data: atom size 4 too small'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_clipper_stco_init_chunk_count: atom size 4 too small'),
    ]),
    ('STCO_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stco') + 4, struct.pack('>L', 1000000000)), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stco_data: number of entries 1000000000 too big'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_parser_validate_stco_data: number of entries 1000000000 too big'),
    ]),
    ('STCO_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stco') + 4, struct.pack('>L', 100000000)), [
        ('/hls', 'seg-1.ts', 404, 'too small to hold 100000000 entries'),
        ('', 'clipTo/10000/a.mp4', 404, 'too small to hold 100000000 entries'),
    ]),
    ('STCO_TOO_FEW_ENTRIES', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stco') + 4, struct.pack('>L', 0)), [
        ('/hls', 'seg-1.ts', 404, 'number of entries 0 smaller than last'),
        ('', 'clipTo/10000/a.mp4', 404, 'number of entries 0 smaller than last'),
    ]),
    # stsc
    ('STSC_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stsc', 4), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stsc_atom: atom size 4 too small'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_parser_validate_stsc_atom: atom size 4 too small'),
    ]),
    ('STSC_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsc') + 4, struct.pack('>L', 1000000000)), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stsc_atom: number of entries 1000000000 too big'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_parser_validate_stsc_atom: number of entries 1000000000 too big'),
    ]),
    ('STSC_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsc') + 4, struct.pack('>L', 100000000)), [
        ('/hls', 'seg-1.ts', 404, 'too small to hold 100000000 entries'),
        ('', 'clipTo/10000/a.mp4', 404, 'too small to hold 100000000 entries'),
    ]),
    ('STSC_ZERO_CHUNK_INDEX', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsc') + 8, struct.pack('>L', 0)), [
        ('/hls', 'seg-1.ts', 404, 'first chunk index is not 1'),
        ('', 'clipTo/10000/a.mp4', 404, 'chunk index is zero'),
    ]),
    ('STSC_INVALID_SPC', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsc') + 4, struct.pack('>LLL', 1, 1, 0)), [
        ('/hls', 'seg-1.ts', 404, 'samples per chunk is zero'),
        ('', 'clipTo/10000/a.mp4', 404, 'samples per chunk is zero'),
    ]),
    # stsz
    ('STSZ_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stsz', 4), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stsz_atom: atom size 4 too small'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_parser_validate_stsz_atom: atom size 4 too small'),
    ]),
    ('STSZ_UNIFORM_SIZE_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 4, struct.pack('>L', 100000000)), [
        ('/hls', 'seg-1.ts', 404, 'uniform size 100000000 is too big'),
        ('', 'clipTo/10000/a.mp4', 404, 'uniform size 100000000 is too big'),
    ]),
    ('STSZ_ENTRIES_TOO_SMALL', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 8, struct.pack('>L', 1)), [
        ('/hls', 'seg-1.ts', 404, 'number of entries 1 smaller than last frame'),
        ('', 'clipTo/10000/a.mp4', 404, 'number of entries 1 smaller than last frame'),
    ]),
    ('STSZ_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 8, struct.pack('>L', 1000000000)), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stsz_atom: number of entries 1000000000 too big'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_parser_validate_stsz_atom: number of entries 1000000000 too big'),
    ]),
    ('STSZ_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 8, struct.pack('>L', 50000000)), [
        ('/hls', 'seg-1.ts', 404, 'too small to hold 50000000 entries'),
        ('', 'clipTo/10000/a.mp4', 404, 'too small to hold 50000000 entries'),
    ]),
    ('STSZ_FRAME_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stsz') + 12, struct.pack('>L', 100000000)), [
        ('/hls', 'seg-1.ts', 404, 'frame size 100000000 too big'),
    ]),
    # stss
    ('STSS_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stss', 4), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stss_atom: atom size 4 too small'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_parser_validate_stss_atom: atom size 4 too small'),
    ]),
    ('STSS_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stss') + 4, struct.pack('>L', 1000000000)), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_validate_stss_atom: number of entries 1000000000 too big'),
        ('', 'clipTo/10000/a.mp4', 404, 'mp4_parser_validate_stss_atom: number of entries 1000000000 too big'),
    ]),
    ('STSS_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.stss') + 4, struct.pack('>L', 100000000)), [
        ('/hls', 'seg-1.ts', 404, 'too small to hold 100000000 entries'),
        ('', 'clipTo/10000/a.mp4', 404, 'too small to hold 100000000 entries'),
    ]),
    # stsd
    ('STSD_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stsd', 4), [
        ('/hls', 'seg-1.ts', 404, 'mp4_parser_parse_stsd_atom: atom size 4 too small'),
    ]),
    ('STSD_NO_ROOM_FOR_ENTRY', lambda: truncateAtom('moov.trak.mdia.minf.stbl.stsd', 12), [
        ('/hls', 'seg-1.ts', 404, 'not enough room for stsd entry'),
    ]),
]

if atomExists('moov.trak.mdia.minf.stbl.ctts'):
    TEST_CASES += [
        # ctts
        ('CTTS_TOO_SMALL', lambda: truncateAtom('moov.trak.mdia.minf.stbl.ctts', 4), 'mp4_parser_validate_ctts_atom: atom size 4 too small', 'seg-1.ts', 404),
        ('CTTS_ENTRIES_TOO_BIG', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.ctts') + 4, struct.pack('>L', 1000000000)), 'mp4_parser_validate_ctts_atom: number of entries 1000000000 too big', 'seg-1.ts', 404),
        ('CTTS_CANT_HOLD', lambda: applyPatch(getAtomDataPos('moov.trak.mdia.minf.stbl.ctts') + 4, struct.pack('>L', 100000000)), 'too small to hold 100000000 entries', 'seg-1.ts', 404),
    ]

def uploadTestEntry(refId, generator):
    print('uploading %s' % refId)
    newEntry = client.media.add(entry=KalturaMediaEntry(name=refId, referenceId=refId, mediaType=KalturaMediaType.VIDEO, conversionProfileId=sourceOnlyConvProfileId))
    uploadToken = client.uploadToken.add(uploadToken=KalturaUploadToken())
    client.media.addContent(entryId=newEntry.id, resource=KalturaUploadedFileTokenResource(token=uploadToken.id))
    client.uploadToken.upload(uploadTokenId=uploadToken.id, fileData=generator())
    return newEntry.id
    

# get all existing entries
referenceIds = ','.join(map(lambda x: REFERENCE_ID_PREFIX + x[0], TEST_CASES))
listResult = client.media.list(filter=KalturaMediaEntryFilter(referenceIdIn=referenceIds), pager=KalturaFilterPager(pageSize=500))
refIdToEntryIdMap = dict(map(lambda x: (x.referenceId, x.id), listResult.objects))

# upload missing entries
entryMap = {}
for refId, generator, tests in TEST_CASES:
    refId = REFERENCE_ID_PREFIX + refId
    if refIdToEntryIdMap.has_key(refId):
        entryId = refIdToEntryIdMap[refId]
    else:
        entryId = uploadTestEntry(refId, generator)
    entryMap[entryId] = (refId, tests)

# print the test uris
print('result:')
flavors = client.flavorAsset.list(filter=KalturaFlavorAssetFilter(flavorParamsIdEqual=0, entryIdIn=','.join(entryMap.keys())),
                                  pager=KalturaFilterPager(pageSize=500)).objects
for flavor in flavors:
    refId, tests = entryMap[flavor.entryId]
    for uriPrefix, fileName, statusCode, message in tests:
        print('%s %s/p/%s/sp/%s00/serveFlavor/entryId/%s/v/%s/flavorId/%s/%s %s %s' % (refId, uriPrefix, PARTNER_ID, PARTNER_ID, flavor.entryId, flavor.version, flavor.id, fileName, statusCode, message))
