#include <cassert>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <map>

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
    void NOINSTR init() { memset(pagesL1, 0, sizeof(pagesL1)); }

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

        auto mid = mid_bits(address);
        auto page = pages->pages[mid];

        auto low = low_bits(address);
        return page->counts[low / sizeof(count_t)];
    }

    NOINSTR ~CountsPagesL2();
};

static CountsPagesL2 g_page_tab[FUNCOUNT_PAGE_TABLES];

static thread_local int g_rand_state = 0;

static inline int INLINE NOINSTR lcg_rand()
{
     int next = (1103515245 * g_rand_state + 12345) & 0x7fffffff;
     g_rand_state = next;
     return next;
}

extern "C" void NOINSTR __cyg_profile_func_enter(void* func, void* caller)
{
    static_assert(sizeof(count_t) == sizeof(std::atomic<count_t>), "wrong size of atomic<count_t>");
    uint64_t addr = (uint64_t)func;
    int tab_ind = FUNCOUNT_PAGE_TABLES == 1 ? 0 : lcg_rand() % FUNCOUNT_PAGE_TABLES;
    std::atomic<count_t>& count = g_page_tab[tab_ind].get_count(addr);
    count += 1;
}

extern "C" void NOINSTR __cyg_profile_func_exit(void* func, void* caller) {}

#include <fstream>
#include <vector>
#include <iostream>

static int g_destroyed_pages = 0;
static std::map<uint64_t, uint64_t> g_addr2count;

NOINSTR CountsPagesL2::~CountsPagesL2()
{
    std::ofstream out("funcount.txt");
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
                            if(FUNCOUNT_PAGE_TABLES == 1) {
                                out << std::hex << "0x" << address << ' ' << std::dec << count << '\n';
                            }
                            else {
                                g_addr2count[address] += count;
                            }
                        }
                    }
                }
            }
        }
    }
    g_destroyed_pages++;
    if(g_destroyed_pages == FUNCOUNT_PAGE_TABLES) {
        if(FUNCOUNT_PAGE_TABLES > 1) {
            for(const auto& p : g_addr2count) {
                out << std::hex << "0x" << p.first << ' ' << std::dec << p.second << '\n';
            }
        }
        std::cout << "function call count report saved to funcount.txt" << std::endl;
    }
}

#include <link.h>
#include <sstream>

//finding the executable segments using dl_iterate_phdr() is faster than reading /proc/self/maps
//and produces less segments since we ignore the non-executable ones
static int NOINSTR phdr_callback (struct dl_phdr_info *info, size_t size, void *data)
{
    for(int i=0; i<info->dlpi_phnum; ++i ) {
        const auto& phdr = info->dlpi_phdr[i];
        //we only care about loadable executable segments (the likes of .text)
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


__attribute__((constructor(101))) void NOINSTR main_init(void)
{
    for(int t=0; t<FUNCOUNT_PAGE_TABLES; ++t) {
        g_page_tab[t].init();
    }
    allocate_page_tables();
}

/*
extern "C" void NOINSTR dlopen(const char *filename, int flags)
{
    void (*orig)(const char*,int) = (void (*)(const char*,int))dlsym(RTLD_NEXT, "dlopen");
    (*orig)(filename, flags);
    allocate_page_tables();
}*/
