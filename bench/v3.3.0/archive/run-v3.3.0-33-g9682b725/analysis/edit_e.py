import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from edlib import *
d = Doc(1)
d.rep("(section 9). Against PostgreSQL 18.6,", "(section 9). The one apparent regression, `bar-lookup` at `cores = 1` (-14.4 %), re-ran as noise (section 3). Against PostgreSQL 18.6,")
d.save()
print("ok")
