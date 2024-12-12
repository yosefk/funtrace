#!/bin/sh
set -ex
gcc -O3 -g -c test.c -finstrument-functions -finstrument-functions-exclude-file-list=.h,.hpp,/usr/include
g++ -O3 -g -o test_trace funtrace.cpp test.o -Wall
g++ -O3 -g -o test_count funcount.cpp test.o -Wall
