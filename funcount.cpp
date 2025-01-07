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
    std::atomic<CountsPage*> pages[PAGE_SIZE];
    NOINSTR CountsPagesL1() { memset(pages, 0, sizeof(pages)); }
};

static thread_local CountsPagesL1* g_freshPagesL1 = nullptr;
static thread_local CountsPage* g_freshPage = nullptr;

struct CountsPagesL2
{
    std::atomic<CountsPagesL1*> pagesL1[PAGE_SIZE];
    NOINSTR CountsPagesL2() { memset(pagesL1, 0, sizeof(pagesL1)); }

    //this is "honestly" thread safe; it might be faster if it only returned an atomic
    //counter but allowed 2 threads to notice that a page wasn't allocated and
    //allocate it concurrently, with one of the threads losing the counts which
    //would be accumulated in its page which would be lost to the other thread
    //writing the page pointer to pagesL1[] or pages[] arrays. but this thing
    //probably needs to be trustworthy a bit more than it needs to be fast
    //(you collect the counts mainly to decide which functions to not instrument
    //with funtrace tracing; you don't do this often so slowness is not that bad
    //in most cases whereas wondering about correctness is annoying. if slowness
    //makes the flow unusable for you you can try replacing atomic<T*> with plain T*)
    std::atomic<count_t>& INLINE NOINSTR get_count(uint64_t address)
    {
        //this and the next similar if test could be avoded by interposing pthread_create
        //and initializing the TLS variables (or using ctors but that's even worse
        //than the if statements in the generated code); probably not worth it
        //given the much costlier stuff like the 3 atomic operations we have
        if(!g_freshPagesL1) {
            g_freshPagesL1 = new CountsPagesL1;
        }
        auto high = high_bits(address);
        auto& pages = pagesL1[high];
        CountsPagesL1* nullPages = nullptr;
        if(pages.compare_exchange_strong(nullPages, g_freshPagesL1)) {
            g_freshPagesL1 = new CountsPagesL1;
        }

        if(!g_freshPage) {
            g_freshPage = new CountsPage;
        }
        auto mid = mid_bits(address);
        auto& page = pages.load()->pages[mid];
        CountsPage* nullPage = nullptr;
        if(page.compare_exchange_strong(nullPage, g_freshPage)) {
            g_freshPage = new CountsPage;
        }
        auto low = low_bits(address);
        return page.load()->counts[low / sizeof(count_t)];
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
    std::atomic<count_t>& count = g_page_tab[lcg_rand() % FUNCOUNT_PAGE_TABLES].get_count(addr);
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
        auto pages = pagesL1[hi].load();
        if(pages) {
            for(uint64_t mid=0; mid<PAGE_SIZE; ++mid) {
                auto page = pages->pages[mid].load();
                if(page) {
                    for(uint64_t lo=0; lo<PAGE_SIZE/sizeof(count_t); ++lo) {
                        auto& count = page->counts[lo];
                        if(count) {
                            uint64_t address = (hi << PAGE_BITS*2) | (mid << PAGE_BITS) | (lo * sizeof(count_t));
                            g_addr2count[address] += count;
                        }
                    }
                }
            }
        }
    }
    g_destroyed_pages++;
    if(g_destroyed_pages == FUNCOUNT_PAGE_TABLES) {
        for(const auto& p : g_addr2count) {
            out << std::hex << "0x" << p.first << ' ' << std::dec << p.second << '\n';
        }
        std::cout << "function call count report saved to funcount.txt" << std::endl;
    }
}
