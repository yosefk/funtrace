#pragma once

//these definitions must be kept in sync with funtrace2viz's
#define FUNTRACE_RETURN_BIT 63 //normally, a return event logs the address of the returning function...
#define FUNTRACE_RETURN_WITH_CALLER_ADDRESS_BIT 62 //...except under XRay when it logs the returning function's caller's address
#define FUNTRACE_CATCH_MASK ((1ULL<<FUNTRACE_RETURN_BIT)|(1ULL<<FUNTRACE_RETURN_WITH_CALLER_ADDRESS_BIT)) //since an event can't
//be both things described by these 2 bits, we can reserve their combination to mean "catch event"

//under most kinds of instrumentation, we don't get a return event upon throw,
//so we pop the call entries from the stack when a catch event is traced, until we find
//the caller which recorded the catch event. unfortunately that caller might have been
//uninstrumented, in which case we pop the entire stack (the least bad option in the
//general case which might be improved upon slightly in some cases but not always.)
//
//since under gcc's -finstrument-functions, we actually _do_ get a return event upon throw,
//we mark such call entries by setting the bit below; so that when the trace decoder
//encounters such an entry when processing a catch event, it stops popping entries
//from the stack (since that function would have recorded a return event had it
//been returned from during stack unwinding.)
#define FUNTRACE_CALL_RETURNING_UPON_THROW_BIT 61
