#!/usr/bin/env python
import sys
from common import FioCommon


def main():

    fio_test = FioCommon('nvmf')
    fio_test.run_fio(sys.argv[1:])

if __name__ == "__main__":

        main()
