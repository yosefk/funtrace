#pragma once

//these definitions must be kept in sync with funtrace2viz's
#define FUNTRACE_RETURN_BIT 63 //normally, a return event logs the address of the returning function...
#define FUNTRACE_RETURN_WITH_CALLER_ADDRESS_BIT 62 //...except under XRay when it logs the returning function's caller's address
#define FUNTRACE_CATCH_MASK ((1ULL<<FUNTRACE_RETURN_BIT)|(1ULL<<FUNTRACE_RETURN_WITH_CALLER_ADDRESS_BIT)) //since an event can't
//be both things described by these 2 bits, we can reserve their combination to mean "catch event"
