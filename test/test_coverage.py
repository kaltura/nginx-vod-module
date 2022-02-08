from __future__ import print_function
import sys
import re
import os

def countArgs(argsSpec):
    result = 0
    parentCount = 0
    for curCh in argsSpec:
        if curCh == '(':
            parentCount += 1
        elif curCh == ')':
            parentCount -= 1
        elif curCh == ',' and parentCount == 0:
            result += 1
    return result

def validateLogParams(fileName, fileData):
    logBlock = None
    for curLine in fileData.split('\n'):
        if re.match('^\s*(?:ngx|vod)_log_(?:debug|error)', curLine):
            logBlock = ''
        if logBlock == None:
            continue
        logBlock += curLine.strip()
        if logBlock.count('(') > logBlock.count(')'):
            continue
        logMessage = logBlock[logBlock.find('"'):logBlock.rfind('"')]
        args = logBlock[logBlock.rfind('"'):]
        if countArgs(args) != logMessage.count('%') + logMessage.count('%*'):
            print(logBlock)
        logBlock = None

if len(sys.argv) < 2:
    print('Usage:\n\tpython %s <nginx-vod code root> [<error log file>]' % os.path.basename(__file__))
    sys.exit(1)

nginxVodRoot = sys.argv[1]
errorLog = None
if len(sys.argv) > 2:
    errorLog = sys.argv[2]

logMessages = []
logPrefixes = set([])
for root, dirs, files in os.walk(nginxVodRoot):
    for name in files:
        if os.path.splitext(name)[1] != '.c':
            continue
        fileData = open(os.path.join(root, name), 'rb').read()
        validateLogParams(name, fileData)
        for curLog in re.findall('(?:ngx|vod)_log_(?:debug|error)[^\(]*\([^\)]+\)', fileData):
            logMessage = curLog.split(',')[3].strip()
            if logMessage.endswith('")'):
                logMessage = logMessage[:-1]
            if logMessage.endswith('"'):
                logMessage = logMessage[:-1]
            if logMessage.startswith('"'):
                logMessage = logMessage[1:]

            if logMessage == 'const char *fmt':
                continue

            # add the log message
            logPrefix = logMessage.split('%')[0]
            logMessages.append((logPrefix, logMessage))
            logPrefixes.add(logPrefix)

            # validate the function name in the log line matches the actual function
            linePos = fileData.find(curLog)
            functionNames = re.findall('^[a-z0-9_]+\(', fileData[:linePos], re.MULTILINE)
            realFunction = functionNames[-1]
            if realFunction.endswith('('):
                realFunction = realFunction[:-1]
            logFunction = logMessage.split(':')[0]
            if logFunction.startswith('"'):
                logFunction = logFunction[1:]
            if realFunction != logFunction:
                print('log function mismatch %s' % logMessage)
            

if errorLog == None:
    sys.exit(0)
            
errorLogData = open(errorLog, 'rb').read()
foundPrefixes = {}
for logPrefix in logPrefixes:
    logPos = errorLogData.find(logPrefix)
    if logPos > 0:
        foundPrefixes[logPrefix] = errorLogData[(errorLogData.rfind('\n', 0, logPos) + 1):errorLogData.find('\n', logPos)]

print('not covered:')
for logPrefix, logMessage in logMessages:
    if not foundPrefixes.has_key(logPrefix):
        print('\t' + logMessage)

print('covered:')
for curLine in foundPrefixes.values():
    print('\t' + curLine[:140])
