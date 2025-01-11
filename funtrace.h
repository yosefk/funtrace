/*
 * there are 2 ways to save the trace:
 *
 * - by getting & saving aside trace data upon interesting events of your choice,
 *   and eventually writing them out at the time of your choosing. this is good,
 *   for instance, for keeping a trace corresponding to the slowest observed
 *   handling of every kind of event (so you throw out this trace and replace it
 *   with a new one every time you observe an even slower event), and writing
 *   it all out upon request or when the program terminates.
 *
 * - by writing to the funtrace.raw file (which is only opened if you call
 *   funtrace_pause_and_write_current_snapshot() or use `kill -SIGTRAP` on
 *   the process). this is good if you detect moments of peak
 *   load and want to write the data out immediately, without wasting memory
 *   for keeping the trace data beyond the cyclic buffers already allocated
 *   to collect the trace in the first place (the data is written out
 *   from these buffers while collecting new trace data is paused - same
 *   as it's paused when data is saved aside for writing out later, but
 *   for a longer period of time.) the downside is that you can't "unwrite"
 *   the trace data, and you don't choose when to handle the writing but
 *   rather have it occur immediately after deciding to save the trace.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* to "just append the current trace snapshot to funtrace.raw", all you need
   is this function (this is also what SIGTRAP does unless you compile with
   -DFUNTRACE_NO_SIGTRAP)

   threads cannot be created, and their termination is delayed until the data
   is fully written out

   note that if a shared object was unloaded during the time range in the snapshot
   (thankfully not a very common scenario), function calls traced from this shared
   object will not be possible to decode to symbolic function names (this is true
   for all the functions taking snapshots below)
 */
void funtrace_pause_and_write_current_snapshot();

/* these methods are for saving trace data snapshots, and then
   writing them out at the time of your choosing. */

struct funtrace_snapshot;

/* a snapshot has the size FUNTRACE_BUF_SIZE times the number of threads alive
   at the time when it's taken. threads can't be created and can't terminate
   until the trace data is copied into the snapshot */
struct funtrace_snapshot* funtrace_pause_and_get_snapshot();

/* you might also want to only get the data up to a certain age,
   both to save time & space and to get "the part you want" (like from the
   start of handling some event till the end) */
uint64_t funtrace_time(); /* timestamp from the same source used for tracing */
uint64_t funtrace_ticks_per_second(); /* funtrace_time()/funtrace_ticks_per_second() converts time to seconds */

struct funtrace_snapshot* funtrace_pause_and_get_snapshot_starting_at_time(uint64_t time);
struct funtrace_snapshot* funtrace_pause_and_get_snapshot_up_to_age(uint64_t max_event_age);
void funtrace_free_snapshot(struct funtrace_snapshot* snapshot);

/* writing out a sample into its own file after it was obtained with funtrace_pause_and_get_snapshot()
   does not interfere with threads starting and terminating. TODO: we could add a version with
   a "write_data" callback instead of a filename given demand */
void funtrace_write_snapshot(const char* filename, struct funtrace_snapshot* snapshot);

/* this is useful to save memory for the event buffer in threads you don't want to trace,
   and also to save some but not all of the function call overhead due to being compiled
   with tracing enabled */
void funtrace_ignore_this_thread();

/* set this thread's buffer size (must be a power of 2, so defined by a log value,
   which must be larger the log of the size of 2 events;
   using a smaller value is equivalent to callung funtrace_ignore_this_thread()). */
void funtrace_set_thread_log_buf_size(int log_buf_size);

/* disabling tracing will speed things up slightly. note that we don't
   free the buffers when disabling tracing and don't reallocate them
   when enabling tracing. funtrace_ignore_this_thread() is how you free
   the buffer of a thread. */
void funtrace_disable_tracing();
void funtrace_enable_tracing();

#ifdef __clang__
#define NOFUNTRACE __attribute__((xray_never_instrument)) __attribute__((no_instrument_function))
#define DOFUNTRACE __attribute__((xray_always_instrument))
#else
#define NOFUNTRACE __attribute__((no_instrument_function))
#define DOFUNTRACE /* gcc doesn't have an attribute to force instrumentation */
#endif

#ifdef __cplusplus
}
#endif
