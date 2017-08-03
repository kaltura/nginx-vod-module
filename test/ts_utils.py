from Crypto.Cipher import AES

def decryptTsSegment(fileData, aesKey, aesIv):
	cipher = AES.new(aesKey, AES.MODE_CBC, aesIv)
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
