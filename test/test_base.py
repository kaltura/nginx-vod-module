from __future__ import print_function
import urllib2
import os

RUN_ONLY_PATH = ''	   # use for running only some tests, e.g. MainTestSuite.RemoteTestSuite.MemoryUpstreamTestSuite
NGINX_LOG_PATH = '/var/log/nginx/error.log'

### Assertions
def assertEquals(v1, v2):
	if v1 == v2:
		return
	print('Assertion failed - %s != %s' % (v1, v2))
	assert(False)

def assertIn(needle, haystack):
	if needle in haystack:
		return
	print('Assertion failed - %s in %s' % (needle, haystack))
	assert(False)

def assertNotIn(needle, haystack):
	if not needle in haystack:
		return
	print('Assertion failed - %s not in %s' % (needle, haystack))
	assert(False)

def assertInIgnoreCase(needle, haystack):
	assertIn(needle.lower(), haystack.lower())

def assertNotInIgnoreCase(needle, haystack):
	assertNotIn(needle.lower(), haystack.lower())

def assertStartsWith(buffer, prefix):
	if buffer.startswith(prefix):
		return
	print('Assertion failed - %s.startswith(%s)' % (buffer, prefix))
	assert(False)

def assertEndsWith(buffer, postfix):
	if buffer.endswith(postfix):
		return
	print('Assertion failed - %s.endswith(%s)' % (buffer, postfix))
	assert(False)

def assertRequestFails(url, statusCode, expectedBody = None, headers = {}, postData = None):
	request = urllib2.Request(url, headers=headers)
	try:
		response = urllib2.urlopen(request, data=postData)
		assert(False)
	except urllib2.HTTPError as e:
		if type(statusCode) == list:
			assertIn(e.getcode(), statusCode)
		else:
			assertEquals(e.getcode(), statusCode)
		if expectedBody != None:
			assertEquals(expectedBody, e.read())

### Cleanup stack - enables automatic cleanup after a test is run
class CleanupStack:
	def __init__(self):
		self.items = []

	def push(self, callback):
		self.items.append(callback)

	def resetAndDestroy(self):
		for i in range(len(self.items), 0 , -1):
			self.items[i - 1]()
		self.items = []

cleanupStack = CleanupStack()

### Log tracker - used to verify certain lines appear in nginx log
class LogTracker:
	def __init__(self):
		self.initialSize = os.path.getsize(NGINX_LOG_PATH)

	def contains(self, logLine):
		f = open(NGINX_LOG_PATH, 'rb')
		f.seek(self.initialSize, os.SEEK_SET)
		buffer = f.read()
		f.close()
		if type(logLine) == list:
			found = False
			for curLine in logLine:
				if curLine in buffer:
					found = True
					break
			return found
		else:
			return logLine in buffer

	def assertContains(self, logLine):
		assert(self.contains(logLine))

### Test suite base class
class TestSuite(object):
	level = 0
	curPath = ''

	def __init__(self):
		self.prepareTest = None

	@staticmethod
	def getIndent():
		return '  ' * TestSuite.level

	def run(self):
		print('%sRunning suite %s' % (TestSuite.getIndent(), self.__class__.__name__))
		TestSuite.level += 1
		TestSuite.curPath += self.__class__.__name__ + '.'
		self.runChildSuites()
		for curFunc in dir(self):
			if not curFunc.startswith('test'):
				continue
			curTestPath = TestSuite.curPath + curFunc
			if len(RUN_ONLY_PATH) != 0 and not curTestPath.startswith(RUN_ONLY_PATH):
				continue
			if self.prepareTest != None:
				self.prepareTest()
			self.logTracker = LogTracker()
			print('%sRunning %s' % (TestSuite.getIndent(), curFunc))
			getattr(self, curFunc)()
			cleanupStack.resetAndDestroy()
		TestSuite.curPath = TestSuite.curPath[:-(len(self.__class__.__name__) + 1)]
		TestSuite.level -= 1

	def runChildSuites(self):
		pass
