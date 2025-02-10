#!/bin/sh

set -ex

./out/test_trace.fi-gcc
./target/x86_64-unknown-linux-gnu/release/funtrace2viz funtrace.raw out/funtrace-fi-gcc
rm funtrace.raw

./out/test_trace.pg
./target/x86_64-unknown-linux-gnu/release/funtrace2viz funtrace.raw out/funtrace-pg
rm funtrace.raw

./out/test_trace.fi-clang
./target/x86_64-unknown-linux-gnu/release/funtrace2viz funtrace.raw out/funtrace-fi-clang
rm funtrace.raw

if [ -e ./out/test_trace.xray ]; then
	env XRAY_OPTIONS="patch_premain=true" ./out/test_trace.xray
	./target/x86_64-unknown-linux-gnu/release/funtrace2viz funtrace.raw out/funtrace-xray
	rm funtrace.raw
fi
