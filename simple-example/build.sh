#!/bin/sh
set -ex

cleanup() {
	rm *.o
}

trap cleanup EXIT

# building the offline conversion tools:
########################################
if [ -e Cargo.toml ]; then
	RUSTFLAGS="-C target-feature=+crt-static" cargo build -r --target x86_64-unknown-linux-gnu
fi

# building tests using the funtrace instrumentation & runtime:
##############################################################

CXXFLAGS="-O3 -std=c++11 -Wall -I."
# the wrappers pass all the flags we need for tracing to be built into the output program;
# XRay is an exception in that in our example, we need to lower the instruction threshold
# or we'll get an empty trace
CLANGXRAYFLAGS="$CXXFLAGS -fxray-instruction-threshold=1"

mkdir -p out

CXX=./compiler-wrappers/funtrace-finstr-g++
$CXX -c simple-example/shared.cpp -fPIC $CXXFLAGS
$CXX -o out/test_shared.fi-gcc.so shared.o -fPIC -shared $CXXFLAGS
$CXX -c simple-example/test.cpp $CXXFLAGS
$CXX -o out/test_trace.fi-gcc test.o out/test_shared.fi-gcc.so $CXXFLAGS

CXX=./compiler-wrappers/funtrace-pg-g++
$CXX -c simple-example/shared.cpp -fPIC $CXXFLAGS
$CXX -o out/test_shared.pg.so shared.o -fPIC -shared $CXXFLAGS
$CXX -c simple-example/test.cpp $CXXFLAGS
$CXX -o out/test_trace.pg test.o out/test_shared.pg.so $CXXFLAGS

CXX=./compiler-wrappers/funtrace-finstr-clang++
$CXX -c simple-example/shared.cpp -fPIC $CXXFLAGS
$CXX -o out/test_shared.fi-clang.so shared.o -fPIC -shared $CXXFLAGS
$CXX -c simple-example/test.cpp $CXXFLAGS
$CXX -o out/test_trace.fi-clang test.o out/test_shared.fi-clang.so $CXXFLAGS

CXX=./compiler-wrappers/funtrace-xray-clang++
$CXX -c simple-example/shared.cpp -fPIC $CLANGXRAYFLAGS
# this command will fail with older clang
$CXX -o out/test_shared.xray.so shared.o -fPIC -shared $CLANGXRAYFLAGS
$CXX -c simple-example/test.cpp $CLANGXRAYFLAGS
$CXX -o out/test_trace.xray test.o out/test_shared.xray.so $CLANGXRAYFLAGS
