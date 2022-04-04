from __future__ import print_function
import manifest_utils
import http_utils
import sys
import os

if len(sys.argv) < 3:
	print('Usage:\n\tpython %s <manifest url> <output path>' % os.path.basename(__file__))
	sys.exit(1)

_, manifestUrl, outputPath = sys.argv

code, headers, body = http_utils.getUrl(manifestUrl, {})
mimeType = headers['content-type'][0]
urls = manifest_utils.getManifestUrls(manifestUrl, body, mimeType, {})

for curUrl in [manifestUrl] + urls:
	fileName = os.path.join(outputPath, os.path.split(curUrl)[1].split('?')[0])
	if os.path.exists(fileName):
		print('Error: %s already exists' % fileName)
		break
	http_utils.downloadUrl(curUrl, fileName)
