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
use memmap2::Mmap;
use cpp_demangle;

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

struct Symbol {
    base_address: u64,
    size: u64,
    name: String,
}

fn read_elf_symbols(elf: &Elf) -> Vec<Symbol> {
    // Create a vector to store our symbols
    let mut symbols = Vec::new();

    // Process dynamic symbols if they exist
    for sym in elf.dynsyms.iter() {
        // Get the symbol name from the dynamic string table
        if let Some(name) = elf.dynstrtab.get_at(sym.st_name) {
            symbols.push(Symbol {
                base_address: sym.st_value,
                size: sym.st_size,
                name: name.to_string(),
            });
        }
    }

    // Process regular symbols if they exist
    for sym in elf.syms.iter() {
        // Get the symbol name from the string table
        if let Some(name) = elf.strtab.get_at(sym.st_name) {
            symbols.push(Symbol {
                base_address: sym.st_value,
                size: sym.st_size,
                name: name.to_string(),
            });
        }
    }

    // Sort symbols by base address
    symbols.sort_by_key(|sym| sym.base_address);

    symbols
}

fn find_symbol(symbols: &Vec<Symbol>, address: u64) -> Option<&Symbol> {
    // Binary search for the largest base address that's <= our target address
    let idx = match symbols.binary_search_by_key(&address, |sym| sym.base_address) {
        Ok(exact) => exact,
        Err(insert_pos) => {
            if insert_pos == 0 {
                return None;
            }
            insert_pos - 1
        }
    };

    // Get candidate symbol and check if address falls within its range
    let candidate = &symbols[idx];
    if address >= candidate.base_address && address < candidate.base_address + candidate.size {
        Some(candidate)
    } else {
        None
    }
}

struct ExecutableFileMetadata
{
    program_headers: Vec<ProgramHeader>,
    addr2line: Context<EndianReader<RunTimeEndian, Rc<[u8]>>>,
    symbols: Vec<Symbol>,
}

pub struct ProcAddr2Sym {
    maps: Vec<MemoryMap>,
    sym_cache: HashMap<String, ExecutableFileMetadata>,
    offset_cache: HashMap<u64, u64>,
}

#[derive(Debug, Clone, Hash, PartialEq, std::cmp::Eq)]
pub struct SymInfo {
    pub func: String, //before c++filt
    pub demangled_func: String, //after c++filt
    //note that these are, whenever possible, the file:line of the FIRST function
    //address, NOT the address passed to proc_addr2sym!
    //TODO: given demand we can provide a way to pass the file:line of the actual
    //address passed to proc_addr2sym
    pub file: String, //source file
    pub line: u32, //line number in the file
    pub executable_file: String, //executable or shared object
    pub static_addr: u64, //the address in the executable's symbol table
    //(without the dynamic offset to which it's loaded - this offset is subtracted
    //from the input address passed to proc_addr2sym()). like file:line, whenever
    //possible, this is the base address of the function, not the address
    //directly corresponding to the input dynamic address
}

impl ProcAddr2Sym {
    pub fn new() -> Self {
        ProcAddr2Sym { maps: Vec::new(), sym_cache: HashMap::new(), offset_cache: HashMap::new() }
    }

    // note that updating the maps doesn't invalidate sym_cache - we don't need to parse
    // the DWARF of the executables / shared objects again; but it does invalidate offset_cache
    // since the same shared object might have been loaded to a different offset
    pub fn set_proc_maps(&mut self, proc_maps_data: &[u8]) {
        let memory_maps = MemoryMaps::from_buf_read(proc_maps_data).expect("failed to parse /proc/self/maps data");
        self.maps = memory_maps.into_iter().collect();
        // not sure we need to sort them - /proc/self/maps appears already sorted - but can't hurt
        self.maps.sort_by_key(|map| map.address.0);
        self.offset_cache = HashMap::new();
    }

    pub fn proc_addr2sym(&mut self, proc_address: u64) -> SymInfo {
        let unknown: SymInfo = SymInfo { func: "??".to_string(), demangled_func: "??".to_string(), file: "??".to_string(), line: 0, executable_file: "??".to_string(), static_addr: 0 };

        let map_opt = find_address_in_maps(proc_address, &self.maps);
        if map_opt == None { return unknown; }
        let map = map_opt.unwrap();

        let path_opt = match &map.pathname {
            MMapPath::Path(p) => Some(p),
            _ => None,
        };
        if path_opt == None { return unknown; }
        let path = path_opt.unwrap();

        let pathstr = path.to_string_lossy().to_string();
        if !self.sym_cache.contains_key(&pathstr) {
            let file = File::open(path).expect("failed to open executable file");
            let buffer = unsafe { Mmap::map(&file).expect("failed to mmap executable file") };
            let elf = Elf::parse(&buffer).expect("Failed to parse ELF");
            let symbols = read_elf_symbols(&elf);
            let program_headers = elf.program_headers.clone();
            let object = object::File::parse(&*buffer).expect("Failed to parse ELF");
            let ctx = addr2line::Context::new(&object).expect("Failed to create addr2line context");
            self.sym_cache.insert(path.to_string_lossy().to_string(), ExecutableFileMetadata { program_headers, addr2line: ctx, symbols });
        }
        let meta = self.sym_cache.get(&pathstr).unwrap();

        if !self.offset_cache.contains_key(&map.address.0) {
            //find the program header containing the file offset of this mapping
            let mut found = false;
            for phdr in meta.program_headers.iter() {
                if map.offset >= phdr.p_offset && map.offset < (phdr.p_offset + phdr.p_filesz) {
                    let vaddr_offset = (map.offset - phdr.p_offset) + phdr.p_vaddr;
                    self.offset_cache.insert(map.address.0, vaddr_offset);
                    found = true;
                    break;
                }
            }
            if !found { return unknown; } 
        }
        let vaddr_offset = self.offset_cache.get(&map.address.0).unwrap();
        let mut static_addr = proc_address - map.address.0 + vaddr_offset;

        let mut name = "??".to_string();
        let mut demangled_func = "??".to_string();
        let mut name_found = false;

        if let Some(sym) = find_symbol(&meta.symbols, static_addr) {
            name_found = true;
            name = sym.name.clone();
            static_addr = sym.base_address;
            if let Ok(demsym) = cpp_demangle::Symbol::new(name.clone()) {
                demangled_func = demsym.to_string();
            }
            else {
                demangled_func = name.clone();
            }
        }

        let (file, linenum) = match meta.addr2line.find_location(static_addr) {
            Ok(Some(location)) => (location.file.unwrap_or("??"), location.line.unwrap_or(0)),
            _ => ("??",0),
        };

        if !name_found {
            //not sure if we are ever going to meet a case where there's no ELF symbol name
            //but we do have DWARF debug info but can't hurt to try.
            //
            //there are at least 3 reasons not to use this code by itself, without bothering
            //with ELF symbol tables at all:
            //
            //* sometimes you have ELF symbols but no DWARF debug info
            //* some functions (such as "virtual" and "non-virtual" "thunks" auto-generated by gcc
            //  have an ELF symbol but no debug info in DWARF (at least not function name info;
            //  and incidentally we very much _need_ this info because such thunks have __return__
            //  without __fentry__ and we need to keep this from mauling the decoded trace)
            //* we want, at least in funtrace's context, to find file:line of the first function
            //  address, which the ELF symbol readily makes available
            //
            //but it seems harmless to keep this code as fallback just in case
            //(in any case we use addr2line for the file:line info so "the object is already there".)
            if let Ok(frames) = meta.addr2line.find_frames(static_addr).skip_all_loads() { 
                if let Ok(Some(frame)) = frames.last() {
                    if let Some(funref) = frame.function.as_ref() {
                        if let Ok(fname) = funref.raw_name() {
                            name = fname.to_string();
                            demangled_func = name.clone();
                        }
                        if let Ok(dname) = funref.demangle() {
                            demangled_func = dname.to_string();
                        }
                    }
                }
            }
        }
        SymInfo{func:name, demangled_func, file:file.to_string(), line:linenum, executable_file:pathstr, static_addr}
    }
}
