from __future__ import print_function
from ts_utils import decryptTsSegment
import struct
import sys
import os

if len(sys.argv) < 5:
	print('Usage:\n\tpython %s <input file> <output file> <aes key hex> <segment index (one based) or 0xIV>' % os.path.basename(__file__))
	sys.exit(1)

INPUT_FILE = sys.argv[1]
OUTPUT_FILE = sys.argv[2]
ENC_KEY = sys.argv[3]
ENC_IV = sys.argv[4]

# key
if ENC_KEY.startswith('0x'):
	ENC_KEY = ENC_KEY[2:]
ENC_KEY = ENC_KEY.decode('hex')

# iv
if ENC_IV.startswith('0x'):
	ENC_IV = ENC_IV[2:]
	ENC_IV = ENC_IV.decode('hex')
else:
	segmentIndex = int(ENC_IV)
	ENC_IV = '\0' * 12 + struct.pack('>L', segmentIndex)

encryptedData = open(INPUT_FILE, 'rb').read()
decryptedData, error = decryptTsSegment(encryptedData, ENC_KEY, ENC_IV)
if decryptedData == None:
	print(error)
	sys.exit(1)
open(OUTPUT_FILE, 'wb').write(decryptedData)
