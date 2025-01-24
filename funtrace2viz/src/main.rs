use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::io::prelude::*;
use std::mem;
use bytemuck::{Pod, Zeroable};
use std::collections::{HashMap, HashSet};
use procaddr2sym::{ProcAddr2Sym, SymInfo};
use serde_json::Value;
use clap::Parser;
use std::cmp::{min, max};
use num::{FromPrimitive, Zero};
use num::rational::Ratio;
use num::bigint::BigInt;

const RETURN_BIT: i32 = 63;
const RETURN_WITH_CALLER_ADDRESS_BIT: i32 = 62;
const CATCH_MASK: u64 = (1<<RETURN_BIT)|(1<<RETURN_WITH_CALLER_ADDRESS_BIT);
const CALL_RETURNING_UPON_THROW_BIT: i32 = 61;
const ADDRESS_MASK: u64 = !(CATCH_MASK | (1<<CALL_RETURNING_UPON_THROW_BIT));
const MAGIC_LEN: usize = 8;
const LENGTH_LEN: usize = 8;

fn bit_set(n: u64, b: i32) -> bool { ((n>>b)&1) != 0 }

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
    funtrace_raw: String,
    #[clap(help="basename.json, basename.1.json, basename.2.json... are created, one JSON file per trace sample")]
    out_basename: String,
    #[clap(short, long, help="print the static addresses and executable/shared object files of decoded functions in addition to name, file & line")]
    executable_file_info: bool,
    #[clap(short, long, help="print the raw timestamps (the default is to subtract the timestamp of the earliest reported event at each sample, so that time starts at 0; in particular it helps to avoid rounding issues you might see with large timestamp values)")]
    raw_timestamps: bool,
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
    raw_timestamps: bool,
    time_base: u64,
    oldest_event_time: Option<u64>,
    dry: bool,
    samples: Vec<u32>,
    threads: Vec<u64>,
    cpu_freq: u64,
    cmd_line: String,
    first_event_in_json: bool,
    first_event_in_thread: bool,
    num_events: i64,
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

fn rat2dec(rat: &Ratio<BigInt>, decimal_places: u32) -> String {
    let mut result = "".to_string();
    let mut rational = rat.clone();
    if rat < &Ratio::from_u64(0).unwrap() { //shouldn't happen in this program but let's print correctly if it does
        rational = -rat;
        result = "-".to_string();
    }
    // Round - add 0.0..05
    let rounded = rational + Ratio::from_u64(5).unwrap() / Ratio::from_u64(10u64.pow(decimal_places+1)).unwrap();

    // Get numerator and denominator
    let numerator = rounded.numer();
    let denominator = rounded.denom();
    
    // Perform division with extra precision to ensure accuracy
    let mut quotient = numerator / denominator;
    let mut remainder = numerator % denominator;
    
    // Build the decimal string
    result = result + &quotient.to_string();
    
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
            max_event_age: args.max_event_age, raw_timestamps: args.raw_timestamps, time_base: 0,
            oldest_event_time: args.oldest_event_time, dry: args.dry,
            samples: args.samples.clone(), threads: args.threads.clone(), cpu_freq: 0, cmd_line: "".to_string(),
            first_event_in_json: false, first_event_in_thread: false, num_events: 0
        }
    }

    fn oldest_event(&self, sample_entries: &Vec<ThreadTrace>, ftrace_events: &Vec<FtraceEvent>) -> u64 {
        let mut youngest = 0;
        let mut oldest = u64::MAX;
        for entries in sample_entries {
            if self.threads.is_empty() || self.threads.contains(&entries.thread_id.tid) {
                oldest = min(entries.trace.first().unwrap().cycle, oldest);
                youngest = max(entries.trace.last().unwrap().cycle, youngest);
            }
        }
        if !ftrace_events.is_empty() {
            oldest = min(ftrace_events.first().unwrap().timestamp, oldest);
            youngest = max(ftrace_events.last().unwrap().timestamp, youngest);
        }
        if let Some(max_age) = self.max_event_age {
            youngest - max_age
        }
        else if let Some(oldest_to_report) = self.oldest_event_time {
            oldest_to_report
        }
        else {
            oldest
        }
    }

    //extra_ns shifts the return timestamp if positive or the call timestamp if negative
    fn write_function_call_event(&mut self, json: &mut File, call_sym: &SymInfo, call_cycle: u64, return_cycle: u64, extra_ns: i32, thread_id: &ThreadID, funcset: &mut HashSet<SymInfo>) -> io::Result<()> {
        self.num_events += 1;
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
        let rat = |n: u64| Ratio::from_u64(n).unwrap();
        let cycles_per_us = rat(self.cpu_freq) / rat(1000000);

        let (extra_ret, extra_call) = if extra_ns > 0 {
            (rat(extra_ns as u64) / rat(1000), rat(0))
        }
        else {
            (rat(0), rat(-extra_ns as u64) / rat(1000))
        };

        let digits = 4; //Perfetto timeline has nanosecond precision - no point in printing
        //more digits than 3 for the microsecond timestamps it expects in the JSON; we print 4
        //for testing to make sure that cycles don't round to the same ns that should be distinct events

        if return_cycle != 0 && call_cycle != 0 { // a "complete" event (ph:X); these needn't be sorted by timestamp
            //note that we could have used the B and E events for "incomplete" function calls missing a call
            //or a return timestamp. however, the last orphan B event seems to be missing from Perfetto's rendering
            //and all of the orphan E events seem to be missing; B and E are apparently mostly designed to come in pairs
            //(despite the beautiful gradient that orphan B events are rendered with)
            json.write(format!(r#"{}{{"tid":{},"ts":{},"dur":{},"name":{},"ph":"X","pid":{}}}"#, "\n,",
                        thread_id.tid,
                        rat2dec(&(rat(call_cycle-self.time_base)/cycles_per_us.clone() - extra_call.clone()), digits),
                        rat2dec(&(rat(return_cycle-call_cycle)/cycles_per_us + extra_call + extra_ret), digits),
                        json_name(call_sym), thread_id.pid).as_bytes())?; 
        }    

        funcset.insert(call_sym.clone());
    
        //cache the source code if it's the first time we see this file
        if !self.source_cache.contains_key(&call_sym.file) {
            let mut source_code: Vec<u8> = Vec::new();
            if let Ok(mut source_file) = File::open(&call_sym.file) {
                source_file.read_to_end(&mut source_code)?;
            }
            else if call_sym.file != "??" {
                println!("WARNING: couldn't open source file {} - you can remap paths using a substitute-path.json file in your working directory", call_sym.file);
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
            println!("decoding a trace sample logged by `{}` into {} ...", self.cmd_line, fname);
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
    
        let rat = |n: u64| Ratio::from_u64(n).unwrap();
        //ftrace timestamps are supposed to be in seconds; CPU frequency is in TSC cycles per second;
        //so dividing by frequency will convert TSC to seconds. Perfetto timeline accuracy is ns
        //hence 10 digits after '.' (9 plus another to make sure different cycles don't become the same ns)
        let cycles_per_second = rat(self.cpu_freq);
        let fixts = |ts: u64| format!("{}", rat2dec(&(rat(ts)/cycles_per_second.clone()), 10));
        let mut ftrace_events = parse_ftrace_lines(ftrace_text, fixts);

        let oldest = self.oldest_event(sample_entries, &ftrace_events);
        self.time_base = if self.raw_timestamps { 0 } else { oldest };

        if self.time_base > 0 {
            //TODO: a bit wasteful to reparse this just to subtract the time base 
            let fixts = |ts: u64| format!("{}", rat2dec(&(rat(ts-self.time_base)/cycles_per_second.clone()), 10));
            ftrace_events = parse_ftrace_lines(ftrace_text, fixts);
        }
    
        ftrace_events.retain(|event| event.timestamp >= oldest);

        for thread_trace in sample_entries {
            let entries = &thread_trace.trace;
            if !self.threads.is_empty() && !self.threads.contains(&thread_trace.thread_id.tid) {
                println!("ignoring thread {} - not on the list {:?}", thread_trace.thread_id.tid, self.threads);
                continue;
            }
            let mut stack: Vec<FunTraceEntry> = Vec::new();
            self.num_events = 0;
            let earliest_cycle = max(entries[0].cycle, oldest);
            let latest_cycle = entries[entries.len()-1].cycle;
            let mut num_orphan_returns = 0;
            self.first_event_in_thread = true;

            let mut expecting_to_return_into_sym = self.procaddr2sym.unknown_symbol();
    
            for entry in entries {
                if oldest > entry.cycle {
                    continue; //ignore old events
                }
                let catch = (entry.address & CATCH_MASK) == CATCH_MASK;
                let ret_with_caller_addr = bit_set(entry.address, RETURN_WITH_CALLER_ADDRESS_BIT) && !catch;
                let ret = (bit_set(entry.address, RETURN_BIT) || ret_with_caller_addr) && !catch;
                let addr = entry.address & ADDRESS_MASK;

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
                //println!("{} {} sym {}", stack.len(), if catch { "catch" } else if ret { "ret" } else { "call" }, json_name(self.sym_cache.get(&addr).unwrap()));
                if catch {
                    //pop the entries on the stack until we find the function which logged the catch entry.
                    //if we don't find it, perhaps its call entry didn't make it into our trace, or, more
                    //troublingly, it was compiled without instrumentation or something else went wrong which
                    //will cause us to pop everything from the stack. but resetting the stack upon a catch
                    //is probably less bad than leaving it as is since then it would keep growing with
                    //every catch
                    //
                    //TODO: we could probably improve the handling of "uninstrumented catchers" by keeping
                    //a history of the fully-popped stacks and then when a return arrives of a function
                    //in one of these stacks that was "orphaned" by the throw/catch, we could find its call
                    //entry in this history and reconstruct the call sequence. this could be done given demand;
                    //ATM we just advise against compiling "catchers" without instrumentation. [note that
                    //the improvement above would work some of the time but not always, eg because a return
                    //of any of the catcher's caller wasn't traced, either because it didn't happen or
                    //because the callers of the catcher were also uninstrumented - and this isn't a far-fetched
                    //scenario, eg if you have some loop with the top-level code catching exceptions,
                    //it might be running "indefinitely" so you won't see a return that would trigger the
                    //logic above. so advising against uninstrumented catchers
                    //will remain valid even if we add all the logic described above.]
                    let catcher = self.sym_cache.get(&addr).unwrap().demangled_func.clone();
                    let mut unwound = 0;
                    while !stack.is_empty() {
                        let last = stack.last().unwrap();
                        if bit_set(last.address, CALL_RETURNING_UPON_THROW_BIT) {
                            //this was traced with -finstrument-functions or "something" that would have
                            //recorded a return event had it been returned from due to stack unwinding
                            break;
                        }
                        let call_sym = self.sym_cache.get(&(last.address & ADDRESS_MASK)).unwrap();
                        if catcher == call_sym.demangled_func { //we don't compare by address since it could be two
                            //different symbols - we entered "f(int)" and we are catching inside "f(int) [clone .cold]";
                            //procaddr2sym strips the [clone...] from the name so we can compare by it
                            break;
                        }
                        //these all end at the same cycle contrary to the JSON spec's perfect nesting requirement;
                        //unlike XRay we try to make them stand apart by 1 ns (the timeline's precision), also makes testing more straightforward
                        unwound += 1;
                        self.write_function_call_event(&mut json, &call_sym.clone(), last.cycle, entry.cycle, unwound, &thread_trace.thread_id, &mut funcset)?;
                        stack.pop();
                    }
                    continue;
                }
                if !ret {
                    stack.push(*entry);
                }
                else {
                    let ret_sym = self.sym_cache.get(&addr).unwrap().clone();

                    if stack.is_empty() { //an "orphan return" - the call wasn't in the trace
                        num_orphan_returns += 1;
                        //if ret_with_caller_addr, record the return into the function we're expecting to return into (might be unknown
                        //or we could know by getting a previous return event with the caller's address)
                        let sym = if ret_with_caller_addr { &expecting_to_return_into_sym } else { &ret_sym };
                        self.write_function_call_event(&mut json, sym, earliest_cycle, entry.cycle, -num_orphan_returns, &thread_trace.thread_id, &mut funcset)?;
                        if ret_with_caller_addr {
                            expecting_to_return_into_sym = ret_sym.clone();
                        }
                        continue;
                    }
                    if ret_with_caller_addr {
                        //this might be useful if we get an orphan return next
                        expecting_to_return_into_sym = ret_sym.clone();
                    }

                    let call_entry = stack.pop().unwrap();
                    let mut call_cycle = call_entry.cycle;
                    let mut call_sym = self.sym_cache.get(&(call_entry.address & ADDRESS_MASK)).unwrap().clone();
                    //warn if we return to a different function from the one predicted by the call stack.
                    //this "shouldn't happen" but it does unless we ignore "virtual override thunks"
                    //and it's good to at least emit a warning when it does since the trace will look strange
                    
                    //warn if we're returning to a function different than predicted by the call stack,
                    //and try to recover from the problem by popping from the stack until we find right function
                    //(eg setjmp/longjmp can cause this problem).
                    let mut returns = 0;
                    if !ret_with_caller_addr {
                        //comparing names instead of addresses because of the [clone ...] business - not sure if we can
                        //call one clone and return into another but who knows, certainly catch returns to another clone at times
                        if ret_sym.demangled_func != call_sym.demangled_func {
                            println!("      WARNING: call/return mismatch - {} popped from the stack but {} returning", json_name(&call_sym), json_name(&ret_sym));
                            let mut found = false;
                            while !found {
                                self.write_function_call_event(&mut json, &call_sym.clone(), call_cycle, entry.cycle, returns, &thread_trace.thread_id, &mut funcset)?;
                                if stack.is_empty() {
                                    break;
                                }
                                let last = stack.last().unwrap();
                                call_sym = self.sym_cache.get(&(last.address & ADDRESS_MASK)).unwrap().clone();
                                call_cycle = last.cycle;
                                println!("        WARNING: popping {}", json_name(&call_sym));
                                stack.pop();
                                returns += 1;
                                found = ret_sym.demangled_func == call_sym.demangled_func;
                            }
                        }
                    }
                    else if !stack.is_empty() {
                        let ret_caller_sym = self.sym_cache.get(&(stack.last().unwrap().address & ADDRESS_MASK)).unwrap();
                        if ret_sym.demangled_func != ret_caller_sym.demangled_func && stack.iter().any(|&entry| self.sym_cache.get(&(entry.address & ADDRESS_MASK)).unwrap().demangled_func == ret_sym.demangled_func) {
                            println!("      WARNING: call/return mismatch - {} called from {}, the returning function's caller is {}", json_name(&call_sym), json_name(ret_caller_sym), json_name(&ret_sym));
                            let mut found = false;
                            while !found {
                                self.write_function_call_event(&mut json, &call_sym.clone(), call_cycle, entry.cycle, returns, &thread_trace.thread_id, &mut funcset)?;
                                if stack.is_empty() {
                                    break;
                                }
                                let last = stack.last().unwrap();
                                call_sym = self.sym_cache.get(&(last.address & ADDRESS_MASK)).unwrap().clone();
                                call_cycle = last.cycle;
                                println!("        WARNING: popping {}", json_name(&call_sym));
                                stack.pop();
                                returns += 1;
                                found = !stack.is_empty() && ret_sym.demangled_func == self.sym_cache.get(&(stack.last().unwrap().address & ADDRESS_MASK)).unwrap().demangled_func;
                            }
                        }
                    }
                    self.write_function_call_event(&mut json, &call_sym, call_cycle, entry.cycle, returns, &thread_trace.thread_id, &mut funcset)?;
                }
            }
            //if the stack isn't empty, record a call with a fake return cycle
            let mut fake_returns = stack.len() as i32;
            for entry in &stack {
                 let call_sym = self.sym_cache.get(&(entry.address & ADDRESS_MASK)).unwrap();
                 self.write_function_call_event(&mut json, &call_sym.clone(), entry.cycle, latest_cycle, fake_returns, &thread_trace.thread_id, &mut funcset)?;
                 fake_returns -= 1;
            }
            let name = String::from_utf8(thread_trace.thread_id.name.iter().filter(|&&x| x != 0 as u8).copied().collect()).unwrap();
            if latest_cycle >= earliest_cycle {
                println!("  thread {} {} - {} recent function calls logged over {} cycles [{} - {}]", thread_trace.thread_id.tid, name, self.num_events, latest_cycle-earliest_cycle, earliest_cycle-self.time_base, latest_cycle-self.time_base);
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
            println!("  ftrace - {} events logged over {} cycles [{} - {}]", ftrace_events.len(), newest_ftrace-oldest_ftrace, oldest_ftrace-self.time_base, newest_ftrace-self.time_base);
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

        self.procaddr2sym.input_source = Some(procaddr2sym::input_source(file_path.clone()));

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
    convert.parse_chunks(&args.funtrace_raw, &args.out_basename)
}

