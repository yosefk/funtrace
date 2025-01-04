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

//these definitions must be kept in sync with funtrace2viz's
#define FUNTRACE_RETURN_BIT 63 //normally, a return event logs the address of the returning function...
#define FUNTRACE_RETURN_WITH_CALLER_ADDRESS_BIT 62 //...except under XRay when it logs the returning function's caller's address
#define FUNTRACE_TAILCALL_BIT 61
#define FUNTRACE_CATCH_MASK ((1ULL<<FUNTRACE_RETURN_BIT)|(1ULL<<FUNTRACE_RETURN_WITH_CALLER_ADDRESS_BIT)) //since an event can't
//be both things described by these 2 bits, we can reserve their combination to mean "catch event"
