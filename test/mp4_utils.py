import struct
import re

def isContainerAtom(inputData, startOffset, endOffset):
    if endOffset - startOffset < 8:
        return False
    if not re.match('^[0-9a-zA-Z]+$', inputData[(startOffset + 4):(startOffset + 8)]):
        return False
    atomSize = struct.unpack('>L', inputData[startOffset:(startOffset + 4)])[0]
    if atomSize > endOffset - startOffset:
        return False
    return True

def parseAtoms(inputData, startOffset, endOffset):
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
            childAtoms = parseAtoms(inputData, curPos + atomHeaderSize, min(curPos + atomSize, endOffset))

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

def getAtom(inputAtoms, path):
    return getAtomInternal(path.split('.'), inputAtoms)

def getAtomData(inputData, inputAtoms, path):
	atom = getAtom(inputAtoms, path)
	if atom == None:
		return False
	return inputData[(atom[0] + atom[1]):atom[2]]
