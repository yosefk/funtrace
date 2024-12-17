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

pub struct ProcAddr2Sym {
    maps: Vec<MemoryMap>,
    sym_cache: HashMap<String, ExecutableFileMetadata>,
    offset_cache: HashMap<u64, u64>,
}

#[derive(Debug, Clone, Hash, PartialEq, std::cmp::Eq)]
pub struct SymInfo {
    pub func: String,
    pub demangled_func: String,
    pub file: String,
    pub line: u32,
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
        let unknown: SymInfo = SymInfo { func: "??".to_string(), demangled_func: "??".to_string(), file: "??".to_string(), line: 0 };

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
            let program_headers = elf.program_headers.clone();
            let object = object::File::parse(&*buffer).expect("Failed to parse ELF");
            let ctx = addr2line::Context::new(&object).expect("Failed to create addr2line context");
            self.sym_cache.insert(path.to_string_lossy().to_string(), ExecutableFileMetadata { program_headers, addr2line: ctx });
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
        let address = proc_address - map.address.0 + vaddr_offset;

        let (file, linenum) = match meta.addr2line.find_location(address) {
            Ok(Some(location)) => (location.file.unwrap_or("??"), location.line.unwrap_or(0)),
            _ => ("??",0),
        };

        //it could be better to use ELF symbol table lookup instead of DWARF name info
        //(which has the advantage of having inlining information, with said advantage completely useless
        //for us); this was done for writing little code and hopefully getting something more efficient
        //than a linear lookup
        let mut name = "??".to_string();
        let mut demangled_func = "??".to_string();
        if let Ok(frames) = meta.addr2line.find_frames(address).skip_all_loads() { 
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
        SymInfo{func:name, demangled_func, file:file.to_string(), line:linenum}
    }
}
