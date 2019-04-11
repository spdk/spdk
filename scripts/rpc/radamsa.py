from subprocess import Popen, PIPE
import sys

radamsa_bin = '/usr/bin/radamsa'


def mutate(payload):
    try:
        radamsa = [radamsa_bin, '-n', '1']
        p = Popen(radamsa, stdin=PIPE, stdout=PIPE)
        rdata = p.communicate(bytes(payload, 'utf-8'))[0]
        return rdata
    except Exception as e:
        print("can't find 'radamsa' in %s ." % radamsa_bin)
        sys.exit(1)
