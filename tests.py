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
    timestamps = set()
    for event in events:
        phase = event['ph']
        tid = event['tid']
        name = event['name']
        if phase == 'M': # metadata
            if name == 'thread_name':
                thread_names[tid] = event['args']['name']
            continue
        timepoints = threads.setdefault(thread_names[tid], list())
        timestamp = event['ts']

        assert timestamp not in timestamps, 'expecting unique timestamps in every thread!'
        timestamps.add(timestamp)

        if phase == 'B': # begin timepoint
            timepoints.append((call, name, timestamp))
        elif phase == 'X': # complete event
            duration = event['dur']
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

exceptions_ref = [
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
]*3

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

def system(cmd):
    print('running',cmd)
    status = os.system(cmd)
    assert status==0, f'`{cmd}` failed with status {status}'

BUILDDIR = './built-tests'
OUTDIR = './out'

def build_trace_analysis_tools():
    system('cargo build -r')

def run_cmds(cmds):
    for cmd in cmds:
        system(cmd)

def build_cxx_test(main, shared=[]):
    cmdlists = []
    binaries = {}
    for mode in ['finstr','pg','xray']:
        CXXFLAGS="-O3 -std=c++11 -Wall"
        if mode == 'xray':
            CXXFLAGS += " -fxray-instruction-threshold=1"
        compiler = 'clang++' if mode=='xray' else 'g++'
        CXX = f'./compiler-wrappers/funtrace-{mode}-{compiler}'
        test = main.split('.')[0]
        binary = f'{BUILDDIR}/{test}.{mode}'
        cmds = [
            f'{CXX} -c tests/{main} -o {BUILDDIR}/{test}.{mode}.o {CXXFLAGS} -I.',
            f'{CXX} -o {binary} {BUILDDIR}/{test}.{mode}.o {CXXFLAGS}'
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
            f'cargo run -r --bin funtrace2viz {OUTDIR}/{name}/funtrace.raw {OUTDIR}/{name}/funtrace > {OUTDIR}/{name}/f2v.out'
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
    def buildcmds(main,shared=[]):
        c,b = build_cxx_test(main,shared)
        cmdlists.extend(c)
        test2bins.update(b)

    buildcmds('exceptions.cpp')
    buildcmds('longjmp.cpp')
    buildcmds('tailcall.cpp')
    buildcmds('orphans.cpp')
    pool.map(run_cmds, cmdlists)

    cmdlists = []
    for test,binaries in test2bins.items():
        cmdlists.extend(run_cxx_test(test,binaries))
    pool.map(run_cmds, cmdlists)

    print('checking results...')

    def load_thread(json):
        return list(parse_perfetto_json(json)['threads'].values())[0]

    def jsons(test): return sorted(glob.glob(f'./out/{test}.*/funtrace.json'))

    for json in jsons('exceptions'):
        print('checking',json)
        assert verify_thread(load_thread(json), exceptions_ref)
    for json in jsons('longjmp'): 
        print('checking',json)
        assert verify_thread(load_thread(json), longjmp_ref)
    for json in jsons('orphans'): 
        print('checking',json)
        assert verify_thread(load_thread(json), orphans_ref(json))

if __name__ == '__main__':
    main()
