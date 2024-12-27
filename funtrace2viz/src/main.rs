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
    threads: Vec<u32>,
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
    threads: Vec<u32>,
    pub cpu_freq: u64,
}

impl TraceConverter {
    pub fn new(args: &Cli) -> Self {
        TraceConverter { procaddr2sym: ProcAddr2Sym::new(), source_cache: HashMap::new(), sym_cache: HashMap::new(),
            max_event_age: args.max_event_age, oldest_event_time: args.oldest_event_time, dry: args.dry,
            samples: args.samples.clone(), threads: args.threads.clone(), cpu_freq: 0 }
    }

    fn oldest_event(&self, sample_entries: &Vec<Vec<FunTraceEntry>>) -> Option<u64> {
        if let Some(max_age) = self.max_event_age {
            let mut youngest = 0;
            let mut tid = 1;
            for entries in sample_entries {
                if self.threads.is_empty() || self.threads.contains(&tid) {
                    for entry in entries {
                        youngest = max(entry.cycle, youngest);
                    }
                }
                tid += 1;
            }
            Some(youngest - max_age)
        }
        else if let Some(oldest) = self.oldest_event_time {
            Some(oldest)
        }
        else {
            None
        }
    }

    fn write_sample_to_json(&mut self, fname: &String, sample_entries: &Vec<Vec<FunTraceEntry>>) -> io::Result<()> {
        let mut json = if self.dry { File::open("/dev/null")? } else { File::create(fname)? };
        if !self.dry {
            json.write(br#"{
"traceEvents": [
"#)?;
            println!("decoding a trace sample into {}...", fname);
        }
        else {
            println!("inspecting sample {} (without creating the file...)", fname);
        }
        let mut tid = 1;
    
        // we list the set of functions (to tell their file, line pair to vizviewer);
        // we also use this set to only dump the relevant part of the source cache to each
        // json (the source cache persists across samples/jsons but not all files are relevant
        // to all samples)
        let mut funcset: HashSet<SymInfo> = HashSet::new();
        let mut first_event = true;
        let mut ignore_addrs: HashSet<u64> = HashSet::new();
    
        let oldest = self.oldest_event(sample_entries);
    
        let cycles_per_us = self.cpu_freq as f64 / 1000000.0;
        let cycles_per_ns = (self.cpu_freq as f64 / 1000000000.0 + 1.) as u64;
    
        for entries in sample_entries {
            if !self.threads.is_empty() && !self.threads.contains(&tid) {
                println!("ignoring thread {} - not on the list {:?}", tid, self.threads);
                tid += 1;
                continue;
            }
            let mut stack: Vec<FunTraceEntry> = Vec::new();
            let mut num_events = 0;
            let mut earliest_cycle = entries[0].cycle;
            if let Some(oldest_cycle) = oldest {
                earliest_cycle = max(earliest_cycle, oldest_cycle);
            } 
            let latest_cycle = entries[entries.len()-1].cycle;
            let mut num_orphan_returns = 0;
    
            for entry in entries {
                if let Some(oldest_cycle) = oldest {
                    if oldest_cycle > entry.cycle {
                        continue; //ignore old events
                    }
                }
                let ret = (entry.address >> RETURN_BIT) != 0;
                let tailcall = (entry.address >> TAILCALL_BIT) != 0;
                let addr = entry.address & !((1<<RETURN_BIT) | (1<<TAILCALL_BIT));
                if !self.dry {
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
                }
                //println!("ret {} sym {}", ret, json_name(sym_cache.get(&addr).unwrap()));
                if !ret {
                    // call
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
                        (call_entry.cycle, call_entry.address)
                    };
                    num_events += 1;
                    if self.dry {
                        continue;
                    }
                    let call_sym = self.sym_cache.get(&call_addr).unwrap();
                    //warn if we return to a different function from the one predicted by the call stack.
                    //this "shouldn't happen" but it does unless we ignore "virtual override thunks"
                    //and it's good to at least emit a warning when it does since the trace will look strange
                    let ret_sym = self.sym_cache.get(&addr).unwrap();
                    if false && ret_sym.static_addr != call_sym.static_addr {
                        println!("      WARNING: call/return mismatch - {} called, {} returning", json_name(call_sym), json_name(ret_sym));
                        println!("      call stack after the return:");
                        for entry in stack.clone() {
                            println!("        {}", json_name(self.sym_cache.get(&entry.address).unwrap()));
                        }
                    }
    
                    // the redundant ph:X is needed to render the event on Perfetto's timeline, and pid:1
                    // for vizviewer --flamegraph to work
                    json.write(format!(r#"{}{{"tid":{},"ts":{:.4},"dur":{:.4},"name":{},"ph":"X","pid":1}}"#, 
                                if first_event { "" } else { "\n," },
                                tid, call_cycle as f64/cycles_per_us, (entry.cycle-call_cycle) as f64/cycles_per_us, json_name(call_sym)).as_bytes())?; 
    
                    first_event = false;
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
                }
            }
            if latest_cycle >= earliest_cycle {
                println!("  thread {} - {} recent function calls logged over {} cycles [{} - {}]", tid, num_events, latest_cycle-earliest_cycle, earliest_cycle, latest_cycle);
                tid += 1;
            }
            else {
                println!("    skipping a thread (all {} logged function entry/return events are too old)", entries.len());
            }
        }
        if self.dry {
            return Ok(())
        }
    
        json.write(b"],\n")?;
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

    fn parse_chunks(&mut self, file_path: &String, json_basename: &String) -> io::Result<()> {
        let mut file = File::open(file_path)?;
    
        let mut sample_entries: Vec<Vec<FunTraceEntry>> = Vec::new();
        let mut num_json = 0;

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
            else if &magic == b"ENDTRACE" {
                if chunk_length != 0 {
                    println!("warning: non-zero length for ENDTRACE chunk");
                    file.seek(SeekFrom::Current(chunk_length as i64))?;
                    continue;
                }
                if !sample_entries.is_empty() {
                    if self.samples.is_empty() || self.samples.contains(&num_json) {
                        self.write_sample_to_json(&format_json_filename(json_basename, num_json), &sample_entries)?;
                    }
                    else {
                        println!("ignoring sample {} - not on the list {:?}", num_json, self.samples);
                    }
                    num_json += 1;
                    sample_entries.clear();
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
            } else if &magic == b"TRACEBUF" {
                if chunk_length % mem::size_of::<FunTraceEntry>() != 0 {
                    println!("Invalid TRACEBUF chunk length {} - must be a multiple of {}", chunk_length, mem::size_of::<FunTraceEntry>());
                    file.seek(SeekFrom::Current(chunk_length as i64))?;
                    continue;
                }
    
                let num_entries = chunk_length / mem::size_of::<FunTraceEntry>();
                let mut entries = vec![FunTraceEntry { address: 0, cycle: 0 }; num_entries];
                file.read_exact(bytemuck::cast_slice_mut(&mut entries))?;
                entries.retain(|&entry| !(entry.cycle == 0 && entry.address == 0));
                if !entries.is_empty() {
                    entries.sort_by_key(|entry| entry.cycle);
                    sample_entries.push(entries);
                }
            } else {
                println!("Unknown chunk type: {:?}", std::str::from_utf8(&magic).unwrap_or("<invalid>"));
                file.seek(SeekFrom::Current(chunk_length as i64))?;
            }
        }
        if !sample_entries.is_empty() {
            println!("warning: FUNTRACE block not closed by ENDTRACE");
            self.write_sample_to_json(&format_json_filename(json_basename, num_json), &sample_entries)?;
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

