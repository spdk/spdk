from subprocess import Popen, PIPE
import sys

radamsa_bin = '/usr/bin/radamsa'


def mutate(payload):
    return mutate_mp(payload,None,'od')
        
def mutate_mp(payload,mutations,pattern):
    if pattern not in ['od','nd','bu']:
        print("pattern must be 'od','nd','bu' .")
        sys.exit(1)

    try:
        radamsa = [radamsa_bin, '-n', '1','-p',pattern]
        if mutations:
            radamsa += ['-m',mutations]
        p = Popen(radamsa, stdin=PIPE, stdout=PIPE)
        rdata = p.communicate(bytes(payload, 'utf-8'))[0]
        return rdata
    except Exception as e:
        print("can't find 'radamsa' in %s ." % radamsa_bin)
        sys.exit(1)

