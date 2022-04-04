from __future__ import print_function
from mp4_utils import *
import sys

MAX_READ_SIZE = 64 * 1024 * 1024

inputFileName = sys.argv[1]
inputData = file(inputFileName, 'rb').read(MAX_READ_SIZE)

def printAtoms(inputData, parsedAtoms, indent):
	for atomName, atoms in parsedAtoms.items():
		print(indent + atomName)
		for startPos, atomHeaderSize, endPos, childAtoms in atoms:
			if childAtoms == {}:
				if endPos - startPos < 256:
					print(inputData[(startPos + atomHeaderSize):endPos].encode('hex'))
				else:
					print('atom size %s' % (endPos - startPos))
			printAtoms(inputData, childAtoms, indent + '  ')

parsedAtoms = parseAtoms(inputData, 0, len(inputData))
printAtoms(inputData, parsedAtoms, '')
