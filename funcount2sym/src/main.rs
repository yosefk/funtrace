use procaddr2sym::ProcAddr2Sym;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::env;

macro_rules! fail {
    ($($arg:tt)*) => {{
        println!($($arg)*);
        std::process::exit(1);
    }};
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        fail!("Usage: {} <funcount.txt> # counts with function names printed to stdout, pipe through c++filt if you want to demangle the symbols", args[0]);
    }

    // Open the input file
    let file = File::open(args[1].to_string()).expect("failed to open input file");
    let mut reader = BufReader::new(file);

    // Validate and parse the magic strings
    let mut line = String::new();
    reader.read_line(&mut line).expect("failed to read FUNCOUNT");
    if line.trim() != "FUNCOUNT" { fail!("missing FUNCOUNT magic string - got `{}'", line); }
    line.clear();

    reader.read_line(&mut line).expect("failed to read PROCMAPS");
    if line.trim() != "PROCMAPS" { fail!("missing PROCMAPS magic string - got `{}'", line); }
    line.clear();

    // Read and parse the memory maps
    let mut proc_maps_data = String::new();
    let mut found = false;
    while reader.read_line(&mut line).expect("failure reading input file") > 0 {
        if line.trim() == "COUNTS" {
            found = true;
            break;
        }
        proc_maps_data.push_str(&line);
        line.clear();
    }
    if !found { fail!("COUNTS magic string not found"); }
    line.clear();

    let input_source = Some(procaddr2sym::input_source(args[1].to_string()));
    let mut procaddr2sym = ProcAddr2Sym::new();
    procaddr2sym.input_source = input_source;
    procaddr2sym.set_proc_maps(proc_maps_data.as_bytes());

    while reader.read_line(&mut line).expect("failure reading input file") > 0 {
        let parts: Vec<&str> = line.trim().split_whitespace().collect();
        if parts.len() != 2 { fail!("Invalid address-count pair {}", line); }

        let address = u64::from_str_radix(parts[0].trim_start_matches("0x"), 16).expect("bad address");
        let count = parts[1].parse::<u64>().expect("bad count");

        let syminfo = procaddr2sym.proc_addr2sym(address);

        println!("{} {} {}:{} {}", count, parts[0], syminfo.file, syminfo.line, syminfo.func);

        line.clear();
    }
}
