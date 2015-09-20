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
		self.index = threadContext
		self.increment, self.stopFile = sharedContext

	def run(self):
		self.writeOutput('Info: started')
		index = self.index
		while not os.path.exists(self.stopFile) and index < len(requests):
			if not self.runTest(requests[index]):
				writeError(requests[index])
			index += self.increment
		self.writeOutput('Info: done')
		self.cleanup()
		
	def writeOutput(self, msg):
		writeOutput(msg, 'TID%s' % self.index)
		
	def cleanup(self):
		pass

class ParentThread(Thread):
	def __init__(self, threadContext, sharedContext):
		Thread.__init__(self)
		self.serverName, self.index, self.startOffset, self.endOffset = threadContext
		self.inputFile, self.threadCount, self.outputFile = sharedContext

	def run(self):		
		splittedOutputFile = os.path.splitext(self.outputFile)
		outputFile = '%s-%s-%s%s' % (splittedOutputFile[0], self.serverName, self.index, splittedOutputFile[1])
		errorFile = '%s-%s-%s.err%s' % (splittedOutputFile[0], self.serverName, self.index, splittedOutputFile[1])
		cmdLine = 'python %s %s %s %s child %s %s' % (os.path.realpath(sys.argv[0]), self.inputFile, self.threadCount, self.outputFile, self.startOffset, self.endOffset)
				
		if self.serverName != 'localhost':
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
	global requests
	
	# parse the command line
	if len(sys.argv) < 4:
		writeOutput('Usage:\n\t%s <input file> <thread count> <output file> [<server1> <server2> .. ]' % os.path.basename(__file__))
		sys.exit(1)
	
	(_, inputFile, threadCount, outputFile) = sys.argv[:4]
	threadCount = int(threadCount)	
	
	serverList = []
	startOffset = 0
	endOffset = None
	if len(sys.argv) == 7 and sys.argv[4] == 'child':
		startOffset = int(sys.argv[5])
		endOffset = int(sys.argv[6])
	else:
		serverList = sys.argv[4:]
		
	# read all the request
	writeOutput('Info: reading input file %s' % inputFile)
	requests = []
	for inputLine in file(inputFile):
		inputLine = inputLine.strip()
		if len(inputLine) == 0 or inputLine[0] == '#':
			continue
		requests.append(inputLine)
	if endOffset == None:
		endOffset = len(requests)
	requests = requests[startOffset:endOffset]
		
	# the following line makes use of worker processes on each server instead of threads
	if len(serverList) == 0 and threadCount > 1:
		serverList = ['localhost'] * threadCount
		threadCount = 1
	
	# parent
	if len(serverList) > 0:
		requestsPerServer = (len(requests) + len(serverList) - 1) / len(serverList)
		threadContexts = []
		for curIndex in xrange(len(serverList)):
			threadContexts.append(
				(serverList[curIndex], 
				curIndex, 
				startOffset + curIndex * requestsPerServer, 
				min(startOffset + (curIndex + 1) * requestsPerServer, endOffset)))
		sharedContext = (inputFile, threadCount, outputFile)
		runWorkerThreads(ParentThread, threadContexts, sharedContext)
		sys.exit(0)

	# child
	sys.stderr.write('touch %s to stop\n' % stopFile)
	try:
		os.remove(stopFile)
	except OSError:
		pass
		
	writeOutput('Info: finished reading %s lines' % len(requests))

	# run the worker threads
	runWorkerThreads(testThread, range(threadCount), (threadCount, stopFile))
