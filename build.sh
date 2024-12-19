#!/bin/sh
set -ex
CXXFLAGS="-O3 -g -std=c++11 -finstrument-functions -finstrument-functions-exclude-file-list=.h,.hpp,/usr/include -Wall -pthread"
g++ -c test.cpp $CXXFLAGS
g++ -c funtrace.cpp $CXXFLAGS
g++ -o test_trace.finstr test.o funtrace.o -ldl -pthread
#g++ -O3 -g -o test_count funcount.cpp test.o -Wall
CXXFLAGS="-O3 -g -std=c++11 -mfentry -minstrument-return=call -pg -Wall -pthread"
g++ -c test.cpp $CXXFLAGS
g++ -c funtrace.cpp $CXXFLAGS
g++ -S funtrace.cpp $CXXFLAGS
g++ -o test_trace test.o funtrace.o -ldl -pthread funtrace_pg.S
