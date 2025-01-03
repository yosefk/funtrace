#pragma once

//the ftrace events buffer size is set in the number of events, not in bytes.
//in a test 20K events took about 1MB but YMMV
#ifndef FUNTRACE_FTRACE_EVENTS_IN_BUF
#define FUNTRACE_FTRACE_EVENTS_IN_BUF 20000
#endif

//the buffer size is in bytes. the size of an event in the current implementation
//is 32 bytes. the size is defined as a log (the size must be a power of 2)
#ifndef FUNTRACE_LOG_BUF_SIZE
#define FUNTRACE_LOG_BUF_SIZE 20
#endif

#define FUNTRACE_BUF_SIZE (1 << FUNTRACE_LOG_BUF_SIZE)

#define FUNTRACE_RETURN_BIT 63
#define FUNTRACE_TAILCALL_BIT 62
#define FUNTRACE_CATCH_BIT 61
