from subprocess import Popen, PIPE
import sys
import re
import random

radamsa_bin = '/usr/bin/radamsa'
number_re = r'(?<=:)(\s?[0-9]\s?)(?=,)'

# -1 , 2 ^ 64 + 1 , - (2 ^ 64 + 1)
random_number = ["-1", "18446744073709551617", "-18446744073709551617"]

'''
Radamsa Mutations :
    ab: enhance silly issues in ASCII string data handling
    bd: drop a byte
    bf: flip one bit
    bi: insert a random byte
    br: repeat a byte
    bp: permute some bytes
    bei: increment a byte by one
    bed: decrement a byte by one
    ber: swap a byte with a random one
    sr: repeat a sequence of bytes
    sd: delete a sequence of bytes
    ld: delete a line
    lds: delete many lines
    lr2: duplicate a line
    li: copy a line closeby
    lr: repeat a line
    ls: swap two lines
    lp: swap order of lines
    lis: insert a line from elsewhere
    lrs: replace a line with one from elsewhere
    td: delete a node
    tr2: duplicate a node
    ts1: swap one node with another one
    ts2: swap two nodes pairwise
    tr: repeat a path of the parse tree
    uw: try to make a code point too wide
    ui: insert funny unicode
    num: try to modify a textual number
    xp: try to parse XML and mutate it
    ft: jump to a similar position in block
    fn: likely clone data between similar positions
    fo: fuse previously seen data elsewhere
    nop: do nothing (debug/test)
'''

radamsa_mutate = [
    'ab', 'bd', 'bf', 'bi', 'br', 'bp', 'bei', 'bed', 'ber', 'sr', 'sd', 'ld', 'lds',
    'lr2', 'li', 'lr', 'ls', 'lp', 'lis', 'lrs', 'td', 'tr2', 'ts1',
    'ts2', 'tr', 'uw', 'ui', 'num', 'xp', 'ft', 'fn', 'fo'
]


def mutate(payload):
    return mutate_mp(payload, None, 'od')


def mutate_mp(payload, mutations, pattern):
    if pattern not in ['od', 'nd', ' bu']:
        print("pattern must be 'od', 'nd', 'bu' .")
        sys.exit(1)

    try:
        radamsa = [radamsa_bin, '-n', '1', '-p', pattern]
        if mutations:
            radamsa += ['-m', mutations]
        p = Popen(radamsa, stdin=PIPE, stdout=PIPE)
        rdata = p.communicate(bytes(payload, 'utf-8'))[0]
        return rdata
    except Exception as e:
        print("can't find 'radamsa' in %s ." % radamsa_bin)
        sys.exit(1)


def mutate_all(payload):
    result = []

    for mutation in radamsa_mutate:
        rdata = mutate_mp(payload, mutation, 'od')
        result.append(rdata)

    matchs = re.finditer(number_re, payload)
    if matchs is not None:
        boundary_test = payload
        for match in matchs:
            boundary_test = "".join((boundary_test[:match.span()[0]],
                                    random.choice(random_number),
                                    boundary_test[match.span()[1]:])
                                    )
        result.append(boundary_test)

    # last add the normal request
    rdata = mutate_mp(payload, 'nop', 'od')
    result.append(rdata)

    return result
