#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * call this at least once before calling funtrace_save_trace(); call it again
 * if shared objects where tracing is enabled were loaded into the process that
 * weren't loaded the last time you called funtrace_save_trace().
 */
void funtrace_save_maps();
/*
 * call this when you want to save the trace of all the live threads.
 */
void funtrace_save_trace();

#ifdef __cplusplus
}
#endif
