#!/usr/bin/python3
import json
import os
import glob
from multiprocessing import Pool

call='+'
ret='-'

def parse_perfetto_json(fname):
    with open(fname) as f:
        data = json.load(f)
    events = data['traceEvents']
    threads = {}
    thread_names = {}
    thread2timestamps = {}
    for event in events:
        phase = event['ph']
        tid = event['tid']
        name = event['name']
        if 'std::thread::_Invoker' in name or 'std::thread::thread' in name: # we use std::thread in tests - ignore the noise it adds to traces
            continue # 
        if phase == 'M': # metadata
            if name == 'thread_name':
                thread_names[tid] = event['args']['name']
                if thread_names[tid] in threads: # not a unique name
                    thread_names[tid] += '.%d'%tid # mangle by tid
            continue
        assert phase == 'X' # complete event
        timepoints = threads.setdefault(thread_names[tid], list())
        timestamp = event['ts']
        duration = event['dur']

        timestamps = thread2timestamps.setdefault(tid, dict())

        assert timestamp not in timestamps, f'expecting unique timestamps in every thread! 2 events with the same timestamp: call of {event}; {timestamps[timestamp]}'
        assert timestamp+duration not in timestamps, f'expecting unique timestamps in every thread! 2 events with the same timestamp: return of {event}; {timestamps[timestamp+duration]}'
        timestamps[timestamp] = ('call',event)
        timestamps[timestamp+duration] = ('ret',event)

        timepoints.append((call, name, timestamp))
        timepoints.append((ret, name, timestamp+duration))

    # sort by the timestamp
    for timepoints in threads.values():
        timepoints.sort(key=lambda t: (t[2]))

    data['threads'] = threads

    return data

def print_thread(flow,line=-1):
    level = 0
    for i,point in enumerate(flow):
        what = point[0]
        name = point[1]
        if what == ret:
            level -= 1
        start = '  '*level
        if line>=0:
            if i<line:
                start='|'+start
            elif i==line:
                start='V'+start
            else:
                start=' '+start
        print(start,what,name)
        if what == call:
            level += 1

def verify_thread(timepoints, ref_calls_and_returns):
    ok = True
    n = len(timepoints)
    errline = -1
    if len(timepoints) != len(ref_calls_and_returns):
        print(f'mismatch in the number of events (expected {len(ref_calls_and_returns)}, found {n})')
        ok = False
        n = min(n, len(ref_calls_and_returns))
    for i,((what_ref,func),(what,name,_)) in enumerate(zip(ref_calls_and_returns[:n], timepoints[:n])):
        if what != what_ref or (func+'(' not in name and func+' ' not in name):
            print('expected',what_ref,func,', found',what,name)
            ok = False
            errline = i
            break
    if not ok:
        print('expected:')
        print_thread(ref_calls_and_returns,errline)
        print('found:')
        print_thread(timepoints,errline)
    return ok

def fn(name, inner=[]):
    return [(call,name)] + inner + [(ret,name)]

# check that the ignored threads as well as the part running when tracing was
# disabled and before it was enabled again weren't traced. there should be 2 children threads
ignore_disable_main_ref = [
    (call,'run_threads'),
    (call,'should_be_traced'),
    (ret,'should_be_traced'),
    (ret,'run_threads'),
]*2
ignore_disable_child_ref = [
    (call,'traced_thread'),
    (call,'should_be_traced'),
    (ret,'should_be_traced'),
    (ret,'traced_thread'),
]

exceptions_ref = [
    (call,'caller'),
    (call,'catcher'),
    (call,'before_try'),
    (ret,'before_try'),
    (call,'wrapper_call_outer'),
    (call,'wrapper_tailcall_2'),
    (call,'wrapper_tailcall_1'),
    (call,'wrapper_call'),
    (call,'thrower'),
    (call,'__cxa_throw'),
    (ret,'__cxa_throw'),
    (ret,'thrower'), # throw going to the catch block should be decoded as all the
    # unwound functions returning
    (ret,'wrapper_call'),
    (ret,'wrapper_tailcall_1'),
    (ret,'wrapper_tailcall_2'),
    (ret,'wrapper_call_outer'),
    (call,'__cxa_begin_catch'),
    (ret,'__cxa_begin_catch'),
    (call,'after_catch'),
    (ret,'after_catch'),
    (call,'__cxa_end_catch'),
    (ret,'__cxa_end_catch'),
    (ret,'catcher'),
    (ret,'caller'),
]*3

# the "clean" case [supplied by gcc -finstrument-functions] is for the untraced
# catcher to simply disappear from the trace but without any other artifacts
clean_untraced_caller_ref = [(evt,func) for evt,func in exceptions_ref if func!='catcher']

unfortunate_full_unwinding = [
    (call,'caller'),
    (call,'before_try'),
    (ret,'before_try'),
    (call,'wrapper_call_outer'),
    (call,'wrapper_tailcall_2'),
    (call,'wrapper_tailcall_1'),
    (call,'wrapper_call'),
    (call,'thrower'),
    (call,'__cxa_throw'),
    (ret,'__cxa_throw'),
    (ret,'thrower'), # throw going to the catch block should be decoded as all the
    # unwound functions returning
    (ret,'wrapper_call'),
    (ret,'wrapper_tailcall_1'),
    (ret,'wrapper_tailcall_2'),
    (ret,'wrapper_call_outer'),
    (ret,'caller'),
    (call,'__cxa_begin_catch'),
    (ret,'__cxa_begin_catch'),
    (call,'after_catch'),
    (ret,'after_catch'),
    (call,'__cxa_end_catch'),
    (ret,'__cxa_end_catch'),
]

def caller_wrapping(events): return [(call,'caller')] + events + [(ret,'caller')]
def catcher_wrapping(events): return [(call,'catcher')] + events + [(ret,'catcher')]

# in the "dirty" untraced caller case, we have 2 artifacts:
# 1. the call stack is fully unwound since we don't know where to stop (the catcher's call event wasn't traced;
#    so for all we know the catcher was the outermost caller)
# 2. when the caller of the catcher returns, it is treated as an "orphan return" and we get the "illusion"
#    that the caller was called more than it actually was since we make up a call event for the orphan return
#    (this artifact could be avoided in most cases by doing more work in the tracer but it wouldn't
#    solve the 1st artifact, definitely not when the return from the caller was never logged, eg because it didn't happen
#    by the time the snapshot was taken)
dirty_untraced_caller_ref = caller_wrapping(caller_wrapping(caller_wrapping(unfortunate_full_unwinding) + unfortunate_full_unwinding) + unfortunate_full_unwinding)
# with XRay, the catcher is the one for which we see "orphan returns" after unwinding [due to XRay's funky return address logging]
dirty_untraced_catcher_xray_ref = catcher_wrapping(catcher_wrapping(catcher_wrapping(unfortunate_full_unwinding) + unfortunate_full_unwinding) + unfortunate_full_unwinding)

# test that the traced funcs were actually traced and that untraced ones weren't.
# if potentially non-trivial, it's mainly for XRay with its return address logging
untraced_funcs_ref = [
    (call, 'tr2'),
    (call, 'tr1'),
    (ret, 'tr1'),
    (ret, 'tr2'),
    (call, 'tr1'),
    (ret, 'tr1'),
    (call, 'tr4'),
    (call, 'tr3'),
    (ret, 'tr3'),
    (ret, 'tr4'),
    (call, 'tr4'),
    (call, 'tr3'),
    (ret, 'tr3'),
    (ret, 'tr4'),
]

longjmp_ref = [
    (call,'setter'),
    (call,'before_setjmp'),
    (ret,'before_setjmp'),
    (call,'wrapper_call_outer'),
    (call,'wrapper_call'),
    (call,'jumper'),
    (call,'after_longjmp'), # this is not the call sequence in the code - after_longjmp is called
    # after we return from setjmp in setter - but this is what we expect to decode since we don't
    # log longjmp events, rather this is a test of our partial recovery from seeing a return from
    # setter instead of a return from jumper, which we expect to see after after_longjmp returns
    (ret,'after_longjmp'),
    (ret,'jumper'),
    (ret,'wrapper_call'),
    (ret,'wrapper_call_outer'),
    (ret,'setter'),
]*3

def orphans_ref(json):
    # XRay instrumentation loses the address of the first orphan return
    first_returning_function = '??' if 'xray' in json else 'orphan_return_3'
    return [
        # the first 3 call events are fake...
        (call,'orphan_return_1'),
        (call,'orphan_return_2'),
        (call,first_returning_function),
        (ret,first_returning_function),
        (ret,'orphan_return_2'),
        (call,'called_and_returned'),
        (ret,'called_and_returned'),
        (ret,'orphan_return_1'),
        (call,'called_and_returned'),
        (ret,'called_and_returned'),
        (call,'orphan_call_1'),
        (call,'called_and_returned'),
        (ret,'called_and_returned'),
        (call,'orphan_call_2'),
        (call,'called_and_returned'),
        (ret,'called_and_returned'),
        # ...and so are the 2 return events
        (ret,'orphan_call_2'),
        (ret,'orphan_call_1'),
    ]

buf_size_ref = [
    (call,'f'),
    (ret,'f'),
]

shared_ref = fn('loop',
    (fn('h', fn('g',fn('f')*2)+fn('f'))+
    fn('h_shared', fn('g_shared',fn('f_shared')*2)+fn('f_shared'))+
    fn('h_dyn_shared_c', fn('h_dyn_shared', fn('g_dyn_shared',fn('f_dyn_shared')*2)+fn('f_dyn_shared'))))*3
)

# the idea was to test tail call support, incl cases when we tail-call a function excluded
# from tracing; the result of this testing was to dump tail call support since it's not clear
# how to do it, at least without runtime overhead.
tailcall_clean_ref = (fn('tail_caller', fn('callee')) + fn('tail_caller_untraced'))*3
tailcall_dirty_ref = (fn('tail_caller') + fn('callee') + fn('tail_caller_untraced'))*3

killed_main_ref = fn('main', fn('g', fn('f')))
killed_children_ref = fn('g', fn('f'))

# short_function and long_but_blacklisted should be filtered out
asm_filter_ref = fn('short_but_whitelisted') + fn('long_enough_function')

freq_ref = [
    (call,'usleep_1500'),
    (ret,'usleep_1500'),
]

class funinfo:
    def __init__(self,line,t):
        self.line = line
        self.count = int(t[0])
        self.module = t[3]
        self.file, self.line = t[4].split(':')
class symcount:
    def __init__(self, lines):
        self.lines = [(line,line.split()) for line in lines]
    def info(self,func):
        for line,t in self.lines:
            if func in line:
                return funinfo(line,t)
        return funinfo('none',[0,0,0,'??','??:0'])

def parse_symcount_txt(f):
    return symcount(open(f).read().strip().split('\n'))

def check_count_results(symcount_txt):
    counts = parse_symcount_txt(symcount_txt)
    iters = 1000 
    for name,c in [('f',iters*9),('g',iters*3),('h',iters*3)]:
        fname = name+'()'
        info = counts.info(fname)
        assert info.count == c, f'wrong count for {fname}: expected {c}, got {info.count}'
        assert 'count.cpp' in info.file
        assert '/count' in info.module

        fname = name+'_shared()'
        info = counts.info(fname)
        assert info.count == c, f'wrong count for {fname}: expected {c}, got {info.count}'
        assert 'count_shared.cpp' in info.file
        assert '.so' in info.module

        fname = name+'_dyn_shared()'
        info = counts.info(fname)
        assert info.count == c, f'wrong count for {fname}: expected {c}, got {info.count}'
        assert 'count_dyn_shared.cpp' in info.file
        assert '.so' in info.module

def check_ftrace(ftrace, threads):
    lines = ftrace.strip().split('\n')

    # we renamed 2 threads, one to "parent" and one to "child"
    renames = [line for line in lines if 'task_rename' in line]
    assert len(renames)==2
    assert 'newcomm=parent' in '\n'.join(renames)
    assert 'newcomm=child' in '\n'.join(renames)

    # parent and child both slept, we expect them to have been awakened
    assert 'sched_waking: comm=parent' in ftrace
    assert 'sched_waking: comm=child' in ftrace

    # find the sleeping periods of parent & child
    def time(line):
        t = line.split()[3]
        assert t.endswith(':')
        return float(t[:-1])*10**6

    # check that the threads slept, and that the sleep() duration recorded
    # by funtrace contains the not-runnable duration recorded by ftrace
    # (this checks timestamp synchronization)
    for thread in ['parent','child']:
        start = None
        finish = None
        for line in reversed(lines):
            if f'sched_waking: comm={thread}' in line:
                finish = time(line)
            elif finish is not None and f'sched_switch: prev_comm={thread}' in line:
                start = time(line)
                break
        assert start is not None and finish is not None
        print('thread', thread, 'slept for', finish-start)
        assert finish-start >= 150000

        func_start = None
        func_finish = None
        for what, func, when in threads[thread]:
            if func.startswith('sleep'):
                if what == call:
                    func_start = when
                else:
                    func_finish = when
                    break
        assert func_start is not None and func_finish is not None
        print('  in sleep() for', func_finish - func_start)
        assert start > func_start and finish < func_finish

def system(cmd):
    print('running',cmd)
    status = os.system(cmd)
    if 'killed' not in cmd: # we have a test that kills itself with SIGKILL - other than that commands shouldn't fail
        assert status==0, f'`{cmd}` failed with status {status}'

BUILDDIR = './built-tests'
OUTDIR = './out'
TARGET = 'x86_64-unknown-linux-gnu'

def build_trace_analysis_tools():
    system(f'RUSTFLAGS="-C target-feature=+crt-static" cargo build -r --target {TARGET}')

def run_cmds(cmds):
    for cmd in cmds:
        system(cmd)

def build_cxx_test(main, shared=[], dyn_shared=[], flags=''):
    cmdlists = []
    binaries = {}
    for mode in ['fi-gcc','fi-clang','pg','xray']:
        CXXFLAGS=f"-O3 -std=c++11 -Wall {flags}"
        if mode == 'xray':
            CXXFLAGS += " -fxray-instruction-threshold=1"
        compiler = {
           'fi-gcc':'finstr-g++',
           'fi-clang':'finstr-clang++',
           'pg':'pg-g++',
           'xray':'xray-clang++',
        }
        CXX = f'./compiler-wrappers/funtrace-{compiler[mode]}'
        test = main.split('.')[0]
        binary = f'{BUILDDIR}/{test}.{mode}'
        cmds = []
        LIBS = ''
        DYNLIBS = ''
        if shared or dyn_shared:
            for cpp in shared+dyn_shared:
                module = cpp.split('.')[0]
                lib = f'{os.path.realpath(BUILDDIR)}/{module}.{mode}.so'
                cmds += [
                    f'{CXX} -c tests/{cpp} -o {BUILDDIR}/{module}.{mode}.o {CXXFLAGS} -I. -fPIC',
                    f'{CXX} -o {lib} {BUILDDIR}/{module}.{mode}.o {CXXFLAGS} -fPIC -shared',
                ]
                if cpp in dyn_shared:
                    DYNLIBS += ' '+lib
                else:
                    LIBS += ' '+lib
        dlibs = ''
        if LIBS:
            dlibs = f'-DLIBS=\\"{DYNLIBS.strip()}\\"'
        cmds += [
            f'{CXX} -c tests/{main} -o {BUILDDIR}/{test}.{mode}.o {CXXFLAGS} -I. {dlibs}',
            f'{CXX} -o {binary} {BUILDDIR}/{test}.{mode}.o {CXXFLAGS}{LIBS}',
        ]
        cmdlists.append(cmds)
        binaries.setdefault(test,list()).append(binary)
    return cmdlists, binaries

def run_cxx_test(test, binaries):
    cmdlists = []
    for binary in binaries:
        name = os.path.basename(binary)
        env = ''
        if 'xray' in binary:
            env = 'env XRAY_OPTIONS="patch_premain=true"' 
        cmds = [
            f'mkdir -p {OUTDIR}/{name}',
            f'cd {OUTDIR}/{name}; {env} ../../{binary}',
        ]
        if 'count' in test:
            cmds += [
                f'./target/{TARGET}/release/funcount2sym {OUTDIR}/{name}/funcount.txt | c++filt > {OUTDIR}/{name}/symcount.txt'
            ]
        else:
            cmds += [
                f'./target/{TARGET}/release/funtrace2viz {OUTDIR}/{name}/funtrace.raw {OUTDIR}/{name}/funtrace > {OUTDIR}/{name}/f2v.out'
            ]
        cmdlists.append(cmds)
    return cmdlists


def main():
    global pool
    pool = Pool()
    build_trace_analysis_tools()
    system(f'rm -rf {BUILDDIR}')
    system(f'rm -rf {OUTDIR}')
    system(f'mkdir -p {BUILDDIR}')

    cmdlists = []
    test2bins = {}
    def buildcmds(*args,**kw):
        c,b = build_cxx_test(*args,**kw)
        cmdlists.extend(c)
        test2bins.update(b)

    buildcmds('ignore_disable.cpp')
    buildcmds('exceptions.cpp')
    buildcmds('untraced_catcher.cpp')
    buildcmds('untraced_funcs.cpp')
    buildcmds('longjmp.cpp')
    buildcmds('tailcall.cpp')
    buildcmds('orphans.cpp')
    buildcmds('buf_size.cpp')
    buildcmds('benchmark.cpp',flags=f'-funtrace-no-trace={os.path.realpath("tests/no-trace-bench.txt")}')
    buildcmds('freq.cpp')
    buildcmds('killed.cpp')
    buildcmds('sigtrap.cpp')
    buildcmds('ftrace.cpp')
    buildcmds('asm_filter.cpp',flags=f'-funtrace-instr-thresh=20 -funtrace-no-trace={os.path.realpath("tests/no-trace.txt")} -funtrace-do-trace={os.path.realpath("tests/do-trace.txt")}')
    buildcmds('shared.cpp',shared=['lib_shared.cpp'],dyn_shared=['lib_dyn_shared.cpp'])
    buildcmds('count.cpp',shared=['count_shared.cpp'],dyn_shared=['count_dyn_shared.cpp'],flags='-DFUNTRACE_FUNCOUNT -DFUNCOUNT_PAGE_TABLES=2')
    pool.map(run_cmds, cmdlists)

    cmdlists = []
    killedcmds = []
    for test,binaries in test2bins.items():
        cmds = killedcmds if 'killed' in test else cmdlists # we run killed later
        cmds.extend(run_cxx_test(test,binaries))

    pool.map(run_cmds, cmdlists)
    check()

    pool.map(run_cmds, killedcmds)
    for binary in test2bins['killed']:
        if 'xray' in binary or 'clang' in binary:
            continue # my gdb is too old to parse the latest LLVM's DWARF; there's no better reason for this condition...
        check_funtrace_from_core_dump(binary)
    check_orphan_tracer_removal()

jsonmod = json
def check():
    print('checking results...')

    def load_threads(json):
        return parse_perfetto_json(json)['threads']
    def load_thread(json):
        return list(load_threads(json).values())[0]
    def load_ftrace(json):
        return jsonmod.load(open(json))['systemTraceEvents']

    def jsons(test): return sorted(glob.glob(f'{OUTDIR}/{test}.*/funtrace.json'))

    # funtrace tests [except freq]
    for json in jsons('ignore_disable'):
        print('checking',json)
        threads = load_threads(json)
        assert len(threads) == 3
        for name,thread in threads.items():
            if name in ['child1','child3']:
                assert verify_thread(thread, ignore_disable_child_ref)
            elif name == 'main':
                assert verify_thread(thread, ignore_disable_main_ref)
            else:
                assert False, f'unexpected thread name: {name}'
    for json in jsons('exceptions'):
        print('checking',json)
        assert verify_thread(load_thread(json), exceptions_ref)
    for json in jsons('untraced_catcher'):
        print('checking',json)
        ref = clean_untraced_caller_ref if 'fi-gcc' in json else (dirty_untraced_caller_ref if 'xray' not in json else dirty_untraced_catcher_xray_ref)
        assert verify_thread(load_thread(json), ref)
    for json in jsons('untraced_funcs'): 
        print('checking',json)
        assert verify_thread(load_thread(json), untraced_funcs_ref)
    for json in jsons('longjmp'): 
        print('checking',json)
        assert verify_thread(load_thread(json), longjmp_ref)
    for json in jsons('tailcall'):
        print('checking',json)
        assert verify_thread(load_thread(json), tailcall_clean_ref if 'fi-' in json else tailcall_dirty_ref)
    for json in jsons('orphans'): 
        print('checking',json)
        assert verify_thread(load_thread(json), orphans_ref(json))
    for json in jsons('buf_size'): 
        print('checking',json)
        threads = load_threads(json)
        assert verify_thread(threads['event_buf_1'], buf_size_ref)
        num_f_calls = len([name for _,name,_ in threads['event_buf_16'] if name.startswith('f()')])
        assert num_f_calls <= 16*2 and num_f_calls >= 14*2, f'wrong number of f calls: {num_f_calls}'
    for json in jsons('sigtrap'):
        print('checking',json)
        thread = load_thread(json)
        assert len([name for _,name,_ in thread if name.startswith('traced_func')]) >= 100
    for json in jsons('shared'):
        print('checking',json)
        for thread in load_threads(json).values():
            assert verify_thread(thread, shared_ref)
    for json in jsons('asm_filter'):
        print('checking',json)
        if 'xray' not in json: # we don't support asm filtering for XRay
            assert verify_thread(load_thread(json), asm_filter_ref)
    for json in jsons('ftrace'):
        print('checking',json)
        ftrace = load_ftrace(json)
        threads = load_threads(json)
        check_ftrace(ftrace, threads)

    # funcount test
    for symcount_txt in sorted(glob.glob(f'{OUTDIR}/count.*/symcount.txt')):
        print('checking',symcount_txt)
        check_count_results(symcount_txt)

    # check last... might fail intermittently because we sleep for more than we asked for
    # due to the machine being loaded or whatever
    for json in jsons('freq'):
        print('checking',json)
        t = load_thread(json)
        assert verify_thread(t, freq_ref)
        slept = t[1][-1]-t[0][-1]
        assert slept >= 1500 and slept < 1700, f'wrong sleeping time {slept}'

def check_funtrace_from_core_dump(test):
    testdir = f'{OUTDIR}/{os.path.basename(test)}'
    # the test produces an empty trace with no samples to extract to funtrace.json
    tracejson = f'{testdir}/funtrace.json'
    assert not os.path.exists(tracejson)
    assert os.path.exists(f'{testdir}/core'), f'{testdir}/core not found - is your /proc/sys/kernel/core_pattern set to "core", and is core dump size unlimited in the shell?'

    system(f'cd {testdir} && gdb -q ../../{test} core -x ../../funtrace_gdb.py -ex funtrace -ex quit')
    system(f'./target/{TARGET}/release/funtrace2viz {testdir}/funtrace.raw {testdir}/funtrace')

    # core dump analysis should produce a sample that will be extraced to funtrace.json
    assert os.path.exists(tracejson)

    data = parse_perfetto_json(tracejson)
    threads = data['threads']
    ftrace = data['systemTraceEvents']
    assert 'sched_waking: comm=child', f'bad ftrace data:\n{ftrace}'
    
    # check that both the active and the recently finished thread were found
    assert len(threads) == 3
    assert 'child' in threads
    for thread in threads.values():
        is_main = len([name for _,name,_ in thread if name.startswith('main')]) > 0
        if is_main:
            # we're checking, in particular, that after saving a snapshot we don't have "noise" trace entries from funtrace itself
            thread = [(what,name,when) for what,name,when in thread if '_GLOBAL__' not in name and '__static_initialization_and_destruction' not in name]
        ref = killed_main_ref if is_main else killed_children_ref
        assert verify_thread(thread, ref)

def check_orphan_tracer_removal():
    def funtrace_pid(s):
        try:
            t = s.split('.')
            assert len(t) == 2 and t[0]=='funtrace'
            return int(t[1])
        except:
            return 0
    def find_tracers():
        return [f for f in glob.glob('/sys/kernel/tracing/instances/funtrace.*') if funtrace_pid(os.path.basename(f))]
    tracers = find_tracers()
    assert len(tracers) >= 4, f'expected at least 4 funtrace ftrace instances, found {len(tracers)}: {tracers}'
    print('\n'.join(['orphan tracer instances:']+tracers))

    # could be any funtrace-instrumented program - they clean orphan tracer dirs upon exit
    system(f'cd out/benchmark.pg; ../../{BUILDDIR}/benchmark.pg')
    for t in tracers:       
        pid = funtrace_pid(os.path.basename(t))
        # either the PID exists or the tracer was removed by the run of benchmark.pg
        assert os.path.exists('/proc/%d'%pid) or not os.path.exists(t)

    tracers = find_tracers()
    print('\n'.join(['orphan tracer instances:']+tracers))

if __name__ == '__main__':
    main()
