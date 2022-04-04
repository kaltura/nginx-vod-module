## nginx-vod-module tests

the accompanied python scripts test the nginx-vod-module, each test script has an associated template file
that contains environment specific parameters. the template file should be copied and edited to match the
specific environment on which the tests are run.
the tests assume that nginx is running with the accompanied nginx.conf file.

### memory pollution

for additional coverage it is recommended to patch ngx_alloc & ngx_memalign (in src/os/unix/ngx_alloc.c)
so that they memset the allocated pointer with some value. the purpose of this change is to ensure the
code does not make any assumptions on allocated pointers being zeroed. ngx_calloc must not be patched.
to apply the patch - edit ngx_alloc and the two implementations of ngx_memalign, and add the following
block after the if:
    else {
        memset(p, 0xBD, size);
    }

### main.py

sanity + coverage tests, e.g.:
  1. bad request parameters
  2. bad upstream responses
  3. file not found

in order to run the tests nginx should be compiled with the --with-debug switch

### hls_compare.py

compares the nginx-vod hls implementation to some reference implementation

### clip_compare.py

compares the nginx-vod mp4 clipping implementation to nginx-mp4-module

### validate_iframes.py

verifies using ffprobe that the byte ranges returned in an EXT-X-I-FRAMES-ONLY m3u8 indeed represent iframes

### setup_test_entries.py / verify_test_entries.py

setup_test_entries.py - crafts different types of corrupted mp4 files and uploads them to a Kaltura account
verify_test_entries.py - feeds the entries created by setup_test_entries.py to nginx-vod-module and validates the result.
	gets the output of setup_test_entries.py as input (contains the test uris and their expected results).

### test_coverage.py

prints a list of nginx-vod-module log lines that do not appear in the provided log file.
can be executed after running main.py and verify_test_entries.py to get a sense of missing test cases.

### buffer_cache

this folder contains a stress test for the buffer cache module. in order to execute the test, run:
 * NGX_ROOT=/path/to/nginx/sources VOD_ROOT=/path/to/nginx/vod bash build.sh
 * ./bctest

### json_parser

this folder contains tests for the json parser module. in order to execute the test, run:
 * NGX_ROOT=/path/to/nginx/sources VOD_ROOT=/path/to/nginx/vod bash build.sh
 * ./jsontest

### bitset

this folder contains tests for the light bitset implementation. in order to execute the test, run:
 * NGX_ROOT=/path/to/nginx/sources VOD_ROOT=/path/to/nginx/vod bash build.sh
 * ./bitsettest
