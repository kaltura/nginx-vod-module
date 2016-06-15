from ts_utils import decryptTsSegment
import sys

if len(sys.argv) < 5:
	print 'Usage:\n\tphp encryptUrl.php <input file> <output file> <aes key hex> <segment index (one based)>'
	sys.exit(1)

INPUT_FILE = sys.argv[1]
OUTPUT_FILE = sys.argv[2]
ENC_KEY = sys.argv[3].decode('hex')
SEGMENT_INDEX = int(sys.argv[4])

encryptedData = file(INPUT_FILE, 'rb').read()
decryptedData, error = decryptTsSegment(encryptedData, ENC_KEY, SEGMENT_INDEX)
if decryptedData == None:
	print error
	sys.exit(1)
file(OUTPUT_FILE, 'wb').write(decryptedData)
