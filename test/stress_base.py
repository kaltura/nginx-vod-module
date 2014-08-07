from threading import Thread
from threading import Lock
import random
import time
import sys
import os

outputLock = Lock()

def writeOutput(msg, index = -1):
	outputLock.acquire()
	sys.stdout.write('[TID%s] %s %s\n' % (index, time.strftime('%Y-%m-%d %H:%M:%S'), msg))
	sys.stdout.flush()
	outputLock.release()

def writeError(msg):
	outputLock.acquire()
	sys.stderr.write('%s\n' % msg)
	sys.stderr.flush()
	outputLock.release()

class TestThreadBase(Thread):
	def __init__(self, index, increment, stopFile):
		Thread.__init__(self)
		self.index = index
		self.increment = increment
		self.stopFile = stopFile

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
		writeOutput(msg, self.index)
		
	def cleanup(self):
		pass

def main(testThread, stopFile):
	global requests
	
	if len(sys.argv) < 3:
		writeOutput('Usage:\n\t%s <input file> <thread count>' % os.path.basename(__file__))
		sys.exit(1)
	
	(_, inputFile, threadCount) = sys.argv[:3]
	threadCount = int(threadCount)
	
	sys.stderr.write('touch %s to stop\n' % stopFile)
	try:
		os.remove(stopFile)
	except OSError:
		pass
	
	# read all the request
	writeOutput('Info: reading input file %s' % inputFile)
	requests = []
	for inputLine in file(inputFile):
		inputLine = inputLine.strip()
		if len(inputLine) == 0 or inputLine[0] == '#':
			continue
		requests.append(inputLine)
	random.shuffle(requests)
	writeOutput('Info: finished reading %s lines' % len(requests))
	
	# create the threads
	threads = []
	for curThreadIdx in range(threadCount):
		curThread = testThread(curThreadIdx, threadCount, stopFile)
		threads.append(curThread)

	# start the threads
	for curThread in threads:
		curThread.start()

	writeOutput('Info: finished launching %s threads' % threadCount)

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
