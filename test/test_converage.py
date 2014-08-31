import sys
import re
import os

_, nginxVodRoot, errorLog = sys.argv

logMessages = []
logPrefixes = set([])
for root, dirs, files in os.walk(nginxVodRoot):
    for name in files:
        if os.path.splitext(name)[1] != '.c':
            continue
        for curLog in re.findall('(?:ngx|vod)_log_(?:debug|error)[^\(]*\([^\)]+\)', file(os.path.join(root, name), 'rb').read()):
            logMessage = curLog.split(',')[3].strip()
            if logMessage.endswith('")'):
                logMessage = logMessage[:-1]
            if logMessage.endswith('"'):
                logMessage = logMessage[:-1]
            if logMessage.startswith('"'):
                logMessage = logMessage[1:]

            if logMessage == 'const char *fmt':
                continue

            logPrefix = logMessage.split('%')[0]
            logMessages.append((logPrefix, logMessage))
            logPrefixes.add(logPrefix)

errorLogData = file(errorLog, 'rb').read()
foundPrefixes = {}
for logPrefix in logPrefixes:
    logPos = errorLogData.find(logPrefix)
    if logPos > 0:
        foundPrefixes[logPrefix] = errorLogData[(errorLogData.rfind('\n', 0, logPos) + 1):errorLogData.find('\n', logPos)]

print 'not covered:'
for logPrefix, logMessage in logMessages:
    if not foundPrefixes.has_key(logPrefix):
        print '\t' + logMessage

print 'convered:'
for curLine in foundPrefixes.values():
    print '\t' + curLine[:140]
