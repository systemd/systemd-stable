#!/usr/bin/env python3
import sys

for line in open(sys.argv[1]):
    print('"{}\\0"'.format(line.strip()))
