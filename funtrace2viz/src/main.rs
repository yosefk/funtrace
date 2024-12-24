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

fn oldest_event(sample_entries: &Vec<Vec<FunTraceEntry>>, max_event_age: &Option<u64>, oldest_event_time: &Option<u64>, threads: &Vec<u32>) -> Option<u64> {
    if let Some(max_age) = max_event_age {
        let mut youngest = 0;
        let mut tid = 1;
        for entries in sample_entries {
            if threads.is_empty() || threads.contains(&tid) {
                for entry in entries {
                    youngest = max(entry.cycle, youngest);
                }
            }
            tid += 1;
        }
        Some(youngest - max_age)
    }
    else if let Some(oldest) = oldest_event_time {
        Some(*oldest)
    }
    else {
        None
    }
}

fn write_sample_to_json(fname: &String, sample_entries: &Vec<Vec<FunTraceEntry>>, procaddr2sym: &mut ProcAddr2Sym, source_cache: &mut HashMap<String, SourceCode>, sym_cache: &mut HashMap<u64, SymInfo>, max_event_age: &Option<u64>, oldest_event_time: &Option<u64>, dry: bool, threads: &Vec<u32>, cpu_freq: u64) -> io::Result<()> {
    let mut json = if dry { File::open("/dev/null")? } else { File::create(fname)? };
    if !dry {
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

    let oldest = oldest_event(sample_entries, max_event_age, oldest_event_time, threads);

    let cycles_per_us = cpu_freq as f64 / 1000000.0;

    for entries in sample_entries {
        if !threads.is_empty() && !threads.contains(&tid) {
            println!("ignoring thread {} - not on the list {:?}", tid, threads);
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
            let ret = entry.address >> RETURN_BIT;
            let addr = entry.address & !(1<<RETURN_BIT);
            if !dry && !sym_cache.contains_key(&addr) {
                sym_cache.insert(addr, procaddr2sym.proc_addr2sym(addr));
            }
            if ret == 0 {
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
                    // them in the wrong lane on the timeline
                    (earliest_cycle - num_orphan_returns, addr)
                }
                else {
                    let call_entry = stack.pop().unwrap();
                    (call_entry.cycle, call_entry.address)
                };
                num_events += 1;
                if dry {
                    continue;
                }
                let sym = sym_cache.get(&call_addr).unwrap();
                // the redundant ph:X is needed to render the event on Perfetto's timeline, and pid:1
                // for vizviewer --flamegraph to work
                json.write(format!(r#"{}{{"tid":{},"ts":{:.4},"dur":{:.4},"name":{},"ph":"X","pid":1}}"#, 
                            if first_event { "" } else { "\n," },
                            tid, call_cycle as f64/cycles_per_us, (entry.cycle-call_cycle) as f64/cycles_per_us, json_name(sym)).as_bytes())?; 

                first_event = false;
                funcset.insert(sym.clone());

                //cache the source code if it's the first time we see this file
                if !source_cache.contains_key(&sym.file) {
                    let mut source_code: Vec<u8> = Vec::new();
                    if let Ok(mut source_file) = File::open(&sym.file) {
                        source_file.read_to_end(&mut source_code)?;
                    }
                    let json_str = Value::String(String::from_utf8(source_code.clone()).unwrap()).to_string();
                    let num_lines = source_code.iter().filter(|&&b| b == b'\n').count(); //TODO: num newlines
                    //might be off by one relatively to num lines...
                    source_cache.insert(sym.file.clone(), SourceCode{ json_str, num_lines });
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
    if dry {
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
        if let Some(&ref source_code) = source_cache.get(file) {
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

fn format_json_filename(basename: &String, number: u32) -> String {
    if number > 0 {
        format!("{}.{}.json", basename, number)
    } else {
        format!("{}.json", basename)
    }
}

fn json_name(sym: &SymInfo) -> String {
    Value::String(format!("{} ({}:{})",sym.demangled_func, sym.file, sym.line)).to_string()
}

fn parse_chunks(file_path: &String, json_basename: &String, max_event_age: Option<u64>, oldest_event_time: Option<u64>, dry: bool, samples: &Vec<u32>, threads: &Vec<u32>) -> io::Result<()> {
    let mut file = File::open(file_path)?;
    let mut procaddr2sym = ProcAddr2Sym::new();
    let mut sym_cache: HashMap<u64, SymInfo> = HashMap::new();

    // we dump source code into the JSON files to make it visible in vizviewer
    let mut source_cache = HashMap::new();

    let mut sample_entries: Vec<Vec<FunTraceEntry>> = Vec::new();

    let mut num_json = 0;
    let mut cpu_freq = 0;

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
            cpu_freq = u64::from_ne_bytes(freq_bytes);
        }
        else if &magic == b"ENDTRACE" {
            if chunk_length != 0 {
                println!("warning: non-zero length for ENDTRACE chunk");
                file.seek(SeekFrom::Current(chunk_length as i64))?;
                continue;
            }
            if !sample_entries.is_empty() {
                if samples.is_empty() || samples.contains(&num_json) {
                    write_sample_to_json(&format_json_filename(json_basename, num_json), &sample_entries, &mut procaddr2sym, &mut source_cache, &mut sym_cache, &max_event_age, &oldest_event_time, dry, &threads, cpu_freq)?;
                }
                else {
                    println!("ignoring sample {} - not on the list {:?}", num_json, samples);
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
            procaddr2sym.set_proc_maps(chunk_content.as_slice());
            //the symbol cache might have been invalidated if the process unloaded and reloaded a shared object
            sym_cache = HashMap::new();
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
        write_sample_to_json(&format_json_filename(json_basename, num_json), &sample_entries, &mut procaddr2sym, &mut source_cache, &mut sym_cache, &max_event_age, &oldest_event_time, dry, &threads, cpu_freq)?;
    }

    Ok(())
}

#[derive(Parser)]
#[clap(about="convert funtrace.raw to JSON files in the viztracer/vizviewer format (pip install viztracer)", version)]
struct Cli {
    #[clap(help="funtrace.raw input file with one or more trace samples")]
    functrace_raw: String,
    #[clap(help="basename.json, basename.1.json, basename.2.json... are created, one JSON file per trace sample")]
    out_basename: String,
    #[clap(short, long, help="ignore events older than this relatively to the latest recorded event (very old events create the appearance of a giant blank timeline in vizviewer/Perfetto which zooms out to show the recorded timeline in full)")]
    max_event_age: Option<u64>,
    #[clap(short, long, help="ignore events older than this cycle (like --max-event-age but as a timestamp instead of an age)")]
    oldest_event_time: Option<u64>,
    #[clap(short, long, help="dry run - only list the samples & threads with basic stats, don't decode into JSON")]
    dry: bool,
    #[clap(short, long, help="ignore samples with the indexes outside this list")]
    samples: Vec<u32>,
    #[clap(short, long, help="ignore threads with the indexes outside this list (including for the purpose of interpreting --max-event-age)")]
    threads: Vec<u32>,
}

fn main() -> io::Result<()> {
    let args = Cli::parse();
    if args.max_event_age.is_some() && args.oldest_event_time.is_some() {
        panic!("both --max-event-age and --oldest-event-time specified - choose one");
    }
    parse_chunks(&args.functrace_raw, &args.out_basename, args.max_event_age, args.oldest_event_time, args.dry, &args.samples, &args.threads)
}

