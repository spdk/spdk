#!/usr/bin/env python
import sys
from common import fio_common

def main():

    fio_test = fio_common('nvmf')
    fio_test.run_fio(sys.argv[1:])

if __name__ == "__main__":

        main()
