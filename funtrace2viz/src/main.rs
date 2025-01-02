use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::io::prelude::*;
use std::mem;
use bytemuck::{Pod, Zeroable};
use std::collections::{HashMap, HashSet};
use procaddr2sym::{ProcAddr2Sym, SymInfo};
use serde_json::Value;
use clap::Parser;
use std::cmp::max;
use num::{Rational64, FromPrimitive, Zero};

const RETURN_BIT: i32 = 63;
const TAILCALL_BIT: i32 = 62;
const MAGIC_LEN: usize = 8;
const LENGTH_LEN: usize = 8;


// Struct to represent a 16-byte FUNTRACE entry
#[repr(C)]
#[derive(Debug, Pod, Zeroable, Clone, Copy)]
struct FunTraceEntry {
    address: u64,
    cycle: u64,
}

struct SourceCode {
    json_str: String,
    num_lines: usize,
}

#[derive(Parser)]
#[clap(about="convert funtrace.raw to JSON files in the viztracer/vizviewer format (pip install viztracer; or use Perfetto but then you won't see source code)", version)]
struct Cli {
    #[clap(help="funtrace.raw input file with one or more trace samples")]
    functrace_raw: String,
    #[clap(help="basename.json, basename.1.json, basename.2.json... are created, one JSON file per trace sample")]
    out_basename: String,
    #[clap(short, long, help="print the static addresses and executable/shared object files of decoded functions in addition to name, file & line")]
    executable_file_info: bool,
    #[clap(short, long, help="ignore events older than this relatively to the latest recorded event in a given trace sample (very old events create the appearance of a giant blank timeline in vizviewer/Perfetto which zooms out to show the recorded timeline in full)")]
    max_event_age: Option<u64>,
    #[clap(short, long, help="ignore events older than this cycle (like --max-event-age but as a timestamp instead of an age in cycles)")]
    oldest_event_time: Option<u64>,
    #[clap(short, long, help="dry run - only list the samples & threads with basic stats, don't decode into JSON")]
    dry: bool,
    #[clap(short, long, help="ignore samples with indexes outside this list")]
    samples: Vec<u32>,
    #[clap(short, long, help="ignore threads with indexes outside this list (including for the purpose of interpreting --max-event-age)")]
    threads: Vec<u64>,
}

struct TraceConverter {
    procaddr2sym: ProcAddr2Sym,
    // we dump source code into the JSON files to make it visible in vizviewer
    source_cache: HashMap<String, SourceCode>,
    sym_cache: HashMap<u64, SymInfo>,
    max_event_age: Option<u64>,
    oldest_event_time: Option<u64>,
    dry: bool,
    samples: Vec<u32>,
    threads: Vec<u64>,
    cpu_freq: u64,
    cmd_line: String,
    first_event_in_json: bool,
    first_event_in_thread: bool,
}

#[repr(C)]
#[derive(Debug, Pod, Zeroable, Clone, Copy)]
struct ThreadID
{
    pid: u64,
    tid: u64,
    name: [u8; 16],
}

struct ThreadTrace {
    thread_id: ThreadID,
    trace: Vec<FunTraceEntry>,
}

struct FtraceEvent {
    timestamp: u64,
    line: String,
}

fn parse_ftrace_lines(input: &String, transform_timestamp: impl Fn(u64) -> String) -> Vec<FtraceEvent> {
    let mut results = Vec::new();

    for line in input.lines() {
        // Find the timestamp section
        if let Some(colon_pos) = line.find(": ") {
            // Search backwards from colon to find the start of timestamp
            if let Some(space_before_ts) = line[..colon_pos].rfind(char::is_whitespace) {
                let timestamp_str = &line[space_before_ts + 1..colon_pos];

                // Parse the timestamp
                if let Ok(timestamp) = timestamp_str.parse::<u64>() {
                    // Split line into parts
                    let before_ts = &line[..space_before_ts + 1];
                    let after_ts = &line[colon_pos..];

                    // Create modified line with transformed timestamp
                    let modified_line = format!(
                        "{}{}{}",
                        before_ts,
                        transform_timestamp(timestamp),
                        after_ts
                    );

                    results.push(FtraceEvent {
                        timestamp,
                        line: modified_line,
                    });
                }
            }
        }
    }

    results
}

fn rat2dec(rational: &Rational64, decimal_places: usize) -> String {
    // Get numerator and denominator as BigInt
    let numerator = rational.numer();
    let denominator = rational.denom();
    
    // Perform division with extra precision to ensure accuracy
    let mut quotient = numerator / denominator;
    let mut remainder = numerator % denominator;
    
    // Build the decimal string
    let mut result = quotient.to_string();
    
    if !remainder.is_zero() {
        result.push('.');
        
        // Calculate decimal digits
        for _ in 0..decimal_places {
            remainder *= 10;
            quotient = &remainder / denominator;
            remainder = &remainder % denominator;
            result.push_str(&quotient.to_string());
            
            if remainder.is_zero() {
                break;
            }
        }
    }
    
    result
}

impl TraceConverter {
    pub fn new(args: &Cli) -> Self {
        TraceConverter { procaddr2sym: ProcAddr2Sym::new(), source_cache: HashMap::new(), sym_cache: HashMap::new(),
            max_event_age: args.max_event_age, oldest_event_time: args.oldest_event_time, dry: args.dry,
            samples: args.samples.clone(), threads: args.threads.clone(), cpu_freq: 0, cmd_line: "".to_string(),
            first_event_in_json: false, first_event_in_thread: false
        }
    }

    fn oldest_event(&self, sample_entries: &Vec<ThreadTrace>, ftrace_events: &Vec<FtraceEvent>) -> u64 {
        if let Some(max_age) = self.max_event_age {
            let mut youngest = 0;
            for entries in sample_entries {
                if self.threads.is_empty() || self.threads.contains(&entries.thread_id.tid) {
                    for entry in entries.trace.iter() {
                        youngest = max(entry.cycle, youngest);
                    }
                }
            }
            for event in ftrace_events {
                youngest = max(event.timestamp, youngest);
            }
            youngest - max_age
        }
        else if let Some(oldest) = self.oldest_event_time {
            oldest
        }
        else {
            0
        }
    }

    fn write_function_call_event(&mut self, json: &mut File, call_sym: &SymInfo, call_cycle: u64, return_cycle: u64, thread_id: &ThreadID, funcset: &mut HashSet<SymInfo>) -> io::Result<()> {
        if self.dry {
            return Ok(());
        }
        if self.first_event_in_thread {
            let name: Vec<_> = thread_id.name.iter().filter(|&&x| x != 0 as u8).copied().collect();
            json.write(format!(r#"{}{{"ph":"M","pid":{},"tid":{},"name":"thread_name","args":{{"name":{}}}}}"#,
                        if self.first_event_in_json { "" } else { "\n," },
                        thread_id.pid,thread_id.tid,Value::String(String::from_utf8(name).unwrap()).to_string()).as_bytes())?;
            self.first_event_in_thread = false;
            self.first_event_in_json = false;

            if thread_id.pid == thread_id.tid {
                json.write(format!(r#"{}{{"ph":"M","pid":{},"tid":{},"name":"process_name","args":{{"name":{}}}}}"#, "\n,",
                        thread_id.pid,thread_id.tid,Value::String(self.cmd_line.clone()).to_string()).as_bytes())?;
            }
        }
        //using f64 would lose precision for machines with an uptime > month since f64 stores
        //52 mantissa bits and TSC increments a couple billion times per second.
        //we use rational numbers instead
        let rat = |n: u64| Rational64::from_u64(n).unwrap();
        let cycles_per_us = rat(self.cpu_freq) / rat(1000000);

        let digits = 4; //Perfetto timeline has nanosecond precision - no point in printing
        //more digits than 4 for the microsecond timestamps it expects in the JSON

        // the redundant ph:X is needed to render the event on Perfetto's timeline
        json.write(format!(r#"{}{{"tid":{},"ts":{},"dur":{},"name":{},"ph":"X","pid":{}}}"#, "\n,",
                    thread_id.tid, rat2dec(&(rat(call_cycle)/cycles_per_us.clone()), digits), rat2dec(&(rat(return_cycle-call_cycle)/cycles_per_us), digits), json_name(call_sym), thread_id.pid).as_bytes())?; 
    
        funcset.insert(call_sym.clone());
    
        //cache the source code if it's the first time we see this file
        if !self.source_cache.contains_key(&call_sym.file) {
            let mut source_code: Vec<u8> = Vec::new();
            if let Ok(mut source_file) = File::open(&call_sym.file) {
                source_file.read_to_end(&mut source_code)?;
            }
            let json_str = Value::String(String::from_utf8(source_code.clone()).unwrap()).to_string();
            let num_lines = source_code.iter().filter(|&&b| b == b'\n').count(); //TODO: num newlines
            //might be off by one relatively to num lines...
            self.source_cache.insert(call_sym.file.clone(), SourceCode{ json_str, num_lines });
        }
        Ok(())
    }

    fn write_sample_to_json(&mut self, fname: &String, sample_entries: &Vec<ThreadTrace>, ftrace_text: &String) -> io::Result<()> {
        let mut json = if self.dry { File::open("/dev/null")? } else { File::create(fname)? };
        if !self.dry {
            json.write(br#"{
"traceEvents": [
"#)?;
            println!("decoding a trace sample logged by `{}` into {}...", self.cmd_line, fname);
        }
        else {
            println!("inspecting sample {} logged by `{}` (without creating the file...)", fname, self.cmd_line);
        }
    
        // we list the set of functions (to tell their file, line pair to vizviewer);
        // we also use this set to only dump the relevant part of the source cache to each
        // json (the source cache persists across samples/jsons but not all files are relevant
        // to all samples)
        let mut funcset: HashSet<SymInfo> = HashSet::new();
        self.first_event_in_json = true;
        let mut ignore_addrs: HashSet<u64> = HashSet::new();
    
        let rat = |n: u64| Rational64::from_u64(n).unwrap();
        //ftrace timestamps are supposed to be in seconds; CPU frequency is in TSC cycles per second;
        //so dividing by frequency will convert TSC to seconds. Perfetto timeline accuracy is ns
        //hence 10 digits after '.'
        let cycles_per_second = rat(self.cpu_freq);
        let fixts = |ts: u64| format!("{}", rat2dec(&(rat(ts)/cycles_per_second), 10));
        let mut ftrace_events = parse_ftrace_lines(ftrace_text, fixts);

        let oldest = self.oldest_event(sample_entries, &ftrace_events);

        ftrace_events.retain(|event| event.timestamp >= oldest);
    
        let cycles_per_ns = (self.cpu_freq as f64 / 1000000000.0 + 1.) as u64;
    
        for thread_trace in sample_entries {
            let entries = &thread_trace.trace;
            if !self.threads.is_empty() && !self.threads.contains(&thread_trace.thread_id.tid) {
                println!("ignoring thread {} - not on the list {:?}", thread_trace.thread_id.tid, self.threads);
                continue;
            }
            let mut stack: Vec<FunTraceEntry> = Vec::new();
            let mut num_events = 0;
            let earliest_cycle = max(entries[0].cycle, oldest);
            let latest_cycle = entries[entries.len()-1].cycle;
            let mut num_orphan_returns = 0;
            self.first_event_in_thread = true;
    
            for entry in entries {
                if oldest > entry.cycle {
                    continue; //ignore old events
                }
                let ret = (entry.address >> RETURN_BIT) != 0;
                let tailcall = ((entry.address >> TAILCALL_BIT) & 1) != 0;
                let addr = entry.address & !((1<<RETURN_BIT) | (1<<TAILCALL_BIT));
                if !self.sym_cache.contains_key(&addr) {
                    let sym = self.procaddr2sym.proc_addr2sym(addr);
                    //we ignore "virtual override thunks" because they aren't interesting
                    //to the user, and what's more, some of them call __return__ but not
                    //__fentry__ under -pg, so you get spurious "orphan returns" (see below
                    //how we handle supposedly "real" orphan returns.)
                    if sym.demangled_func.contains("virtual override thunk") {
                        ignore_addrs.insert(addr);
                    }
                    self.sym_cache.insert(addr, sym);
                }
                if ignore_addrs.contains(&addr) {
                    continue;
                }
                //println!("ret {} sym {}", ret, json_name(sym_cache.get(&addr).unwrap()));
                if !ret && !tailcall {
                    stack.push(*entry);
                }
                else {
                    //write an event to json
                    let (call_cycle, call_addr) = if stack.is_empty() {
                        num_orphan_returns += 1;
                        // a return without a call - the call event must have been overwritten
                        // in the cyclic trace buffer; fake a call at the start of the trace
                        //
                        // the "-num_orphan_returns" is here because vizviewer / Perfetto is
                        // thrown off by multiple calls starting at the same cycle and puts
                        // them in the wrong lane on the timeline (the JSON spec requires perfect
                        // nesting of events). we multiply by cycles_per_ns
                        // since Perfetto works at nanosecond accuracy and will treat timestamps
                        // within the same ns as identical, the very thing we're trying to avoid
                        (earliest_cycle - num_orphan_returns * cycles_per_ns, addr)
                    }
                    else {
                        let call_entry = stack.pop().unwrap();
                        if (call_entry.address & (1<<TAILCALL_BIT)) != 0 {
                            println!("WARNING: a non-orphan tailcall popped from the stack, expected a call?..");
                        }
                        (call_entry.cycle, call_entry.address)
                    };
                    if tailcall {
                        //a tailcall from f to g means we won't ever see a return event from f - instead,
                        //f jumps to g, and g returns straight to f's caller. when g returns to f's caller
                        //(or when h tail-called by g returns to f's caller), we'll emit an event
                        //returning from f, too. for this, we push an entry to the stack with TAILCALL_BIT set -
                        //basically the same entry we just popped (f's), but with the bit set - essentially
                        //we're "postponing its return for later."

                        //either call addr is this entry's address (an orphan tail call) or we
                        //popped it from the stack in which case it should have been a call, not a tailcall
                        stack.push(FunTraceEntry{cycle: call_cycle, address: call_addr | (1<<TAILCALL_BIT)});
                        continue;
                    }
                    let call_sym = self.sym_cache.get(&call_addr).unwrap();
                    //warn if we return to a different function from the one predicted by the call stack.
                    //this "shouldn't happen" but it does unless we ignore "virtual override thunks"
                    //and it's good to at least emit a warning when it does since the trace will look strange
                    let ret_sym = self.sym_cache.get(&addr).unwrap();
                    //FIXME: adapt the warning for XRay
                    if false && ret_sym.static_addr != call_sym.static_addr {
                        println!("      WARNING: call/return mismatch - {} called, {} returning", json_name(call_sym), json_name(ret_sym));
                        println!("      call stack after the return:");
                        for entry in stack.clone() {
                            println!("        {}", json_name(self.sym_cache.get(&entry.address).unwrap()));
                        }
                    }
                    //this is a return - keep popping from the stack until we find a call,
                    //handle popped tail calls as returns from the called function
                    num_events += 1;
                    self.write_function_call_event(&mut json, &call_sym.clone(), call_cycle, entry.cycle, &thread_trace.thread_id, &mut funcset)?;
                    let mut returns = 1;
                    while !stack.is_empty() && (stack.last().unwrap().address & (1<<TAILCALL_BIT)) != 0 {
                        let last = stack.pop().unwrap();
                        let tailcall_addr = last.address & !(1<<TAILCALL_BIT);
                        let call_sym = self.sym_cache.get(&tailcall_addr).unwrap();
                        num_events += 1;
                        //we (or rather the Perfetto JSON spec) want perfect nesting, so we make sure there's an ns latency between the timestmaps
                        //of returns from tailcalls (though eg XRay doesn't bother and have them return at the same cycle.) hope we won't
                        //have too many returns from tailcalls for a return timestamp to exceed a next actual event timestamp...
                        self.write_function_call_event(&mut json, &call_sym.clone(), last.cycle, entry.cycle + cycles_per_ns*returns, &thread_trace.thread_id, &mut funcset)?;
                        returns += 1;
                    }

                }
            }
            let name = String::from_utf8(thread_trace.thread_id.name.iter().filter(|&&x| x != 0 as u8).copied().collect()).unwrap();
            if latest_cycle >= earliest_cycle {
                println!("  thread {} {} - {} recent function calls logged over {} cycles [{} - {}]", thread_trace.thread_id.tid, name, num_events, latest_cycle-earliest_cycle, earliest_cycle, latest_cycle);
            }
            else {
                println!("    skipping thread {} {} (all {} logged function entry/return events are too old)", thread_trace.thread_id.tid, name, entries.len());
            }
        }
        if self.dry {
            return Ok(())
        }
    
        json.write(b"],\n")?;

        if !ftrace_events.is_empty() {
            let joined: String = ftrace_events.iter().map(|e| e.line.clone() + "\n").collect();

            json.write(br#""systemTraceEvents": "#)?;
            //# tracer: nop is something Perfetto doesn't seem to need but the Chromium trace
            //JSON spec insists is a must
            json.write(Value::String("# tracer: nop\n".to_string() + &joined).to_string().as_bytes())?;
            json.write(b",\n")?;

            let oldest_ftrace = ftrace_events[0].timestamp;
            let newest_ftrace = ftrace_events[ftrace_events.len()-1].timestamp;
            println!("  ftrace - {} events logged over {} cycles [{} - {}]", ftrace_events.len(), newest_ftrace-oldest_ftrace, oldest_ftrace, newest_ftrace);
        }

        // find the source files containing the functions in this sample's set
        let mut fileset: HashSet<String> = HashSet::new();
        for sym in funcset.iter() {
            fileset.insert(sym.file.clone());
        }
        json.write(br#""viztracer_metadata": {
  "version": "0.16.3",
  "overflow": false,
  "producer": "funtrace2viz"
},
"file_info": {
"files": {
"#)?;
    
        // dump the source code of these files into the json
        for (i, file) in fileset.iter().enumerate() {
            if let Some(&ref source_code) = self.source_cache.get(file) {
                json.write(Value::String(file.clone()).to_string().as_bytes())?;
                json.write(b":[")?;
                json.write(source_code.json_str.as_bytes())?;
                json.write(b",")?;
                json.write(format!("{}", source_code.num_lines).as_bytes())?;
                json.write(if i==fileset.len()-1 { b"]\n" } else { b"],\n" })?;
            }
        }
        json.write(br#"},
"functions": {
"#)?;
    
        // tell where each function is defined
        for (i, sym) in funcset.iter().enumerate() {
            // line-3 is there to show the function prototype in vizviewer/Perfetto
            // (often the debug info puts the line at the opening { of a function
            // and then the prototype is not seen, it can also span a few lines)
            json.write(format!("{}:[{},{}]{}\n", json_name(sym), Value::String(sym.file.clone()).to_string(), if sym.line <= 3 { sym.line } else { sym.line-3 }, if i==funcset.len()-1 { "" } else { "," }).as_bytes())?;
        }
        json.write(b"}}}\n")?;
    
        Ok(())
    }

    pub fn parse_chunks(&mut self, file_path: &String, json_basename: &String) -> io::Result<()> {
        let mut file = File::open(file_path)?;
    
        let mut sample_entries: Vec<ThreadTrace> = Vec::new();
        let mut num_json = 0;

        let mut thread_id = ThreadID { pid: 0, tid: 0, name: [0; 16] };

        let mut ftrace_text = "".to_string();

        loop {
            //the file consists of chunks with an 8-byte magic string telling the chunk
            //type, followed by an 8-byte length field and then contents of that length
            let mut magic = [0u8; MAGIC_LEN];
            if file.read_exact(&mut magic).is_err() {
                break; // End of file
            }
    
            let mut length_bytes = [0u8; LENGTH_LEN];
            file.read_exact(&mut length_bytes)?;
            let chunk_length = usize::from_ne_bytes(length_bytes);
    
            if &magic == b"FUNTRACE" {
                if chunk_length != 8 {
                    println!("warning: unexpected length {} for FUNTRACE chunk", chunk_length);
                    file.seek(SeekFrom::Current(chunk_length as i64))?;
                    continue;
                }
                let mut freq_bytes = [0u8; 8];
                file.read_exact(&mut freq_bytes)?;
                self.cpu_freq = u64::from_ne_bytes(freq_bytes);
            }
            else if &magic == b"CMD LINE" {
                let mut cmd_bytes = vec![0u8; chunk_length];
                file.read_exact(&mut cmd_bytes)?;
                self.cmd_line = String::from_utf8(cmd_bytes).unwrap();
            }
            else if &magic == b"ENDTRACE" {
                if chunk_length != 0 {
                    println!("warning: non-zero length for ENDTRACE chunk");
                    file.seek(SeekFrom::Current(chunk_length as i64))?;
                    continue;
                }
                if !sample_entries.is_empty() || !ftrace_text.is_empty() {
                    if self.samples.is_empty() || self.samples.contains(&num_json) {
                        self.write_sample_to_json(&format_json_filename(json_basename, num_json), &sample_entries, &ftrace_text)?;
                    }
                    else {
                        println!("ignoring sample {} - not on the list {:?}", num_json, self.samples);
                    }
                    num_json += 1;
                    sample_entries.clear();
                    ftrace_text.clear();
                }
            }
            else if &magic == b"PROCMAPS" {
                //the content of the dumping process's /proc/self/maps to use when
                //interpreting the next trace samples (until another PROCMAPS chunk is encountered)
                let mut chunk_content = vec![0u8; chunk_length];
                file.read_exact(&mut chunk_content)?;
                self.procaddr2sym.set_proc_maps(chunk_content.as_slice());
                //the symbol cache might have been invalidated if the process unloaded and reloaded a shared object
                self.sym_cache = HashMap::new();
            } else if &magic == b"THREADID" {
                if chunk_length != std::mem::size_of::<ThreadID>() {
                    println!("Unexpected THREAD chunk length {} - expecting {}", chunk_length, std::mem::size_of::<ThreadID>());
                    file.seek(SeekFrom::Current(chunk_length as i64))?;
                    continue;
                }

                file.read_exact(bytemuck::bytes_of_mut(&mut thread_id))?;
            } else if &magic == b"TRACEBUF" {
                if chunk_length % mem::size_of::<FunTraceEntry>() != 0 {
                    println!("Invalid TRACEBUF chunk length {} - must be a multiple of {}", chunk_length, mem::size_of::<FunTraceEntry>());
                    file.seek(SeekFrom::Current(chunk_length as i64))?;
                    continue;
                }
    
                let num_entries = chunk_length / mem::size_of::<FunTraceEntry>();
                let mut entries = ThreadTrace { thread_id, trace: vec![FunTraceEntry { address: 0, cycle: 0 }; num_entries] };
                file.read_exact(bytemuck::cast_slice_mut(&mut entries.trace))?;
                entries.trace.retain(|&entry| !(entry.cycle == 0 && entry.address == 0));
                if !entries.trace.is_empty() {
                    entries.trace.sort_by_key(|entry| entry.cycle);
                    sample_entries.push(entries);
                }
            } else if &magic == b"FTRACETX" {
                let mut ftrace_bytes = vec![0u8; chunk_length];
                file.read_exact(&mut ftrace_bytes)?;
                ftrace_text = String::from_utf8(ftrace_bytes).unwrap();
            } else {
                println!("Unknown chunk type: {:?}", std::str::from_utf8(&magic).unwrap_or("<invalid>"));
                file.seek(SeekFrom::Current(chunk_length as i64))?;
            }
        }
        if !sample_entries.is_empty() || !ftrace_text.is_empty() {
            println!("warning: FUNTRACE block not closed by ENDTRACE");
            self.write_sample_to_json(&format_json_filename(json_basename, num_json), &sample_entries, &ftrace_text)?;
        }
    
        Ok(())
    }
}

fn format_json_filename(basename: &String, number: u32) -> String {
    if number > 0 {
        format!("{}.{}.json", basename, number)
    } else {
        format!("{}.json", basename)
    }
}

static mut PRINT_BIN_INFO: bool = false;

fn json_name(sym: &SymInfo) -> String {
    //"unsafe" access to a config parameter... I guess I should have put stuff into a struct and have
    //most methods operate on it to make it prettier or something?..
    let print_bin_info = unsafe { PRINT_BIN_INFO };
    if print_bin_info {
        Value::String(format!("{} ({}:{} {:#x}@{})", sym.demangled_func, sym.file, sym.line, sym.static_addr, sym.executable_file)).to_string()
    }
    else {
        Value::String(format!("{} ({}:{})", sym.demangled_func, sym.file, sym.line)).to_string()
    }
}

fn main() -> io::Result<()> {
    let args = Cli::parse();
    if args.max_event_age.is_some() && args.oldest_event_time.is_some() {
        panic!("both --max-event-age and --oldest-event-time specified - choose one");
    }
    unsafe {
        PRINT_BIN_INFO = args.executable_file_info;
    }
    let mut convert = TraceConverter::new(&args);
    convert.parse_chunks(&args.functrace_raw, &args.out_basename)
}

