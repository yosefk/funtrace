#pragma once

//the buffer size is in bytes. the size of an event in the current implementation
//is 32 bytes. the size is defined as a log (the size must be a power of 2)
#ifndef FUNTRACE_LOG_BUF_SIZE
#define FUNTRACE_LOG_BUF_SIZE 18
#endif

#define FUNTRACE_BUF_SIZE (1 << FUNTRACE_LOG_BUF_SIZE)

#define FUNTRACE_RETURN_BIT 63
