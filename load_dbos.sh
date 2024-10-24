#!/bin/bash

if lsmod | grep -q dune; then rmmod dune; fi
cd kern; make && insmod dune.ko && cd ..
cd libdune; make && cd ..
