# `info proc mappings` format:
#         Start Addr           End Addr       Size     Offset objfile
#     0x555555554000     0x555555556000     0x2000        0x0 /path/to/file
#
# /proc/self/maps format - start-end permissions offset device inode /path/to/file 
# 7f74a4ae6000-7f74a4b08000 r--p 00000000 103:07 109578392                 /usr/lib/x86_64-linux-gnu/libc-2.31.so

import gdb, struct, traceback

def write_chunk(f, magic, content):
    assert len(magic)==8
    f.write(magic)
    f.write(struct.pack('Q', len(content)))
    f.write(content)

def write_proc_maps(f):
    mappings = gdb.execute('info proc mappings', from_tty=False, to_string=True)
 
    proc_maps = b''
    for line in mappings.strip().split('\n'):
        line = line.strip()
        if line.startswith('0x'):
            t = line.split()
            if len(t) == 5: # we don't care about unnamed segments
                start, end, size, offset, path = line.split()
                # we don't care about permissions, device and inode
                proc_maps += b'%10x-%10x r-xp %08x 0:0 0 %s\n'%(int(start,16), int(end,16), int(offset,16), bytes(path,encoding='utf-8'))
 
    print('funtrace: saving proc mappings')
    write_chunk(f, b'PROCMAPS', proc_maps)

def get_vector_elements(v):
    vis = gdb.default_visualizer(v)
    if vis:
        return [elem for _,elem in list(vis.children())]
    else: # no pretty printers - assume we know the representation
        start = v['_M_impl']['_M_start']
        finish = v['_M_impl']['_M_finish']

        return [start[i] for i in range(finish-start)] 

def write_funtrace(f):
    write_chunk(f, b'FUNTRACE', b'')
    thread_traces = get_vector_elements(gdb.parse_and_eval('g_trace_state.thread_traces'))
    buf_size = int(gdb.parse_and_eval('funtrace_buf_size'))
    for i, trace in enumerate(thread_traces):
        buf = trace.dereference()['buf']
        data = bytes(gdb.selected_inferior().read_memory(buf, buf_size))
        print(f'funtrace: thread {i+1} - saving {buf_size} bytes of data read from {buf}')
        write_chunk(f, b'TRACEBUF', data)
    write_chunk(f, b'ENDTRACE', b'')

class FuntraceCmd(gdb.Command):
    '''prints the content of the funtrace event buffers into ./funtrace.raw

you can then decode that file using funtrace2viz and open the output JSON files with vizviewer
(installed by `pip install viztracer`) or Perfetto (https://ui.perfetto.dev, click
"Open with legacy UI" - no source access unlike in vizviewer but otherwise should work)'''

    def __init__(self):
        super(FuntraceCmd, self).__init__("funtrace", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        try:
            with open('funtrace.raw', 'wb') as f:
                write_proc_maps(f)
                write_funtrace(f)
        except:
            traceback.print_exc()
            raise

FuntraceCmd()
