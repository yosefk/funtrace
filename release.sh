#!/bin/bash
set -ex
zip funtrace.zip README.md funtrace.cpp funcount.cpp funtrace.h funtrace_flags.h *.S funtrace.dyn \
    target/x86_64-unknown-linux-gnu/release/{funcount2sym,funtrace2viz} compiler-wrappers/* compiler-wrappers/xray/* simple-example/*
