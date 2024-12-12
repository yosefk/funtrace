use addr2line::{
    Context,
    object,
    gimli::{EndianReader, RunTimeEndian},
};
use addr2line::fallible_iterator::FallibleIterator;
use std::rc::Rc;
use procfs::process::{MemoryMaps, MemoryMap, MMapPath};
use procfs::FromBufRead;
use goblin::elf::{Elf, ProgramHeader};
use std::collections::HashMap;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::env;
use memmap2::Mmap;

macro_rules! fail {
    ($($arg:tt)*) => {{
        println!($($arg)*);
        std::process::exit(1);
    }};
}

fn find_address_in_maps(address: u64, maps: &Vec<MemoryMap>) -> Option<&MemoryMap> {
    maps.binary_search_by(|map| {
        if address < map.address.0 {
            std::cmp::Ordering::Greater // Address is before this map
        } else if address >= map.address.1 {
            std::cmp::Ordering::Less // Address is after this map
        } else {
            std::cmp::Ordering::Equal // Address is within this map
        }
    })
    .ok()
    .map(|index| &maps[index])
}

struct ExecutableFileMetadata
{
    program_headers: Vec<ProgramHeader>,
    addr2line: Context<EndianReader<RunTimeEndian, Rc<[u8]>>>,
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

    let memory_maps = MemoryMaps::from_buf_read(proc_maps_data.as_bytes()).expect("failed to parse /proc/self/map data");
    let mut maps: Vec<_> = memory_maps.into_iter().collect();
    // not sure we need to sort them - /proc/self/maps appears already sorted - but can't hurt
    maps.sort_by_key(|map| map.address.0);

    // Parse the (address, count) pairs
    let mut sym_cache: HashMap<String, ExecutableFileMetadata> = HashMap::new();
    let mut offset_cache: HashMap<u64, u64> = HashMap::new();

    while reader.read_line(&mut line).expect("failure reading input file") > 0 {
        let parts: Vec<&str> = line.trim().split_whitespace().collect();
        if parts.len() != 2 { fail!("Invalid address-count pair {}", line); }

        let mut address = u64::from_str_radix(parts[0].trim_start_matches("0x"), 16).expect("bad address");
        let count = parts[1].parse::<u64>().expect("bad count");

        let map = find_address_in_maps(address, &maps).expect("address not found in /proc/self/maps of the process which produced funcount.txt");

        let path = match &map.pathname {
            MMapPath::Path(p) => p,
            _ => fail!("address doesn't have an associated executable file (mapping {:?})", map),
        };
        let pathstr = path.to_string_lossy().to_string();
        if !sym_cache.contains_key(&pathstr) {
            let file = File::open(path).expect("failed to open executable file");
            let buffer = unsafe { Mmap::map(&file).expect("failed to mmap executable file") };
            let elf = Elf::parse(&buffer).expect("Failed to parse ELF");
            let program_headers = elf.program_headers.clone();
            let object = object::File::parse(&*buffer).expect("Failed to parse ELF");
            let ctx = addr2line::Context::new(&object).expect("Failed to create addr2line context");
            sym_cache.insert(path.to_string_lossy().to_string(), ExecutableFileMetadata { program_headers, addr2line: ctx });
        }
        let meta = sym_cache.get(&pathstr).unwrap();

        if !offset_cache.contains_key(&map.address.0) {
            //find the program header containing the file offset of this mapping
            let mut found = false;
            for phdr in meta.program_headers.iter() {
                if map.offset >= phdr.p_offset && map.offset < (phdr.p_offset + phdr.p_filesz) {
                    let vaddr_offset = (map.offset - phdr.p_offset) + phdr.p_vaddr;
                    offset_cache.insert(map.address.0, vaddr_offset);
                    found = true;
                    break;
                }
            }
            if !found { fail!("can't find the program header containing the mapping"); }
        }
        let vaddr_offset = offset_cache.get(&map.address.0).unwrap();
        address = address - map.address.0 + vaddr_offset;

        let location = meta.addr2line.find_location(address).unwrap().unwrap();

        //it could be better to use ELF symbol table lookup instead of DWARF name info
        //(which has the advantage of having inlining information, with said advantage completely useless
        //for us); this was done for writing little code and hopefully getting something more efficient
        //than a linear lookup
        let frames = meta.addr2line.find_frames(address).skip_all_loads().unwrap();
        let frame = frames.last().unwrap().unwrap();
        let name = frame.function.as_ref().unwrap().raw_name().unwrap();
             
        println!("{} {} {}:{} {}", count, parts[0], location.file.unwrap(), location.line.unwrap(), name.to_string());

        line.clear();
    }
}
