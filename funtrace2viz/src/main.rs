use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::io::prelude::*;
use std::mem;
use bytemuck::{Pod, Zeroable};
use std::collections::HashMap;
use procaddr2sym::{ProcAddr2Sym, SymInfo};
use std::env;

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

fn start_json(mut json: &File) -> io::Result<()> {
    json.write(br#"{
"traceEvents": [
"#)?;
    Ok(())
}

fn finish_json(mut json: &File) -> io::Result<()> {
    json.write(b"]\n}\n")?;
    Ok(())
}

fn format_json_filename(basename: &String, number: u32) -> String {
    if number > 0 {
        format!("{}.{}.json", basename, number)
    } else {
        format!("{}.json", basename)
    }
}

fn parse_chunks(file_path: &String, json_basename: &String) -> io::Result<()> {
    let mut file = File::open(file_path)?;
    let mut procaddr2sym = ProcAddr2Sym::new();
    let mut sym_cache: HashMap<u64, SymInfo> = HashMap::new();
    let mut stack: Vec<FunTraceEntry> = Vec::new();

    let mut json_opt = None;
    let mut num_json = 0;
    let mut tid = 1;

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

        if &magic == b"FUNTRACE" || &magic == b"ENDTRACE" {
            //these are empty chunks telling where the trace buffers of a single trace
            //sample start / end
            if chunk_length != 0 {
                println!("warning: non-zero length for {}", std::str::from_utf8(&magic).unwrap());
                file.seek(SeekFrom::Current(chunk_length as i64))?;
            }
            if &magic == b"FUNTRACE" {
                match json_opt {
                    Some(ref json) => {
                        println!("warning: FUNTRACE block not closed");
                        finish_json(json)?;
                    },
                    _ => {}
                }
                json_opt = Some(File::create(format_json_filename(json_basename, num_json))?);
                start_json(&json_opt.as_ref().unwrap())?;
                tid = 1;
            }
            else {
                match json_opt {
                    Some(ref json) => { finish_json(&json)?; },
                    _ => { println!("warning: ENDTRACE without a preceding FUNTRACE"); }
                }
                json_opt = None;
                num_json += 1;
            }
            continue;
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
            entries.sort_by_key(|entry| entry.cycle);

            if !json_opt.is_some() {
                println!("Ignoring a TRACEBUF chunk since it's outside a FUNTRACE ... ENDTRACE area");
                continue;
            }
            let mut json = json_opt.as_ref().unwrap();

            let earliest_cycle = entries[0].cycle;

            for entry in entries {
                let ret = entry.address >> RETURN_BIT;
                let addr = entry.address & !(1<<RETURN_BIT);
                if !sym_cache.contains_key(&addr) {
                    sym_cache.insert(addr, procaddr2sym.proc_addr2sym(addr));
                }
                if ret == 0 {
                    // call
                    stack.push(entry);
                }
                else {
                    //write an event to json
                    let call_cycle = if stack.is_empty() {
                        // a return without a call - the call event must have been overwritten
                        // in the cyclic trace buffer; fake a call at the start of the trace
                        earliest_cycle
                    }
                    else {
                        stack.pop().unwrap().cycle
                    };
                    let sym = sym_cache.get(&addr).unwrap();
                    json.write(format!(r#"{{"tid":{},"ts":{},"dur":{},"name":"{} ({}:{})","ph":"X"}},
"#, 
                                tid, call_cycle, entry.cycle-call_cycle, sym.demangled_func, sym.file, sym.line).as_bytes())?; 
                }
            }
            tid += 1;
        } else {
            println!("Unknown chunk type: {:?}", std::str::from_utf8(&magic).unwrap_or("<invalid>"));
            file.seek(SeekFrom::Current(chunk_length as i64))?;
        }
    }

    Ok(())
}

fn main() -> io::Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() != 3 {
        println!("Usage: {} <funtrace.raw> <basename> #basename.json, basename.1.json, basename.2.json... are created, one file per trace sample", args[0]);
        std::process::exit(1);
    }
    let raw_file = &args[1];
    let out_basename = &args[2];
    parse_chunks(&raw_file, &out_basename)
}

