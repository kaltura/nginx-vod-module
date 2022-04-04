from __future__ import print_function
import commands
import sys
import re
import os

IGNORE_LIST = set([
	'vod_array_init_impl', 
	'vod_array_destroy',
	'hls_encryption_methods',
	'dash_manifest_formats',
	])

if len(sys.argv) < 2:
	print('Usage:\n\tpython %s <source root> [<object root> [<object use only last folder>]]' % os.path.basename(__file__))
	sys.exit(1)

sourceRoot = sys.argv[1]
if len(sys.argv) > 2:
	objectRoot = sys.argv[2]
else:
	objectRoot = ''

if len(sys.argv) > 3:
	useOnlyLastFolder = sys.argv[3].lower() == 'yes'
else:
	useOnlyLastFolder = True

funcNames = []
wordsByFile = {}
wordsByHeaderFiles = set([])
for root, dirs, files in os.walk(sourceRoot):
	for name in files:
		fileExt = os.path.splitext(name)[1]
		fullPath = os.path.join(root, name)
		if fileExt == '.h':
			fileData = open(fullPath, 'rb').read()
			wordsByHeaderFiles.update(set(re.findall(r'\b(\w+)\b', fileData)))
			continue
		if fileExt != '.c':
			continue
		fileData = open(fullPath, 'rb').read()
		wordsByFile[fullPath] = set(re.findall(r'\b(\w+)\b', fileData))
		if objectRoot == '':
			# detect exports from code
			for curLine in fileData.split('\n'):
				if re.match('^\w+\(', curLine) and not 'static' in prevLine:
					funcName = curLine.split('(')[0]
					funcNames.append(funcName)
				prevLine = curLine
		else:
			# get exports from object file
			if useOnlyLastFolder:
				objectFile = os.path.join(os.path.split(os.path.split(fullPath)[0])[1], os.path.split(fullPath)[1])
			else:
				objectFile = fullPath[len(sourceRoot):]
			if objectFile.startswith('/'):
				objectFile = objectFile[1:]
			objectFile = os.path.join(objectRoot, os.path.splitext(objectFile)[0] + '.o')
			if not os.path.exists(objectFile):
				continue
			for curLine in commands.getoutput("readelf -Ws %s | grep -vw UND | grep -w GLOBAL | awk '{print $NF}'" % objectFile).split('\n'):
				funcName = curLine.strip()
				if len(funcName) > 0:
					funcNames.append(funcName)

print('Found %s exports' % len(funcNames))

for funcName in funcNames:
	if funcName in IGNORE_LIST:
		continue
	fileCount = 0
	for words in wordsByFile.values():
		if funcName in words:
			fileCount += 1
	if fileCount < 2:
		print(funcName, funcName in wordsByHeaderFiles)
