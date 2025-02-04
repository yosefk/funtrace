#include <cassert>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <map>
#include <link.h>
#include <x86intrin.h>

#ifndef FUNCOUNT_PAGE_TABLES
#define FUNCOUNT_PAGE_TABLES 1
#endif

#ifdef __clang__
#define NOINSTR __attribute__((xray_never_instrument)) __attribute__((no_instrument_function))
#else
#define NOINSTR __attribute__((no_instrument_function))
#endif
#define INLINE __attribute__((always_inline))

const int PAGE_BITS = 16; //works for a 2-level page table with 48b virtual addresses
//which is OK for most userspace address spaces
const int PAGE_SIZE = 1<<PAGE_BITS;
const uint64_t PAGE_BITS_MASK = PAGE_SIZE-1;

inline uint64_t NOINSTR high_bits(uint64_t address)
{
    uint64_t bits = address >> PAGE_BITS*2;
    //make sure bits higher than PAGE_BITS*3 are not set
    assert((bits & PAGE_BITS_MASK) == bits && "pointer has more than 48 bits set - try recompiling funcount.cpp with a larger PAGE_BITS constant");
    return bits;
}

inline uint64_t NOINSTR mid_bits(uint64_t address) { return (address >> PAGE_BITS) & PAGE_BITS_MASK; }
inline uint64_t NOINSTR low_bits(uint64_t address) { return address & PAGE_BITS_MASK; }

//8-byte counts have the downside where very short functions are counted together;
//4-byte counts would have been better for this but would be more likely to overflow
typedef uint64_t count_t;

struct CountsPage
{
    std::atomic<count_t> counts[PAGE_SIZE/sizeof(count_t)];
    NOINSTR CountsPage() { memset(counts, 0, sizeof(counts)); }
};

struct CountsPagesL1
{
    CountsPage* pages[PAGE_SIZE];
    NOINSTR CountsPagesL1() { memset(pages, 0, sizeof(pages)); }
};

struct CountsPagesL2
{
    CountsPagesL1* pagesL1[PAGE_SIZE];
    //this counts function calls in executable segments not mapped at the time
    //when the code was running (allocate_range() wasn't called); AFAIK this
    //should be limited to constructors in shared objects (which get called
    //before we get a chance to call dl_iterate_phdr() to update our view
    //of the address space)
    //
    //note that these misses could be avoided by allocating the pages on demand
    //when a function is first called; however this slows things down even
    //if it's done in a non-thread-safe manner (potentially leaking pages and
    //losing call counts) and more so if it's done with in a thread-safe way
    //(we have a commit in the history doing this with 2 compare_exchange_strong()
    //calls.) for the purpose of finding the most commonly called functions
    //in order to exclude them from funtrace instrumentation, it's probably better
    //to limit the slowdown (s.t. interactive / real-time flows have some chance
    //of being usable for collecting statistics) at the expense of missing
    //constructor calls in dynamic libraries (which are very unlikely to be
    //where you badly need to suppress funtrace instrumentation because of
    //its overhead)
    std::atomic<count_t> unknown;

    void NOINSTR init()
    {
        memset(pagesL1, 0, sizeof(pagesL1));
        unknown = 0;
    }

    void NOINSTR allocate_range(uint64_t base, uint64_t size)
    {
        uint64_t start = base & ~PAGE_BITS_MASK;
        uint64_t end = (base + size + PAGE_SIZE - 1) & ~PAGE_BITS_MASK;
        for(uint64_t address=start; address<=end; address+=PAGE_SIZE) {
            auto high = high_bits(address);
            auto& pages = pagesL1[high];
            if(!pages) {
                pages = new CountsPagesL1;
            }

            auto mid = mid_bits(address);
            auto& page = pages->pages[mid];
            if(!page) {
                page = new CountsPage;
            }
        }
    }

    std::atomic<count_t>& INLINE NOINSTR get_count(uint64_t address)
    {
        auto high = high_bits(address);
        auto pages = pagesL1[high];
        if(!pages) {
            return unknown;
        }

        auto mid = mid_bits(address);
        auto page = pages->pages[mid];
        if(!page) {
            return unknown;
        }

        auto low = low_bits(address);
        return page->counts[low / sizeof(count_t)];
    }

    NOINSTR ~CountsPagesL2();
};

static CountsPagesL2 g_page_tab[FUNCOUNT_PAGE_TABLES];

static inline unsigned int INLINE NOINSTR core_num()
{
    unsigned int aux;
    __rdtscp(&aux);
    return aux & 0xfff;
}

extern "C" void NOINSTR __cyg_profile_func_enter(void* func, void* caller)
{
    static_assert(sizeof(count_t) == sizeof(std::atomic<count_t>), "wrong size of atomic<count_t>");
    uint64_t addr = (uint64_t)func;
    int tab_ind = FUNCOUNT_PAGE_TABLES == 1 ? 0 : core_num() % FUNCOUNT_PAGE_TABLES;
    std::atomic<count_t>& count = g_page_tab[tab_ind].get_count(addr);
    count += 1;
}

extern "C" void NOINSTR __cyg_profile_func_exit(void* func, void* caller) {}

#include <fstream>
#include <vector>
#include <iostream>

NOINSTR CountsPagesL2::~CountsPagesL2()
{
    //the first object in the array is constructed first and destroyed last -
    auto last_page_tab = &g_page_tab[0];

    std::ofstream out;
    if(this == last_page_tab) {
        out.open("funcount.txt");
        out << "FUNCOUNT\nPROCMAPS\n";
        std::ifstream maps_file("/proc/self/maps", std::ios::binary);
        if (!maps_file.is_open()) {
            std::cerr << "funtrace - failed to open /proc/self/maps, traces will be impossible to decode" << std::endl;
            return;
        }

        std::vector<char> maps_data(
            (std::istreambuf_iterator<char>(maps_file)),
            std::istreambuf_iterator<char>());

        maps_file.close();
        out.write(&maps_data[0], maps_data.size());
        out << "COUNTS\n";
    }

    for(uint64_t hi=0; hi<PAGE_SIZE; ++hi) {
        auto pages = pagesL1[hi];
        if(pages) {
            for(uint64_t mid=0; mid<PAGE_SIZE; ++mid) {
                auto page = pages->pages[mid];
                if(page) {
                    for(uint64_t lo=0; lo<PAGE_SIZE/sizeof(count_t); ++lo) {
                        auto& count = page->counts[lo];
                        if(count) {
                            uint64_t address = (hi << PAGE_BITS*2) | (mid << PAGE_BITS) | (lo * sizeof(count_t));
                            if(this == last_page_tab) {
                                //print the final counts
                                out << std::hex << "0x" << address << ' ' << std::dec << count << '\n';
                            }
                            else {
                                //accumulate the results into the first page table
                                last_page_tab->get_count(address) += count;
                            }
                        }
                    }
                }
                pages->pages[mid] = nullptr;
                delete page;
            }
            pagesL1[hi] = nullptr;
            delete pages;
        }
    }
    if(unknown) {
        if(this == last_page_tab) {
            std::cout << "WARNING: " << unknown << " function calls were to functions in parts of the address space unknown at the time they were made (likely constructors in shared objects)" << std::endl;
        }
        else {
            last_page_tab->unknown += unknown;
        }
    }
    if(this == last_page_tab) {
        std::cout << "function call count report saved to funcount.txt - decode with funcount2sym to get: call_count, dyn_addr, static_addr, num_bytes, bin_file, src_file:src_line, mangled_func_name" << std::endl;
    }
}

static int NOINSTR phdr_callback (struct dl_phdr_info *info, size_t size, void *data)
{
    for(int i=0; i<info->dlpi_phnum; ++i ) {
        const auto& phdr = info->dlpi_phdr[i];
        if(phdr.p_type == PT_LOAD && (phdr.p_flags & PF_X)) {
            uint64_t start_addr = info->dlpi_addr + phdr.p_vaddr;
            for(int t=0; t<FUNCOUNT_PAGE_TABLES; ++t) {
                g_page_tab[t].allocate_range(start_addr, phdr.p_memsz);
            }
        }
    }
    return 0;
}

static void NOINSTR allocate_page_tables()
{
    dl_iterate_phdr(phdr_callback, nullptr);
}

__attribute__((constructor(101))) void NOINSTR funcount_init()
{
    for(int t=0; t<FUNCOUNT_PAGE_TABLES; ++t) {
        g_page_tab[t].init();
    }
    allocate_page_tables();
}


extern "C" void* NOINSTR dlopen(const char *filename, int flags)
{
    void* (*orig)(const char*,int) = (void* (*)(const char*,int))dlsym(RTLD_NEXT, "dlopen");
    void* lib = (*orig)(filename, flags);
    allocate_page_tables();
    return lib;
}


extern "C" void* NOINSTR dlmopen(Lmid_t lmid, const char *filename, int flags)
{
    void* (*orig)(Lmid_t,const char*,int) = (void* (*)(Lmid_t,const char*,int))dlsym(RTLD_NEXT, "dlmopen");
    void* lib = (*orig)(lmid, filename, flags);
    allocate_page_tables();
    return lib;
}

//provide empty implementations of the funtrace APIs so that you could use funcount
//with a program calling funtrace APIs easily (and not just the first time you integrate the APIs)
extern "C" {
void funtrace_pause_and_write_current_snapshot() {}
struct funtrace_snapshot* funtrace_pause_and_get_snapshot() { return nullptr; }
uint64_t funtrace_time() { return __rdtsc(); }
uint64_t funtrace_ticks_per_second() { return 1000000000; } //we shouldn't need this to be correct */
struct funtrace_snapshot* funtrace_pause_and_get_snapshot_starting_at_time(uint64_t time) { return nullptr; }
struct funtrace_snapshot* funtrace_pause_and_get_snapshot_up_to_age(uint64_t max_event_age) { return nullptr; }
void funtrace_free_snapshot(struct funtrace_snapshot* snapshot) {}
void funtrace_write_snapshot(const char* filename, struct funtrace_snapshot* snapshot) {}
void funtrace_ignore_this_thread() {}
void funtrace_set_thread_log_buf_size(int log_buf_size) {}
void funtrace_disable_tracing() {}
void funtrace_enable_tracing() {}
}

