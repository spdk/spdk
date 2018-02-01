import json
import os.path
from client import print_dict

def load(args):
    filename = args.filename
    with open(filename, 'r') as json_file:
        conf = json.load(json_file)
    params = conf.get('config', None)
    method = conf.get('method', None)
    args.client.call(method, params, verbose=args.verbose)


def dump(args):
    method = args.method
    conf = args.client.call(method, verbose=args.verbose)
    print_dict(conf)
    filename = args.filename
    if args.indent:
        ind = int(args.indent)
    else:
        ind = None
    if args.key_sep and args.item_sep:
        ksep = args.key_sep
        isep = args.item_sep
        seps = (ksep, isep)
    else:
        seps = None
    with open(filename, 'w') as json_file:
        json.dump(conf, json_file, indent=ind, separators=seps) 
