from threading import Thread
from threading import Lock
import random
import time
import sys
import os

outputLock = Lock()

def writeOutput(msg, index = ''):
	outputLock.acquire()
	sys.stdout.write('[%s] %s %s\n' % (index, time.strftime('%Y-%m-%d %H:%M:%S'), msg))
	sys.stdout.flush()
	outputLock.release()

def writeError(msg):
	outputLock.acquire()
	sys.stderr.write('%s\n' % msg)
	sys.stderr.flush()
	outputLock.release()

class TestThreadBase(Thread):
	def __init__(self, threadContext, sharedContext):
		Thread.__init__(self)
		self.curIndex = threadContext
		self.totalCount, self.stopFile, self.inputFile = sharedContext

	def run(self):
		self.writeOutput('Info: started')
		index = 0
		for inputLine in open(self.inputFile):
			inputLine = inputLine.strip()
			if len(inputLine) == 0 or inputLine[0] == '#':
				continue
			if index % self.totalCount != self.curIndex:
				index += 1
				continue
			if not self.runTest(inputLine):
				writeError(inputLine)
			index += 1
		self.writeOutput('Info: done')
		self.cleanup()
		
	def writeOutput(self, msg):
		writeOutput(msg, 'TID%s' % self.curIndex)
		
	def cleanup(self):
		pass

class ParentThread(Thread):
	def __init__(self, threadContext, sharedContext):
		Thread.__init__(self)
		self.serverName, self.curIndex = threadContext
		self.inputFile, self.threadCount, self.totalCount, self.outputFile = sharedContext

	def run(self):		
		splittedOutputFile = os.path.splitext(self.outputFile)
		outputFile = '%s-%s-%s%s' % (splittedOutputFile[0], self.serverName, self.curIndex, splittedOutputFile[1])
		errorFile = '%s-%s-%s.err%s' % (splittedOutputFile[0], self.serverName, self.curIndex, splittedOutputFile[1])
		cmdLine = 'python2 %s %s %s %s child %s %s' % (os.path.realpath(sys.argv[0]), self.inputFile, self.threadCount, self.outputFile, self.curIndex, self.totalCount)
				
		if self.serverName != '-':
			cmdLine = 'ssh %s %s < /dev/null' % (self.serverName, cmdLine)
			
		cmdLine += ' > %s 2> %s' % (outputFile, errorFile)
		
		self.writeOutput('Info: running %s' % cmdLine)
		os.system(cmdLine)		
		self.writeOutput('Info: done')
		
	
	def writeOutput(self, msg):
		writeOutput(msg, self.serverName)
			

def runWorkerThreads(workerThread, threadContexts, sharedContext):
	# create the threads
	threads = []
	for threadContext in threadContexts:
		curThread = workerThread(threadContext, sharedContext)
		threads.append(curThread)

	# start the threads
	for curThread in threads:
		curThread.start()

	writeOutput('Info: finished launching %s threads' % len(threads))

	# wait on the threads
	threadAlive = True
	while threadAlive:
		threadAlive = False
		for thread in threads:
			if thread.isAlive():
				threadAlive = True
				break
		time.sleep(5)

	writeOutput('Info: done !')

		
def main(testThread, stopFile):
	# parse the command line
	if len(sys.argv) < 4:
		writeOutput('Usage:\n\tpython %s <input file> <thread count> <output file> [<server1> <server2> .. ]' % os.path.basename(__file__))
		return 1
	
	(_, inputFile, threadCount, outputFile) = sys.argv[:4]
	threadCount = int(threadCount)	
	
	if len(sys.argv) == 7 and sys.argv[4] == 'child':
		curIndex = int(sys.argv[5])
		totalCount = int(sys.argv[6])
		serverList = []
	else:
		curIndex = 0
		totalCount = 1
		serverList = sys.argv[4:]
		
	# the following line makes use of worker processes on each server instead of threads
	if len(serverList) == 0 and threadCount > 1:
		serverList = ['-'] * threadCount
		threadCount = 1
	
	# parent
	if len(serverList) > 0:
		indexes = [curIndex + x * totalCount for x in range(len(serverList))]
		threadContexts = list(zip(serverList, indexes))
		sharedContext = (inputFile, threadCount, len(serverList) * totalCount, outputFile)
		runWorkerThreads(ParentThread, threadContexts, sharedContext)
		return 0

	# child
	sys.stderr.write('touch %s to stop\n' % stopFile)
	try:
		os.remove(stopFile)
	except OSError:
		pass
		
	writeOutput('Info: started')

	# run the worker threads
	threadContexts = [curIndex + x * totalCount for x in range(threadCount)]
	runWorkerThreads(testThread, threadContexts, (threadCount * totalCount, stopFile, inputFile))
