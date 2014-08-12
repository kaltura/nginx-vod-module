## nginx-vod-module tests

the accompanied python scripts test the nginx-vod-module, each test script has an associated template file
that contains environment specific parameters. the template file should copied and edited to match the 
specific environment on which the tests are run.
the tests assume that nginx is running with the accompanied nginx.conf file.

### main.py

sanity + coverage tests, e.g.:
  1. bad request parameters
  2. bad upstream responses
  3. file not found

in order to run the tests nginx should be compiled with the --with-debug switch
  
### hls_compare.py

compares the nginx-vod hls implementation to some reference implementation

### validate_iframes.py

verifies using ffprobe that the byte ranges returned in an EXT-X-I-FRAMES-ONLY m3u8 indeed represent iframes
