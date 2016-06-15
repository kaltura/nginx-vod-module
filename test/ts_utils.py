from Crypto.Cipher import AES

def decryptTsSegment(self, fileData, aesKey, segmentIndex):
	cipher = AES.new(aesKey, AES.MODE_CBC, '\0' * 12 + struct.pack('>L', segmentIndex))
	try:
		decryptedTS = cipher.decrypt(fileData)
	except ValueError:
		return None, 'Error: cipher.decrypt failed'
	padLength = ord(decryptedTS[-1])
	if padLength > 16:
		return None, 'Error: invalid pad length %s' % padLength
	if decryptedTS[-padLength:] != chr(padLength) * padLength:
		return None, 'Error: invalid padding'
	return decryptedTS[:-padLength], ''
