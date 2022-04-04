from __future__ import print_function
from test_base import *
from threading import Thread
import commands
import urllib2
import random
import time
import sys
import pwd
import grp
import os

'''
config:
-------
use the associated test_open_file_cache.conf, also recommended to run some tests (no need for the whole matrix) 
with each of the following combinations:
* comment out open_file_cache (no file caching)
* change open_file_cache_min_uses to 2
* change open_file_cache to max=10 inactive=2;
* add open_file_cache_events = on (mac / freebsd only)

note:
-----
if the tests are interrupted, release the nginx server with:
for i in `seq 1 12`; do touch /tmp/open-unblock-$i.flag; done

required code changes:
----------------------
* in ngx_async_open_file_cache.c, replace ngx_thread_open_handler with:

static void
ngx_thread_open_handler(void *data, ngx_log_t *log)
{
	ngx_async_open_file_ctx_t* ctx = data;
	
	int block_type = ctx->of->size;
	char file_name[256];
	int fail = (ctx->of->failed != NULL);

	ctx->of->size = 0;
	ctx->of->failed = NULL;

	if (block_type > 0)
	{
		sprintf(file_name, "/tmp/open-unblock-%d.flag", block_type);
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0, "waiting for %d", block_type);
		while (access(file_name, F_OK) == -1)
		{
			ngx_msleep(100);
		}
	}

	if (!fail)
	{
		ctx->err = ngx_open_and_stat_file(&ctx->name, ctx->of, log);
	}
	else
	{
		ctx->of->fd = NGX_INVALID_FILE;
		ctx->err = NGX_ERROR;
	}

	if (block_type < 0)
	{
		sprintf(file_name, "/tmp/open-unblock-%d.flag", -block_type);
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0, "waiting for %d", -block_type);
		while (access(file_name, F_OK) == -1)
		{
			ngx_msleep(100);
		}
	}
}

* in ngx_file_reader.c, before the call to ngx_async_open_cached_file add:

	ngx_str_t value;
	
	if (ngx_http_arg(r, (u_char *) "block", 5, &value) == NGX_OK) 
	{
		if (value.data[0] == '-')
		{
			open_context->of.size = -ngx_atoi(value.data + 1, value.len - 1);
		}
		else
		{
			open_context->of.size = ngx_atoi(value.data, value.len);
		}
	}

	if (ngx_http_arg(r, (u_char *) "fail", 4, &value) == NGX_OK) 
	{
		open_context->of.failed = "fail";
	}


debugging handle leak:
----------------------
* in ngx_async_open_file_cache.c, add:

int 
ngx_open_file_cache_get_handle_count(ngx_open_file_cache_t *cache)
{
	ngx_cached_open_file_t  *file;
	ngx_queue_t          *q;
	int result = 0;

	for (q = ngx_queue_head(&cache->expire_queue);
		q != ngx_queue_sentinel(&cache->expire_queue);
		q = ngx_queue_next(q))
	{
		file = ngx_queue_data(q, ngx_cached_open_file_t, queue);

		if (file->fd != NGX_INVALID_FILE)
		{
			result++;
		}
	}

	return result;
}

* in ngx_async_open_file_cache.h, add:

int ngx_open_file_cache_get_handle_count(ngx_open_file_cache_t *cache);

* in after the call to ngx_http_discard_request_body, add:

	if (ngx_http_arg(r, (u_char *) "fdcount", 7, &response) == NGX_OK)
	{
		ngx_http_core_loc_conf_t *clcf;
		char count[100];

		clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

		sprintf(count, "%d", ngx_open_file_cache_get_handle_count(clcf->open_file_cache));
		
		response.data = count;
		response.len = strlen(count);

		rc = ngx_http_vod_send_response(r, &response, "text/html", sizeof("text/html") - 1);
		goto done;
	}

'''

# parameters
DEBUG_HANDLE_LEAK = False
TESTS_TO_RUN = []		# populate with specific test indexes to run only a subset of tests

OPEN_UNBLOCK_FILE = "/tmp/open-unblock-%d.flag"
FLAG_OWNER = "nobody"
FLAG_GROUP = "nogroup"

CACHE_VALID_TIME = 1
CACHE_INACTIVE_TIME = 2

BASE_URL = 'http://localhost:8001/local/content/'
BASE_PATH = '/usr/local/nginx/html/local/content/'

# derived parameters
if DEBUG_HANDLE_LEAK:
	THREAD_COUNT = 1
else:
	THREAD_COUNT = 12

FLAG_UID = pwd.getpwnam(FLAG_OWNER).pw_uid
FLAG_GID = grp.getgrnam(FLAG_GROUP).gr_gid

def writeFile(path, data):
	f = file(path, 'wb')
	f.write(data)
	f.close()

def touchFile(path):
	open(path, 'a').close()
	try:
		os.chown(path, FLAG_UID, FLAG_GID)
	except:
		pass

class AsyncHttpRequest(Thread):
	def __init__(self, url):
		Thread.__init__(self)
		self.url = url
		self.response = None
		self.status = None
		self.start()
		
	def run(self):
		try:
			self.response = urllib2.urlopen(urllib2.Request(self.url)).read()
		except urllib2.HTTPError as e:
			self.status = e.getcode()

INITIAL_CONTENT = 'hello'
FINAL_CONTENT = 'worldhello'
MIXED_CONTENT = 'world'

def changeFileToDir(fileName):
	os.remove(BASE_PATH + fileName)
	os.mkdir(BASE_PATH + fileName)

def changeDirToFile(fileName):
	os.rmdir(BASE_PATH + fileName)
	writeFile(BASE_PATH + fileName, FINAL_CONTENT)

class TestThread(Thread):	
	def __init__(self, id, count, tests):
		Thread.__init__(self)
		self.id = id + 1
		self.count = count
		self.tests = tests
		self.completeCount = 0

	def issueRequest(self, url):
		try:
			return (urllib2.urlopen(urllib2.Request(url)).read(), None)
		except urllib2.HTTPError as e:
			return (None, e.getcode())

	def doValidate(self, response, elapsedTime, validateInitial, validateMidway, validateFinal, hasPostBlockType = False):
		if validateFinal(*response):
			return True
		if elapsedTime < CACHE_VALID_TIME or hasPostBlockType:
			if validateMidway != None:
				return validateMidway(*response)
			return validateInitial(*response)
		return False

	def getUniqueFileName(self):
		return 'test-%s-%s' % (self.id, random.randint(0, 0x10000))

	def issueAndValidate(self, id, test, testType, fileName, validate):
		if testType == 'no':
			return
		url = BASE_URL + fileName
		if testType == 'fail':
			url += '?fail=1'
		response, status = self.issueRequest(url)
		if testType == 'fail' and status == 500:
			return
		if not validate(response, status):
			print('%s - %s %s %s' % (id, test, response, status))
		
	def runSyncTest(self, test):
		if DEBUG_HANDLE_LEAK:
			print('Running %s' % test['index'])
		#print 'Running %s' % test
		
		testName, prepare, validateInitial, change, validateMidway, validateFinal, cleanup = test['test']

		# clean up any previous unblock file
		try:
			os.remove(OPEN_UNBLOCK_FILE % self.id)
		except OSError:
			pass
		
		# prepare initial state
		fileName = self.getUniqueFileName()
		if prepare != None:
			prepare(fileName)
		
		# feed into cache
		if test['initialCacheLoad']:
			response = self.issueRequest(BASE_URL + fileName)
			if not validateInitial(*response):
				print('Error1 - %s %s' % (test, response))
			# wait until it will expire
			time.sleep(CACHE_INACTIVE_TIME)
			
		# enable block
		url = BASE_URL + fileName
		if test['blockType'] == 'pre':
			url += '?block=%s' % self.id
		elif test['blockType'] == 'post':
			url += '?block=-%s' % self.id
		elif test['blockType'] == 'fail':
			url += '?block=%s&fail=1' % self.id
		
		lt = LogTracker()
		# start the async request
		ar = AsyncHttpRequest(url)

		# wait until the thread gets blocked or the request completes
		while True:
			if lt.contains('waiting for %s\n' % self.id) or not ar.isAlive():
				break
			time.sleep(.01)

		# check whether the request was served from cache
		if not ar.isAlive():
			ar.join()
			response = (ar.response, ar.status)
			if not validateInitial(*response):
				print('Error1.5 - %s %s' % (test, response))
			if change != None:
				change(fileName)
			if cleanup != None:
				cleanup(fileName)
			return

		# second request
		ar2 = None
		if test['secondBlockType'] != 'none':
			url = BASE_URL + fileName
			if test['secondBlockType'] == 'pre':
				url += '?block=%s' % self.id
			elif test['secondBlockType'] == 'post':
				url += '?block=-%s' % self.id
			if test['secondBlockType'] == 'fail':
				url += '?block=%s&fail=1' % self.id
			
			# start the async request
			lt = LogTracker()
			ar2 = AsyncHttpRequest(url)
			
			# wait until the thread gets blocked or the request completes
			while True:
				if lt.contains('waiting for %s\n' % self.id) or not ar2.isAlive():
					break
				time.sleep(.01)
			
			if not ar2.isAlive():
				print('Unexpected, second request not locked, %s %s %s %s' % (ar.status, ar.response, ar2.status, ar2.response))
		
		# sleep before change
		self.issueAndValidate('Error2', test, test['requestBeforeSleep1'], fileName, validateInitial)
			
		time.sleep(test['sleepTime1'])

		self.issueAndValidate('Error3', test, test['requestAfterSleep1'], fileName, validateInitial)
		
		# make the change
		if change != None:
			change(fileName)
		changeTime = time.time()

		# sleep after change
		self.issueAndValidate('Error4', test, test['requestBeforeSleep2'], fileName, 
			lambda response, status: self.doValidate((response, status), time.time() - changeTime, validateInitial, validateMidway, validateFinal))

		time.sleep(test['sleepTime2'])

		self.issueAndValidate('Error5', test, test['requestAfterSleep2'], fileName, 
			lambda response, status: self.doValidate((response, status), time.time() - changeTime, validateInitial, validateMidway, validateFinal))

		# unblock
		touchFile(OPEN_UNBLOCK_FILE % self.id)

		# final state
		ar.join()
		response = (ar.response, ar.status)
		if not (ar.status == 500 and test['blockType'] == 'fail'):
			if not self.doValidate(response, time.time() - changeTime, validateInitial, validateMidway, validateFinal, 'post' in [test['blockType'], test['secondBlockType']]):
				print('Error6 - %s %s' % (test, response))

		if ar2 != None:	
			# final state
			ar2.join()
			response = (ar2.response, ar2.status)
			if not (ar2.status == 500 and test['secondBlockType'] == 'fail'):
				if not self.doValidate(response, time.time() - changeTime, validateInitial, validateMidway, validateFinal, 'post' in [test['blockType'], test['secondBlockType']]):
					print('Error7 - %s %s' % (test, response))
			
		# cleanup
		if cleanup != None:
			cleanup(fileName)

	def getUnrefHandles(self):
		if not DEBUG_HANDLE_LEAK:
			return 0
		cacheFdCount = urllib2.urlopen(BASE_URL + 'a?fdcount=1').read()
		totalFdCount = commands.getoutput('''ls /proc/`ps aux | grep sbin/ngin[x] | awk '{print $2}'`/fd | wc -l''')
		return int(totalFdCount) - int(cacheFdCount)
			
	def run(self):
		curPos = self.id - 1
		while curPos < len(self.tests):
			initialHandles = self.getUnrefHandles()
			
			self.runSyncTest(self.tests[curPos])
			
			currentHandles = self.getUnrefHandles()
			if initialHandles != currentHandles:
				print('handle leak %s != %s in test %s' % (initialHandles, currentHandles, self.tests[curPos]['index']))
			
			curPos += self.count
			self.completeCount += 1

def generateAllOptions(testMatrix):
	curState = {}
	for curKey in testMatrix:
		curState[curKey] = 0
	
	result = []
	while True:
		# append the current combination
		curComb = {}
		for curKey in testMatrix:
			curComb[curKey] = testMatrix[curKey][curState[curKey]]
		curComb['index'] = len(result)
		result.append(curComb)
		
		# increment the state
		isDone = True
		for curKey in testMatrix:
			curState[curKey] += 1
			if curState[curKey] < len(testMatrix[curKey]):
				isDone = False
				break
			curState[curKey] = 0
			
		if isDone:
			break
			
	return result

TEST_SCENARIOS = [
	(
		'not exist',														# name
		None,																# prepare
		lambda response, status: status == 404,								# validateInitial
		None,																# change
		None,																# validateMidway
		lambda response, status: status == 404,								# validateFinal
		None,																# cleanup
	),
	(
		'file exist',														# name
		lambda fileName: writeFile(BASE_PATH + fileName, FINAL_CONTENT),	# prepare
		lambda response, status: response == FINAL_CONTENT,					# validateInitial
		None,																# change
		None,																# validateMidway
		lambda response, status: response == FINAL_CONTENT,					# validateFinal
		lambda fileName: os.remove(BASE_PATH + fileName),					# cleanup
	),
	(
		'dir exist',														# name
		lambda fileName: os.mkdir(BASE_PATH + fileName),					# prepare
		lambda response, status: status == 403,								# validateInitial
		None,																# change
		None,																# validateMidway
		lambda response, status: status == 403,								# validateFinal
		lambda fileName: os.rmdir(BASE_PATH + fileName),					# cleanup
	),
	(
		'create file',														# name
		None,																# prepare
		lambda response, status: status == 404,								# validateInitial
		lambda fileName: writeFile(BASE_PATH + fileName, FINAL_CONTENT),	# change
		None,																# validateMidway
		lambda response, status: response == FINAL_CONTENT,					# validateFinal
		lambda fileName: os.remove(BASE_PATH + fileName),					# cleanup
	),
	(
		'create dir',														# name
		None,																# prepare
		lambda response, status: status == 404,								# validateInitial
		lambda fileName: os.mkdir(BASE_PATH + fileName),					# change
		None,																# validateMidway
		lambda response, status: status == 403,								# validateFinal
		lambda fileName: os.rmdir(BASE_PATH + fileName),					# cleanup
	),
	(
		'delete file',														# name
		lambda fileName: writeFile(BASE_PATH + fileName, INITIAL_CONTENT),	# prepare
		lambda response, status: response == INITIAL_CONTENT,				# validateInitial
		lambda fileName: os.remove(BASE_PATH + fileName),					# change
		None,																# validateMidway
		lambda response, status: status == 404,								# validateFinal
		None,																# cleanup
	),
	(
		'delete dir',														# name
		lambda fileName: os.mkdir(BASE_PATH + fileName),					# prepare
		lambda response, status: status == 403,								# validateInitial
		lambda fileName: os.rmdir(BASE_PATH + fileName),					# change
		None,																# validateMidway
		lambda response, status: status == 404,								# validateFinal
		None,																# cleanup
	),
	(
		'update file',														# name
		lambda fileName: writeFile(BASE_PATH + fileName, INITIAL_CONTENT),	# prepare
		lambda response, status: response == INITIAL_CONTENT,				# validateInitial
		lambda fileName: writeFile(BASE_PATH + fileName, FINAL_CONTENT),	# change
		lambda response, status: response == MIXED_CONTENT,					# validateMidway
		lambda response, status: response == FINAL_CONTENT,					# validateFinal
		lambda fileName: os.remove(BASE_PATH + fileName),					# cleanup
	),
	(
		'file to dir',														# name
		lambda fileName: writeFile(BASE_PATH + fileName, INITIAL_CONTENT),	# prepare
		lambda response, status: response == INITIAL_CONTENT,				# validateInitial
		changeFileToDir,													# change
		None,																# validateMidway
		lambda response, status: status == 403,								# validateFinal
		lambda fileName: os.rmdir(BASE_PATH + fileName),					# cleanup
	),
	(
		'dir to file',														# name
		lambda fileName: os.mkdir(BASE_PATH + fileName),					# prepare
		lambda response, status: status == 403,								# validateInitial
		changeDirToFile,													# change
		None,																# validateMidway
		lambda response, status: response == FINAL_CONTENT,					# validateFinal
		lambda fileName: os.remove(BASE_PATH + fileName),					# cleanup
	),
]

TEST_MATRIX = {
	'test': TEST_SCENARIOS,
	'initialCacheLoad': [True, False],
	'blockType': ['pre', 'post', 'fail'],
	'secondBlockType': ['none', 'pre', 'post', 'fail'],
	'requestBeforeSleep1': ['yes', 'no', 'fail'],
	'sleepTime1': [0, CACHE_VALID_TIME, CACHE_INACTIVE_TIME],
	'requestAfterSleep1': ['yes', 'no', 'fail'],
	'requestBeforeSleep2': ['yes', 'no', 'fail'],
	'sleepTime2': [0, CACHE_VALID_TIME, CACHE_INACTIVE_TIME],
	'requestAfterSleep2': ['yes', 'no', 'fail'],
}

# clean up any leftovers from previous runs
os.system('rm -rf %stest-*' % BASE_PATH)

# build the list of tests to run
allOptions = generateAllOptions(TEST_MATRIX)
if len(TESTS_TO_RUN) == 0:
	random.shuffle(allOptions)
else:
	allOptions = map(lambda x: allOptions[x], TESTS_TO_RUN)

# create worker threads
threads = []
for i in xrange(THREAD_COUNT):
	threads.append(TestThread(i, THREAD_COUNT, allOptions))

# start threads
for thread in threads:
	thread.start()
print('Started, running %s tests in %s threads' % (len(allOptions), len(threads)))

# wait until all threads complete
lastCount = 0
while True:
	hasAlive = False
	completeCount = 0
	for thread in threads:
		if thread.isAlive():
			hasAlive = True
		completeCount += thread.completeCount
	if not hasAlive:
		break
		
	if completeCount >= lastCount + 100:
		sys.stdout.write('.')
		sys.stdout.flush()
		lastCount = completeCount
		
	time.sleep(5)

print('Done !')
