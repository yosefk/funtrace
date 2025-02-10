#!/bin/bash
set -ex
cd ..
rm -f funtrace/funtrace.zip
zip funtrace/funtrace.zip funtrace/README.md funtrace/funtrace.cpp funtrace/funcount.cpp funtrace/funtrace.h funtrace/funtrace_flags.h funtrace/*.S funtrace/funtrace.dyn \
    funtrace/target/x86_64-unknown-linux-gnu/release/{funcount2sym,funtrace2viz} funtrace/compiler-wrappers/* funtrace/compiler-wrappers/xray/* funtrace/simple-example/*
