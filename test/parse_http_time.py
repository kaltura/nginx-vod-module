import email.utils as eut
import calendar
import sys

print calendar.timegm(eut.parsedate(sys.argv[1]))
