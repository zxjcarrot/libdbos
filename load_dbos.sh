#!/bin/bash

if lsmod | grep -q dune; then rmmod dune; fi
cd kern; make && insmod dune.ko && cd ..
cd libdune; make && cd ..
echo 5000 > /proc/sys/vm/nr_hugepages # allocate 10GB huge pages for libdbos