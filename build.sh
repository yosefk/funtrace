#!/bin/sh
set -ex
CXXFLAGS="-O3 -std=c++11 -Wall"

CXX=./compiler-wrappers/funtrace-finstr-g++
$CXX -c shared.cpp -fPIC $CXXFLAGS
$CXX -o test_shared.finstr.so shared.o -fPIC -shared $CXXFLAGS
$CXX -c test.cpp $CXXFLAGS
$CXX -o test_trace.finstr test.o ./test_shared.finstr.so $CXXFLAGS

CXX=./compiler-wrappers/funtrace-pg-g++
$CXX -c shared.cpp -fPIC $CXXFLAGS
$CXX -o test_shared.pg.so shared.o -fPIC -shared $CXXFLAGS
$CXX -c test.cpp $CXXFLAGS
$CXX -o test_trace.pg test.o ./test_shared.pg.so $CXXFLAGS
