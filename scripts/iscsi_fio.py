#!/usr/bin/env python
import sys
from common import FioCommon

def main():

    fio_test = FioCommon('iscsi')
    fio_test.run_fio(sys.argv[1:], fio_test.devices)

if __name__ == "__main__":

        main()
