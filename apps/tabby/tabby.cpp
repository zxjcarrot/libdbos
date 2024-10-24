#include <atomic>
#include <algorithm>
#include <cassert>
#include <csignal>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <vector>
#include <span>
#include <map>
#include "spinlock.h"
#include <condition_variable>
//#include <liburing.h>

// #define BACKWARD_HAS_BFD 1

// // #include "backward.hpp"
#include "exception_hack.hpp"

#ifndef NDEBUG
#define TABBY_ASSERT(expr)                                                           \
	if (!(expr)) {                                                             \
		printf("ASSERT(" #expr ") at %s:%d in function %s\n", __FILE__,   \
					__LINE__, __func__);                                       \
      dune_die();                                                            \
	}
#define TABBY_ASSERT2(expr, v)                                                           \
	if (!(expr)) {                                                             \
		printf("ASSERT(" #expr ") at %s:%d in function %s, extra str %s\n", __FILE__,   \
					__LINE__, __func__, (v));                                       \
      exit(EXIT_FAILURE);                                                  \
	}
#else
#define TABBY_ASSERT(expr) while(0) { !(expr); }
#define TABBY_ASSERT2(expr, v) while(0) { !(expr); }
#endif


#include <libaio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <immintrin.h>
#ifdef __cplusplus
extern "C"{
#endif 
#include "dune.h"
extern int dune_cnt;
#ifdef __cplusplus
}
#endif


__thread uint16_t workerThreadId = 0;
__thread int32_t tpcchistorycounter = 0;
#include "tpcc/TPCCWorkload.hpp"
#include "tpcc/ScrambledZipfGenerator.hpp"

using namespace std;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef u64 PID; // page id type

static const u64 pageSize = 4096;

static const int16_t maxWorkerThreads = 256;

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

uint64_t rdtsc() {
   uint32_t hi, lo;
   __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
   return static_cast<uint64_t>(lo)|(static_cast<uint64_t>(hi)<<32);
}

// allocate memory using huge pages
void* allocHuge(size_t size) {
   void* p = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
   madvise(p, size, MADV_HUGEPAGE);
   return p;
}

// use when lock is not free
void yield(u64 counter) {
   _mm_pause();
}

static constexpr u64 kTabbyInvalidAddr = 0xfffffffffffff000;
static constexpr u64 kTabbyBaseVMAddressMask = 0xffff000000000000;
static constexpr u64 kTabbyBaseVMAddress = 0xffff800000000000;
static constexpr u64 kTabbyVersionMask = 0x00000000000fffff;
static constexpr u64 kTabbyStateMask = 0x000000000ff00000;
static constexpr u64 kTabbyAddressSpaceStart = 0xffff800000000000;

// We assume 4-level page table that uses 48-bits of virtual address.
// Since we use 4KB-aligned pages, the lower 12 bits are always zero.
// Therefore we have 64-48+12=28 bits of information. 
// In theory, the maximum database size that can be supported by Tabby is 2^48=256 TiB. 
// For the 28 available bits, we use 20 bits as version number and 8 bits as state.
// We keep the VM address in the top 36 bits of the page state.
// We keep 8 bits as state.
// We keep the version number at the end.
struct VMPageState {
   atomic<u64> vmAddrAndVersionAndState;

   static const u64 Unlocked = 0;
   static const u64 MaxShared = 252;
   static const u64 Locked = 253;
   static const u64 Marked = 254;
   static const u64 Evicted = 255;

   VMPageState() {}

   //void init() { vmAddrAndVersionAndState.store(sameVersion(kTabbyInvalidAddr, 0, Unlocked), std::memory_order_release); }

   void init(u64 addr) { vmAddrAndVersionAndState.store(sameVersion(addr, 0, Unlocked), std::memory_order_release); }
   // Trim a 48-bit vm address down to 36 bits
   static inline u64 compressNormalVMAddress(u64 addr) { return ((~kTabbyBaseVMAddressMask) & addr) >> 12; }
   // Recover a 48-bit address from a 36-bit address
   static inline u64 decompressVMAddress(u64 addr) { return (addr << 12) | kTabbyBaseVMAddressMask; }

   static inline u64 sameVersion(u64 addr, u64 version, u64 newState) { return (compressNormalVMAddress(addr) << 28) | (version) | (newState << 20); }
   static inline u64 nextVersion(u64 addr, u64 version, u64 newState) { return (compressNormalVMAddress(addr) << 28) | ((version + 1) & kTabbyVersionMask) | (newState << 20); }
   // static inline u64 sameVersion(u64 oldStateAndVersion, u64 newState) { return ((oldStateAndVersion<<8)>>8) | newState<<56; }
   // static inline u64 nextVersion(u64 oldStateAndVersion, u64 newState) { return (((oldStateAndVersion<<8)>>8)+1) | newState<<56; }

   bool tryLockX(u64 oldVMAddrAndStateAndVersion) {
      auto v = oldVMAddrAndStateAndVersion;
      return vmAddrAndVersionAndState.compare_exchange_strong(v, nextVersion(getVMAddress(v), getVersion(v), Locked));
   }

   void unlockX() {
      //cerr << "Unlock at state address " << this << endl;
      TABBY_ASSERT2(getState() == Locked, ToString().c_str());
      auto v = vmAddrAndVersionAndState.load();
      vmAddrAndVersionAndState.store(nextVersion(getVMAddress(v), getVersion(v), Unlocked), std::memory_order_release);
   }

   void unlockXEvicted() {
      TABBY_ASSERT(getState() == Locked);
      auto v = vmAddrAndVersionAndState.load();
      vmAddrAndVersionAndState.store(nextVersion(getVMAddress(v), getVersion(v), Evicted), std::memory_order_release);
   }

   void unlockXInvalidated() {
      TABBY_ASSERT2(getState() == Locked, "unlockXInvalidated");
      auto v = vmAddrAndVersionAndState.load();
      vmAddrAndVersionAndState.store(nextVersion(kTabbyInvalidAddr, getVersion(v), Unlocked), std::memory_order_release);
   }

   void downgradeLock() { // downgrade from X-lock to S-lock
      TABBY_ASSERT(getState() == Locked);
      auto v = vmAddrAndVersionAndState.load();
      vmAddrAndVersionAndState.store(nextVersion(getVMAddress(v), getVersion(v), 1), // We are the only reader
                                     std::memory_order_release);
   }

   bool tryLockS(u64 oldStateAndVersion) {
      auto v = oldStateAndVersion;
      u64 s = getState(v);

      if (s<MaxShared)
         return vmAddrAndVersionAndState.compare_exchange_strong(v, sameVersion(getVMAddress(v), getVersion(v), s+1));
      if (s==Marked)
         return vmAddrAndVersionAndState.compare_exchange_strong(v, sameVersion(getVMAddress(v), getVersion(v), 1));
      return false;
   }

   void unlockS() {
      while (true) {
         u64 v = vmAddrAndVersionAndState.load();
         u64 state = getState(v);
         if (!(state>0 && state<=MaxShared)) {
            std::cerr << "unlockS state " << state  << " vmaddr " << getVMAddress(v) << std::endl;
         }
         TABBY_ASSERT(state>0 && state<=MaxShared);
         if (vmAddrAndVersionAndState.compare_exchange_strong(v, sameVersion(getVMAddress(v), getVersion(v), state-1)))
            return;
      }
   }

   bool tryMark(u64 oldStateAndVersion) {
      u64 v = oldStateAndVersion;
      assert(getState(v)==Unlocked);
      return vmAddrAndVersionAndState.compare_exchange_strong(v, sameVersion(getVMAddress(v), getVersion(v), Marked));
   }

   static u64 getState(u64 v) { return (v & kTabbyStateMask) >> 20; };
   static u64 getVersion(u64 v) { return v & kTabbyVersionMask; };
   static u64 getVMAddress(u64 v) { return decompressVMAddress(v >> 28); };
   u64 getState() { return getState(vmAddrAndVersionAndState.load()); }
   u64 getVMAddress() { return getVMAddress(vmAddrAndVersionAndState.load()); }
   u64 getVersion() { return getVersion(vmAddrAndVersionAndState.load()); }

   void operator=(VMPageState&) = delete;

   u64 getWord() { return vmAddrAndVersionAndState.load(); }

   std::string ToString() {
      return "Raw: " + std::to_string(getWord()) + ", VMAddress " + std::to_string(getVMAddress()) + ", version " + std::to_string(getVersion()) + ", state " + std::to_string(getState());
   }
};

struct alignas(4096) Page {
   VMPageState state;
   bool dirty;
};



static const u64 metadataPageId = 0;

struct alignas(4096) MetaDataPage {
   VMPageState state;
   bool dirty;
   PID roots[(pageSize- sizeof(VMPageState) - sizeof(dirty))/8];

   PID getRoot(unsigned slot) { return roots[slot]; }
};

// struct PageState {
//    atomic<u64> stateAndVersion;

//    static const u64 Unlocked = 0;
//    static const u64 MaxShared = 252;
//    static const u64 Locked = 253;
//    static const u64 Marked = 254;
//    static const u64 Evicted = 255;

//    PageState() {}

//    void init() { stateAndVersion.store(sameVersion(0, Evicted), std::memory_order_release); }

//    static inline u64 sameVersion(u64 oldStateAndVersion, u64 newState) { return ((oldStateAndVersion<<8)>>8) | newState<<56; }
//    static inline u64 nextVersion(u64 oldStateAndVersion, u64 newState) { return (((oldStateAndVersion<<8)>>8)+1) | newState<<56; }

//    bool tryLockX(u64 oldStateAndVersion) {
//       return stateAndVersion.compare_exchange_strong(oldStateAndVersion, sameVersion(oldStateAndVersion, Locked));
//    }

//    void unlockX() {
//       assert(getState() == Locked);
//       stateAndVersion.store(nextVersion(stateAndVersion.load(), Unlocked), std::memory_order_release);
//    }

//    void unlockXEvicted() {
//       assert(getState() == Locked);
//       stateAndVersion.store(nextVersion(stateAndVersion.load(), Evicted), std::memory_order_release);
//    }

//    void downgradeLock() {
//       assert(getState() == Locked);
//       stateAndVersion.store(nextVersion(stateAndVersion.load(), 1), std::memory_order_release);
//    }

//    bool tryLockS(u64 oldStateAndVersion) {
//       u64 s = getState(oldStateAndVersion);
//       if (s<MaxShared)
//          return stateAndVersion.compare_exchange_strong(oldStateAndVersion, sameVersion(oldStateAndVersion, s+1));
//       if (s==Marked)
//          return stateAndVersion.compare_exchange_strong(oldStateAndVersion, sameVersion(oldStateAndVersion, 1));
//       return false;
//    }

//    void unlockS() {
//       while (true) {
//          u64 oldStateAndVersion = stateAndVersion.load();
//          u64 state = getState(oldStateAndVersion);
//          assert(state>0 && state<=MaxShared);
//          if (stateAndVersion.compare_exchange_strong(oldStateAndVersion, sameVersion(oldStateAndVersion, state-1)))
//             return;
//       }
//    }

//    bool tryMark(u64 oldStateAndVersion) {
//       assert(getState(oldStateAndVersion)==Unlocked);
//       return stateAndVersion.compare_exchange_strong(oldStateAndVersion, sameVersion(oldStateAndVersion, Marked));
//    }

//    static u64 getState(u64 v) { return v >> 56; };
//    u64 getState() { return getState(stateAndVersion.load()); }

//    void operator=(PageState&) = delete;
// };

// // open addressing hash table used for second chance replacement to keep track of currently-cached pages
// struct ResidentPageSet {
//    static const u64 empty = ~0ull;
//    static const u64 tombstone = (~0ull)-1;

//    struct Entry {
//       atomic<u64> pid;
//    };

//    Entry* ht;
//    u64 count;
//    u64 mask;
//    atomic<u64> clockPos;

//    ResidentPageSet(u64 maxCount) : count(next_pow2(maxCount * 1.5)), mask(count - 1), clockPos(0) {
//       ht = (Entry*)allocHuge(count * sizeof(Entry));
//       memset((void*)ht, 0xFF, count * sizeof(Entry));
//    }

//    ~ResidentPageSet() {
//       munmap(ht, count * sizeof(u64));
//    }

//    u64 next_pow2(u64 x) {
//       return 1<<(64-__builtin_clzl(x-1));
//    }

//    u64 hash(u64 k) {
//       const u64 m = 0xc6a4a7935bd1e995;
//       const int r = 47;
//       u64 h = 0x8445d61a4e774912 ^ (8*m);
//       k *= m;
//       k ^= k >> r;
//       k *= m;
//       h ^= k;
//       h *= m;
//       h ^= h >> r;
//       h *= m;
//       h ^= h >> r;
//       return h;
//    }

//    void insert(u64 pid) {
//       u64 pos = hash(pid) & mask;
//       while (true) {
//          u64 curr = ht[pos].pid.load();
//          assert(curr != pid);
//          if ((curr == empty) || (curr == tombstone))
//             if (ht[pos].pid.compare_exchange_strong(curr, pid))
//                return;

//          pos = (pos + 1) & mask;
//       }
//    }

//    bool remove(u64 pid) {
//       u64 pos = hash(pid) & mask;
//       while (true) {
//          u64 curr = ht[pos].pid.load();
//          if (curr == empty)
//             return false;

//          if (curr == pid)
//             if (ht[pos].pid.compare_exchange_strong(curr, tombstone))
//                return true;

//          pos = (pos + 1) & mask;
//       }
//    }

//    template<class Fn>
//    void iterateClockBatch(u64 batch, Fn fn) {
//       u64 pos, newPos;
//       do {
//          pos = clockPos.load();
//          newPos = (pos+batch) % count;
//       } while (!clockPos.compare_exchange_strong(pos, newPos));

//       for (u64 i=0; i<batch; i++) {
//          u64 curr = ht[pos].pid.load();
//          if ((curr != tombstone) && (curr != empty))
//             fn(curr);
//          pos = (pos + 1) & mask;
//       }
//    }
// };

// libaio interface used to write batches of pages
struct LibaioInterface {
   static const u64 maxIOs = 256;

   std::vector<int> blockfds;
   Page* virtMem;
   io_context_t ctx;
   iocb cb[maxIOs];
   iocb* cbPtr[maxIOs];
   io_event events[maxIOs];

   LibaioInterface(const std::vector<int> & blockfds, Page* virtMem) : blockfds(blockfds), virtMem(virtMem) {
      memset(&ctx, 0, sizeof(io_context_t));
      int ret;
      if ((ret = io_setup(maxIOs, &ctx)) != 0) {
         printf("io_setup %s\n", strerror(-ret));
         exit(EXIT_FAILURE);
      }
   }

   // void writePages(const vector<PID>& pages) {
   //    assert(pages.size() < maxIOs);
   //    for (u64 i=0; i<pages.size(); i++) {
   //       PID pid = pages[i];
   //       virtMem[pid].dirty = false;
   //       cbPtr[i] = &cb[i];
   //       io_prep_pwrite(cb+i, blockfd, &virtMem[pid], pageSize, pageSize*pid);
   //    }
   //    int cnt = io_submit(ctx, pages.size(), cbPtr);
   //    assert(cnt == pages.size());
   //    cnt = io_getevents(ctx, pages.size(), pages.size(), events, nullptr);
   //    assert(cnt == pages.size());
   // }

   void writePages(const vector<Page*>& pages) {
      // int cpu_id = dune_get_cpu_id();
      // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
      //    dune_printf("cpu_id %d writePages %d\n", cpu_id, pages.size());		
      // }
      TABBY_ASSERT(pages.size() < maxIOs);
      for (u64 i=0; i<pages.size(); i++) {
         Page * page = pages[i];
         page->dirty = false;
         TABBY_ASSERT(page->state.getVMAddress() != kTabbyInvalidAddr);
         u64 offset = page->state.getVMAddress() - kTabbyAddressSpaceStart;
         TABBY_ASSERT(page->state.getState() == VMPageState::Locked);
         TABBY_ASSERT2((offset & (pageSize - 1)) == 0, std::to_string(offset).c_str());
         cbPtr[i] = &cb[i];
         PID pid = offset / pageSize;
         int blockfd = blockfds[pid % blockfds.size()];
         PID page_id_in_file = pid / blockfds.size();
         io_prep_pwrite(cb+i, blockfd, page, pageSize, page_id_in_file * pageSize);
      }
      int cnt = io_submit(ctx, pages.size(), cbPtr);
      TABBY_ASSERT(cnt == pages.size());
      cnt = io_getevents(ctx, pages.size(), pages.size(), events, nullptr);
      TABBY_ASSERT(cnt == pages.size());
      // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
      //    dune_printf("cpu_id %d writePages %d done\n", cpu_id, pages.size());		
      // }
   }

   ssize_t pread(PID pid, void * __buf, size_t __nbytes) {
      assert(__nbytes == pageSize);
      int blockfd = blockfds[pid % blockfds.size()];
      int page_id_in_file = pid / blockfds.size();
      return ::pread(blockfd, __buf, __nbytes, pageSize * page_id_in_file);
   }
};


// struct IOUringInterface {
//    static const u64 QUEUE_DEPTH = 64;
//    struct io_uring ring;
//    int blockfd;
//    Page* virtMem;
//    IOUringInterface(int blockfd, Page* virtMem) : blockfd(blockfd), virtMem(virtMem) {
//       if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0)) {
//          die("io_uring_queue_init");
//       }
//       if (io_uring_register_files(&ring, &blockfd, 1)) {
//          die("io_uring_register_files");
//       }
//    }

//    ssize_t pread(int __fd, void * __buf, size_t __nbytes, __off_t __offset) {
//       struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
//       /* Setup a pread operation */
//       io_uring_prep_read(sqe, __fd, __buf, __nbytes, __offset);
//       /* Set user data */
//       io_uring_sqe_set_data(sqe, NULL);
//       /* Finally, submit the request */
//       if (io_uring_submit_and_wait(&ring, 1) <= 0) {
//          die("io_uring_submit_and_wait");
//       }
//       struct io_uring_cqe *cqe;
//       ssize_t ret = io_uring_wait_cqe(&ring, &cqe);

//       if (ret != 0) {
//          die("io_uring_wait_cqe");
//       }
//       ret = cqe->res;
//       /* Mark this completion as seen */
//       io_uring_cqe_seen(&ring, cqe);
//       return ret;
//    }
// };

#ifndef TABBY_ALLOCATOR_STRIPED_LOCK  // Whether to enable stripped locks optimization
static constexpr u64 kLocalFreeListSoftLimit = 64;
static constexpr u64 kNumGlobalFreeLists = 1;
#else
static constexpr u64 kLocalFreeListSoftLimit = 64;
static constexpr u64 kNumGlobalFreeLists = 64;
#endif

struct FramesMemoryManager {
   Page* framesMem;
   u64 physCount;
   atomic<u64> clockPos;
   // std::mutex slotMutexes[kNumGlobalFreeLists];
   // vector<Page*> globalFreeLists[kNumGlobalFreeLists]; 
   struct alignas(64)  FreeList {
      std::mutex mtx;
      // spinlock mtx;
      std::vector<Page*> list;
      int count;
      FreeList() {
         // cerr << "mtx " << &mtx << endl;
         count = 0;
      }
      void push(Page * p) {
         list.push_back(p);
         count = list.size();
      }
      Page* pop() {
         Page * p = list.back();
         list.pop_back();
         count = list.size();
         return p;
      }
   };
   FreeList freelists[kNumGlobalFreeLists];
   // thread_local static FreeList freelist;
   thread_local static int last_stolen;


   std::mutex manager_mtx;

   // void returnFramesToGlobalList(const std::vector<Page*> & list,  int maxToFree) {
   //    int cpu_id = dune_get_cpu_id();
   //    if ((slotMutexes[cpu_id].try_lock())) {
   //       for (int i = list.size() - 1; i >= 0 && maxToFree--; --i) {
   //          globalFreeLists[cpu_id].push_back(list[i]);
   //       }
   //       slotMutexes[cpu_id].unlock();
   //       return;
   //    }
   //    retry:
   //    u64 slot = last_stolen != -1 ? last_stolen : RandomGenerator::getRand(0, 1024) % kNumGlobalFreeLists;
   //    if ((slotMutexes[slot].try_lock())) {
   //       for (int i = list.size() - 1; i >= 0 && maxToFree--; --i) {
   //          globalFreeLists[slot].push_back(list[i]);
   //       }
   //       last_stolen = slot;
   //       slotMutexes[slot].unlock();
   //    } else {
   //       last_stolen = -1;
   //       goto retry;
   //    }
   // }

   
   FramesMemoryManager(Page* framesMem, u64 physCount): framesMem(framesMem), physCount(physCount), clockPos(0) {
      for (u64 i=0; i < physCount; i++){
         framesMem[i].state.init(kTabbyInvalidAddr);
         freelists[i%kNumGlobalFreeLists].push(framesMem+i);
      }
   }

   bool steal_pages(vector<Page*> & container, int slot, int max_to_steal) {
      assert(container.empty());
      int cpu_id = dune_get_cpu_id() % kNumGlobalFreeLists;
      // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
      //    dune_printf("cpu_id %d acquired mutex %p steal_pages 1\n", cpu_id, &freelists[cpu_id].mtx);		
      // }
      if (freelists[slot].mtx.try_lock()) { // Fail if the mutex is locked to avoid contention
         // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
         //    dune_printf("cpu_id %d acquired mutex %p steal_pages 2\n", cpu_id, &freelists[cpu_id].mtx);		
         // }
         while (freelists[slot].list.size() > 0 && max_to_steal--) {
            container.push_back(freelists[slot].pop());
         }
         freelists[slot].mtx.unlock();
         return container.empty() == false;
      }
      return false;
   }

   Page* allocFrame() {
      int cpu_id = dune_get_cpu_id() % kNumGlobalFreeLists;
      // if (cpu_id >= kNumGlobalFreeLists) {
      //    cerr << "cpu_id " << cpu_id << " >= kNumGlobalFreeLists " << kNumGlobalFreeLists << endl; 
      // }
      // #ifndef TABBY_ALLOCATOR_STRIPED_LOCK  // Whether to enable stripped locks optimization
      // std::unique_lock<std::mutex> gg(manager_mtx);
      // #endif
      assert(cpu_id < kNumGlobalFreeLists);
      // try local list
      if (freelists[cpu_id].count > 0) {
         // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
         //    dune_printf("cpu_id %d acquired mutex %p allocFrame 31\n", cpu_id, &freelists[cpu_id].mtx);		
         // }
         // std::unique_lock<spinlock> g(freelists[cpu_id].mtx);
         std::unique_lock<std::mutex> g(freelists[cpu_id].mtx);
         // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
         //    dune_printf("cpu_id %d acquired mutex %p allocFrame 32\n", cpu_id, &freelists[cpu_id].mtx);		
         // }
         if (freelists[cpu_id].count > 0) {
            Page * p = freelists[cpu_id].pop();
            return p;
         }
      }
      
      if (kNumGlobalFreeLists == 1) {
         assert(false);
         return NULL;
      }
      std::vector<Page*> container;
      container.reserve(kLocalFreeListSoftLimit);
      // try last place we stole from
      if (last_stolen != -1 && freelists[last_stolen].count > 0) {
         if (steal_pages(container, last_stolen, kLocalFreeListSoftLimit)) {
            assert(container.empty() == false);
            // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
            //    dune_printf("cpu_id %d acquired mutex %p  allocFrame 11\n", cpu_id, &freelists[cpu_id].mtx);		
            // }
            // std::unique_lock<spinlock> g(freelists[cpu_id].mtx);
            std::unique_lock<std::mutex> g(freelists[cpu_id].mtx);
            // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
            //    dune_printf("cpu_id %d acquired mutex %p  allocFrame 12\n", cpu_id, &freelists[cpu_id].mtx);		
            // }
            for (Page * p : container) {
               freelists[cpu_id].push(p);
            }
            Page * p = freelists[cpu_id].pop();
            return p;
         }
      }

      last_stolen = -1;

      // Power of two choices
      while (last_stolen == -1) {
         int slot1 = RandomGenerator::getRand(0, 1024) % kNumGlobalFreeLists;
         int slot2 = RandomGenerator::getRand(0, 1024) % kNumGlobalFreeLists;
         if (slot1 > slot2) {
            std::swap(slot1, slot2);
         }
         int size1 = freelists[slot1].count;
         int size2 = freelists[slot2].count;
         int slot = size1 > size2 ? slot1: slot2;

         if (steal_pages(container, slot, kLocalFreeListSoftLimit)) {
            assert(container.empty() == false);
            last_stolen = slot;
            // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
            //    dune_printf("cpu_id %d acquired mutex %p allocFrame 21\n", cpu_id, &freelists[cpu_id].mtx);		
            // }
            // std::unique_lock<spinlock> g(freelists[cpu_id].mtx);
            std::unique_lock<std::mutex> g(freelists[cpu_id].mtx);
            // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
            //    dune_printf("cpu_id %d acquired mutex %p allocFrame 22\n", cpu_id, &freelists[cpu_id].mtx);		
            // }
            for (Page * p : container) {
               freelists[cpu_id].push(p);
            }
            Page * p = freelists[cpu_id].pop();
            return p;
         }
      }
      assert(false);
      return NULL;
   }

   void freeFrames(const std::vector<Page*> & pages) {
      // #ifndef TABBY_ALLOCATOR_STRIPED_LOCK
      // std::unique_lock<std::mutex> gg(manager_mtx);
      // #endif
      int cpu_id = dune_get_cpu_id() % kNumGlobalFreeLists;
      // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
      //    dune_printf("cpu_id %d acquired mutex %p freeFrames 11, freeing %d pages\n", cpu_id, &freelists[cpu_id].mtx, pages.size());		
      // }
      // std::unique_lock<spinlock> g(freelists[cpu_id].mtx);
      std::unique_lock<std::mutex> g(freelists[cpu_id].mtx);
      for (Page * p : pages) {
         freelists[cpu_id].push(p);
      }
      // if (cpu_id == 12 && dune_cnt >= 500000 && dune_cnt < 500500) {
      //    dune_printf("cpu_id %d acquired mutex %p freeFrames 12, freed %d pages\n", cpu_id, &freelists[cpu_id].mtx, pages.size());		
      // }
   }

   void freeFrame(Page* page) {
      // #ifndef TABBY_ALLOCATOR_STRIPED_LOCK
      // std::unique_lock<std::mutex> gg(manager_mtx);
      // #endif
      int cpu_id = dune_get_cpu_id() % kNumGlobalFreeLists;
      // std::unique_lock<spinlock> g(freelists[cpu_id].mtx);
      std::unique_lock<std::mutex> g(freelists[cpu_id].mtx);
      freelists[cpu_id].push(page);
   }

   template<class Fn>
   void iterateClockBatch(u64 batch, Fn fn) {
      u64 pos, newPos;
      do {
         pos = clockPos.load();
         newPos = (pos+batch) % physCount;
      } while (!clockPos.compare_exchange_strong(pos, newPos));
      //pos = RandomGenerator::getRand<u64>(0, physCount);
      for (u64 i=0; i<batch; i++) {
         fn(&framesMem[pos]);
         pos = (pos + 1) % physCount;
      }
   }
};

thread_local int FramesMemoryManager::last_stolen = -1;

struct BufferManager {
   static const u64 mb = 1024ull * 1024;
   static const u64 gb = 1024ull * 1024 * 1024;
   u64 virtSize;
   u64 physSize;
   u64 virtCount;
   u64 physCount;
   vector<LibaioInterface> libaioInterface;
   //vector<IOUringInterface> iouringInterface;
   std::vector<int> blockfds;

   atomic<u64> physUsedCount;
   //ResidentPageSet residentSet;
   atomic<u64> allocCount;

   atomic<u64> readCount;
   atomic<u64> writeCount;

   // virtual memmory address range that maps to the storage space
   Page* virtMem; 
   // virtual memory address range that maps to all the buffer frames. 
   // This is mostly for ease of iterating over all the buffer frames 
   // without using a auxilary data structures such as a hash map.
   Page* framesMem; 
   FramesMemoryManager * frameMemManager;
   //PageState* pageState;
   u64 batch;

   VMPageState& getPageState(PID pid) {
      //return pageState[pid];
      //cerr << "getPageState pid " << pid << " page vm address " << &virtMem[pid] << endl;
      return virtMem[pid].state;
   }

   BufferManager();
   ~BufferManager() {}

   void init();

   Page* fixX(PID pid);
   void unfixX(PID pid);
   Page* fixS(PID pid);
   void unfixS(PID pid);

   bool isValidPtr(void* page) { return (page >= virtMem) && (page < (virtMem + virtSize + 16)); }
   PID toPID(void* page) { return reinterpret_cast<Page*>(page) - virtMem; }
   Page* toPtr(PID pid) { return virtMem + pid; }

   void ensureFreePages();
   Page* allocPage();
   void handlePageFault(uint64_t addr, bool pf, bool newpage);
   void handleFault(PID pid);
   //void handlePageFault(uint64_t addr);
   void readPage(PID pid);
   void evict();
};

BufferManager * bm = nullptr;

struct OLCRestartException {};

template<class T>
struct GuardO {
   PID pid;
   T* ptr;
   u64 version;
   static const u64 moved = ~0ull;

   // constructor
   explicit GuardO(u64 pid) : pid(pid), ptr(reinterpret_cast<T*>(bm->toPtr(pid))) {
      init();
   }

   template<class T2>
   GuardO(u64 pid, GuardO<T2>& parent)  {
      parent.checkVersionAndRestart();
      this->pid = pid;
      ptr = reinterpret_cast<T*>(bm->toPtr(pid));
      init();
   }

   GuardO(GuardO&& other) {
      pid = other.pid;
      ptr = other.ptr;
      version = other.version;
   }

   void init() {
      assert(pid != moved);
      //VMPageState& ps = bm->getPageState(pid);
      u64 expectedVMAddress = reinterpret_cast<u64>(ptr);
      for (u64 repeatCounter=0; ; repeatCounter++) {
         //u64 v = ps.stateAndVersion.load();
         VMPageState& ps = bm->getPageState(pid); // This access could trigger page-fault handler
         u64 v = ps.getWord(); 
         if (VMPageState::getVMAddress(v) != expectedVMAddress) {
            bm->handlePageFault(expectedVMAddress, false, false);
            continue;
         }
         assert(VMPageState::getState(v) != VMPageState::Evicted);
         switch (VMPageState::getState(v)) {
            case VMPageState::Marked: {
               u64 newV = VMPageState::sameVersion(VMPageState::getVMAddress(v), VMPageState::getVersion(v), VMPageState::Unlocked);
               if (ps.vmAddrAndVersionAndState.compare_exchange_weak(v, newV)) {
                  version = newV;
                  return;
               }
               break;
            }
            case VMPageState::Locked:
               break;
            // case PageState::Evicted:
            //    if (ps.tryLockX(v)) {
            //       bm->handleFault(pid);
            //       bm->unfixX(pid);
            //    }
            //    break;
            case VMPageState::Evicted:
               assert(false);
               break;
            default:
               version = v;
               return;
         }
         yield(repeatCounter);
      }
   }

   // move assignment operator
   GuardO& operator=(GuardO&& other) {
      if (pid != moved)
         checkVersionAndRestart();
      pid = other.pid;
      ptr = other.ptr;
      version = other.version;
      other.pid = moved;
      other.ptr = nullptr;
      return *this;
   }

   // assignment operator
   GuardO& operator=(const GuardO&) = delete;

   // copy constructor
   GuardO(const GuardO&) = delete;

   void checkVersionAndRestart() {
      if (pid != moved) {
         u64 expectedVMAddress = reinterpret_cast<u64>(ptr);
         VMPageState& ps = bm->getPageState(pid);
         u64 stateAndVersion = ps.getWord();
         if (version == stateAndVersion) // fast path, nothing changed
            return;

         //if ((stateAndVersion<<8) == (version<<8)) { // same version
         if (VMPageState::getVMAddress(stateAndVersion) == expectedVMAddress &&
             VMPageState::getVersion(stateAndVersion) == VMPageState::getVersion(version)) {
            u64 state = VMPageState::getState(stateAndVersion);
            if (state <= VMPageState::MaxShared)
               return; // ignore shared locks
            if (state == VMPageState::Marked)
               if (ps.vmAddrAndVersionAndState.compare_exchange_weak(stateAndVersion, 
                  VMPageState::sameVersion(VMPageState::getVMAddress(stateAndVersion), VMPageState::getVersion(stateAndVersion), VMPageState::Unlocked)))
                  return; // mark cleared
         }
         if (std::uncaught_exceptions()==0)
            throw OLCRestartException();
      }
   }

   // destructor
   ~GuardO() noexcept(false) {
      checkVersionAndRestart();
   }

   T* operator->() {
      assert(pid != moved);
      return ptr;
   }

   void release() {
      checkVersionAndRestart();
      pid = moved;
      ptr = nullptr;
   }
};

template<class T>
struct GuardX {
   PID pid;
   T* ptr;
   static const u64 moved = ~0ull;

   // constructor
   GuardX(): pid(moved), ptr(nullptr) {}

   // constructor
   explicit GuardX(u64 pid) : pid(pid) {
      ptr = reinterpret_cast<T*>(bm->fixX(pid));
      ptr->dirty = true;
   }

   explicit GuardX(GuardO<T>&& other) {
      assert(other.pid != moved);
      u64 expectedVMAddress = reinterpret_cast<u64>(other.ptr);
      for (u64 repeatCounter=0; ; repeatCounter++) {
         //VMPageState& ps = bm->getPageState(other.pid);
         // u64 stateAndVersion = ps.stateAndVersion;
         VMPageState& ps = bm->getPageState(other.pid); // This access could trigger page-fault handler
         u64 stateAndVersion = ps.getWord(); 
         if (VMPageState::getVMAddress(stateAndVersion) != expectedVMAddress) {
            bm->handlePageFault(expectedVMAddress, false, false);
            continue;
         }
         // if ((stateAndVersion<<8) != (other.version<<8))
         //    throw OLCRestartException();
         if (VMPageState::getVersion(stateAndVersion) != VMPageState::getVersion(other.version))
            throw OLCRestartException();
         assert(VMPageState::getState(stateAndVersion) != VMPageState::Evicted);
         u64 state = VMPageState::getState(stateAndVersion);
         if ((state == VMPageState::Unlocked) || (state == VMPageState::Marked)) {
            if (ps.tryLockX(stateAndVersion)) {
               pid = other.pid;
               ptr = other.ptr;
               ptr->dirty = true;
               other.pid = moved;
               other.ptr = nullptr;
               return;
            }
         }
         yield(repeatCounter);
      }
   }

   // assignment operator
   GuardX& operator=(const GuardX&) = delete;

   // move assignment operator
   GuardX& operator=(GuardX&& other) {
      if (pid != moved) {
         bm->unfixX(pid);
      }
      pid = other.pid;
      ptr = other.ptr;
      other.pid = moved;
      other.ptr = nullptr;
      return *this;
   }

   // copy constructor
   GuardX(const GuardX&) = delete;

   // destructor
   ~GuardX() {
      //cerr << "GuardX destructor pid " << pid << endl;
      if (pid != moved)
         bm->unfixX(pid);
   }

   T* operator->() {
      assert(pid != moved);
      return ptr;
   }

   void release() {
      if (pid != moved) {
         bm->unfixX(pid);
         pid = moved;
      }
   }
};

template<class T>
struct AllocGuard : public GuardX<T> {
   template <typename ...Params>
   AllocGuard(Params&&... params) {
      GuardX<T>::ptr = reinterpret_cast<T*>(bm->allocPage());
      T*p = GuardX<T>::ptr;
      new (GuardX<T>::ptr) T(std::forward<Params>(params)...);
      p->state.init((u64)(p));
      u64 stateAndVersion = p->state.getWord();
      bool succ = p->state.tryLockX(stateAndVersion);
      TABBY_ASSERT(succ);
      TABBY_ASSERT(p->state.getState() == VMPageState::Locked);
      p->dirty = true;
      GuardX<T>::pid = bm->toPID(GuardX<T>::ptr);
   }
};

template<class T>
struct GuardS {
   PID pid;
   T* ptr;
   static const u64 moved = ~0ull;

   // constructor
   explicit GuardS(u64 pid) : pid(pid) {
      ptr = reinterpret_cast<T*>(bm->fixS(pid));
   }

   GuardS(GuardO<T>&& other) {
      assert(other.pid != moved);
      //cerr << "GuardS, pid " << other.pid << endl;
      u64 expectedVMAddress = reinterpret_cast<u64>(other.ptr);
      for (u64 repeatCounter=0; ; repeatCounter++) {
         VMPageState& ps = bm->getPageState(other.pid); // This access could trigger page-fault handler
         //cerr << "GuardS, pid &ps = " << &ps << endl;
         u64 stateAndVersion = ps.getWord(); 
         if (VMPageState::getVMAddress(stateAndVersion) != expectedVMAddress) {
            bm->handlePageFault(expectedVMAddress, false, false);
            continue;
         }
      
         // if ((stateAndVersion<<8) != (other.version<<8))
         //    throw OLCRestartException();
         if (VMPageState::getVersion(stateAndVersion) != VMPageState::getVersion(other.version))
            throw OLCRestartException();
         assert(VMPageState::getState(stateAndVersion) != VMPageState::Evicted);
         // u64 state = VMPageState::getState(stateAndVersion);
         // if ((state == VMPageState::Unlocked) || (state == VMPageState::Marked)) {
         //    if (ps.tryLockX(stateAndVersion)) {
         //       pid = other.pid;
         //       ptr = other.ptr;
         //       ptr->dirty = true;
         //       other.pid = moved;
         //       other.ptr = nullptr;
         //       return;
         //    }
         // }
         if (ps.tryLockS(stateAndVersion)) { // XXX: optimize?
            pid = other.pid;
            ptr = other.ptr;
            other.pid = moved;
            other.ptr = nullptr;
            return;
         } else {
            throw OLCRestartException();
         }
         yield(repeatCounter);
      }

      // assert(other.pid != moved);
      // if (bm->getPageState(other.pid).tryLockS(other.version)) { // XXX: optimize?
      //    pid = other.pid;
      //    ptr = other.ptr;
      //    other.pid = moved;
      //    other.ptr = nullptr;
      // } else {
      //    throw OLCRestartException();
      // }
   }

   GuardS(GuardS&& other) {
      if (pid != moved)
         bm->unfixS(pid);
      pid = other.pid;
      ptr = other.ptr;
      other.pid = moved;
      other.ptr = nullptr;
   }

   // assignment operator
   GuardS& operator=(const GuardS&) = delete;

   // move assignment operator
   GuardS& operator=(GuardS&& other) {
      if (pid != moved)
         bm->unfixS(pid);
      pid = other.pid;
      ptr = other.ptr;
      other.pid = moved;
      other.ptr = nullptr;
      return *this;
   }

   // copy constructor
   GuardS(const GuardS&) = delete;

   // destructor
   ~GuardS() {
      if (pid != moved)
         bm->unfixS(pid);
   }

   T* operator->() {
      assert(pid != moved);
      return ptr;
   }

   void release() {
      if (pid != moved) {
         bm->unfixS(pid);
         pid = moved;
      }
   }
};

u64 envOr(const char* env, u64 value) {
   if (getenv(env))
      return atof(getenv(env));
   return value;
}

static int page_walk_fill(const void *arg, ptent_t *ptep, void *va) {
   (*(u64*)arg)++;
   *ptep = 0;
   dune_flush_tlb_one((unsigned long)va);
   return 0;
}

static int page_walk_fill(const void *arg, ptent_t *ptep, void *va, int create) {
   (*(u64*)arg)++;
   *ptep = 0;
   dune_flush_tlb_one((unsigned long)va);
   return 0;
}

// for string delimiter
std::vector<std::string> split(std::string s, std::string delimiter) {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }

    res.push_back (s.substr (pos_start));
    return res;
}

BufferManager::BufferManager() : virtSize(envOr("VIRTGB", 16)*gb), physSize(envOr("PHYSGB", 4)*gb), virtCount(virtSize / pageSize), physCount(physSize / pageSize) {
   assert(virtSize>=physSize);
   const char* path = getenv("BLOCK") ? getenv("BLOCK") : "/tmp/bm";
   std::vector<std::string> paths = split(path, ",");
   //blockfd = open(path, O_RDWR | O_DIRECT, S_IRWXU);
   for (size_t i = 0; i < paths.size(); ++i) {
      int blockfd = open(paths[i].c_str(), O_RDWR | O_DIRECT, S_IRWXU);
      if (blockfd == -1) {
         cerr << "cannot open BLOCK device '" << paths[i] << "'" << endl;
         exit(EXIT_FAILURE);
      }
      blockfds.push_back(blockfd);
   }
   
   u64 virtAllocSize = virtSize + (1<<16); // we allocate 64KB extra to prevent segfaults during optimistic reads

   //virtMem = (Page*)mmap(NULL, virtAllocSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
   //madvise(virtMem, virtAllocSize, MADV_NOHUGEPAGE);
   virtMem = (Page*)(kTabbyAddressSpaceStart);

   // Fill the page table entries with zero for pages between [kTabbyAddressSpaceStart, kTabbyAddressSpaceStart + virtAllocSize)
   u64 counter = 0;
   // dune_vm_page_walk_fill(pgroot, (void *) kTabbyAddressSpaceStart, (void*)(kTabbyAddressSpaceStart + virtAllocSize), page_walk_fill, &counter, CREATE_NORMAL);
   dune_vm_page_walk_fill(pgroot, (void *) kTabbyAddressSpaceStart, (void*)(kTabbyAddressSpaceStart + virtAllocSize), page_walk_fill, &counter, CREATE_NORMAL);


   cerr << "virtMem " << (Page*)kTabbyAddressSpaceStart << ", initialized " << counter << " PTEs" << endl;
   // pageState = (PageState*)allocHuge(virtCount * sizeof(PageState));
   // for (u64 i=0; i<virtCount; i++)
   //    pageState[i].init();
   // if (virtMem == MAP_FAILED)
   //    die("mmap failed");
   auto nthreads = envOr("THREADS", 1);
   libaioInterface.reserve(maxWorkerThreads);
   for (unsigned i=0; i<maxWorkerThreads; i++)
      libaioInterface.emplace_back(LibaioInterface(blockfds, virtMem));
   // iouringInterface.reserve(maxWorkerThreads);
   // for (unsigned i=0; i<nthreads; i++)
   //    iouringInterface.emplace_back(IOUringInterface(blockfd, virtMem));
   physUsedCount = 0;
   allocCount = 0; // pid 0 reserved for meta data
   readCount = 0;
   writeCount = 0;
   //batch = envOr("BATCH", 128);
   batch = envOr("BATCH", 64);

   cerr << "tabby " << "blk:" << path << " virtgb:" << virtSize/gb << " physgb:" << physSize/gb << endl;

   framesMem = (Page*)mmap(NULL, physSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
   if (framesMem == MAP_FAILED) {
      die("mmap failed");
   }

   cerr << "bm ptr " << bm << endl;

   frameMemManager = new FramesMemoryManager(framesMem, physCount);
   cerr << "framesMem " << framesMem << ", initialization finished" << endl;
}

void BufferManager::init() {
   assert(allocCount.load() == 0);
   AllocGuard<MetaDataPage> md;
   //cerr << "Allocated metadata page at " << md.ptr << " state address " << (void*)&md->state << " vm address in state " << (void*)md->state.getVMAddress() << ", " << md->state.ToString() << endl;
   assert(allocCount.load() == 1);

}

void BufferManager::ensureFreePages() {
   while (physUsedCount >= physCount*0.97)
      evict();
}

// allocate a new page and x-fix it
Page* BufferManager::allocPage() {
   //cerr << "allocPage physUsedCount0 " <<endl;
   //physUsedCount++;
   //cerr << "allocPage physUsedCount1 " << physUsedCount <<endl;
   ensureFreePages();
   //cerr << "allocPage physUsedCount2 " << physUsedCount <<endl;
   u64 pid = allocCount++;
   //cerr << "allocPage pid allocated " << pid <<endl;
   if (pid >= virtCount) {
      cerr << "VIRTGB is too low" << endl;
      exit(EXIT_FAILURE);
   }
   handlePageFault((u64)(virtMem + pid), false, true);
   Page* p = &virtMem[pid];

   //cerr << "Return allocated page " << p << " state address " << &p->state << " " << p->state.ToString() <<endl;
   return p;
}

void BufferManager::handleFault(PID pid) {
   physUsedCount++;
   ensureFreePages();
   readPage(pid);
   //residentSet.insert(pid);
}
int hardware_page_faults = 0;
void BufferManager::handlePageFault(uint64_t addr, bool pf, bool newpage) {
   ensureFreePages();

   //cerr << "handlePageFault at " << (void*)addr << " pf " << pf << " newpage " << newpage << endl;
   uint64_t aligned_addr = addr & ~(4095);
   TABBY_ASSERT(addr >= kTabbyAddressSpaceStart);
   assert(aligned_addr >= kTabbyAddressSpaceStart);
   ptent_t * pte;
   ptent_t new_pte = 0;
   int ret = 0;
   {
      ret = dune_vm_lookup(pgroot, (void *) addr, CREATE_NONE, &pte);
      assert(!ret);
   }
   ptent_t old_pte = *pte;
   PID pid = (aligned_addr - kTabbyAddressSpaceStart) / pageSize;
   retry:
   // Take the write lock using PTE_USR1 bit
   if (!(old_pte & PTE_USR1) && std::atomic_ref(*pte).compare_exchange_strong(old_pte, old_pte | PTE_USR1)) {
      if (pf) { // If page fault, present bit must not be set.
         assert(!(*pte & PTE_P));
         ++hardware_page_faults;
      }
      if (!(*pte & PTE_P)) {
         assert(!(*pte & PTE_P));
         // Allocate a physical frame.
         Page * page = frameMemManager->allocFrame();
         if (page == NULL) {
            *pte = old_pte;
            goto retry;
         }
         #ifndef TABBY_PREALLOCATION // if pre-allocation optimization is not enabled, we simulate the cost of zeroing the page that is required in Linux. This is for ablation study.
         memset(page, 0, pageSize);
         #endif
         if (!newpage) {
            //ret = pread(blockfd, page, pageSize, aligned_addr - kTabbyAddressSpaceStart);
            ret = libaioInterface[workerThreadId].pread(pid, page, pageSize);
            //ret = iouringInterface[workerThreadId].pread(blockfd, page, pageSize, aligned_addr - kTabbyAddressSpaceStart);
            assert(ret==pageSize);
            TABBY_ASSERT(page->state.getVMAddress() == aligned_addr);
            TABBY_ASSERT(page->state.getState() == VMPageState::Locked);
         }
         new_pte = (PTE_FLAGS(old_pte) & (~PTE_USR1)) | PTE_P | PTE_W | PTE_ADDR(dune_va_to_pa((void *) page));
         //cerr << "handlePageFault " << (Page*)addr << " " << pf << " " << newpage << " aligned_addr " << (Page*)aligned_addr << " got " << (Page*)page->state.getVMAddress() << " old pte " << (void*)*pte << " new pte " << (void*)new_pte<< " allocated page " << page << " paddr " << (void*)dune_va_to_pa((void *) page) << endl;
         physUsedCount++;
      } else {
         dune_flush_tlb_one(aligned_addr);
         assert(!pf);
         // Page is present, check if it points to the correct physical frame.
         if (((VMPageState*)(aligned_addr))->getVMAddress() == aligned_addr) {
            new_pte = old_pte;
         } else {
            // Do an I/O read to bring in the correct page and update mapping
            Page * page = frameMemManager->allocFrame();
            if (page == NULL) {
               *pte = old_pte;
               goto retry;
            }
            #ifndef TABBY_PREALLOCATION // if pre-allocation optimization is not enabled, we simulate the cost of zeroing the page that is required in Linux. This is for ablation study.
            memset(page, 0, pageSize);
            #endif
            TABBY_ASSERT(page->state.getVMAddress() == kTabbyInvalidAddr);
            //ret = pread(blockfd, page, pageSize, aligned_addr - kTabbyAddressSpaceStart);
            //ret = iouringInterface[workerThreadId].pread(blockfd, page, pageSize, aligned_addr - kTabbyAddressSpaceStart);
            ret = libaioInterface[workerThreadId].pread(pid, page, pageSize);
            TABBY_ASSERT(ret==pageSize);
            if (page->state.getVMAddress() != aligned_addr) {
               cerr << "handlePageFault " << (Page*)addr << " " << pf << " " << newpage << " aligned_addr " << (Page*)aligned_addr << " got " << (Page*)page->state.getVMAddress() << " *pte " << (void*)*pte << endl;
               // using namespace backward;
               // StackTrace st; st.load_here(32);
               // Printer p; p.print(st);
               exit(EXIT_FAILURE);
            }
            TABBY_ASSERT(page->state.getVMAddress() == aligned_addr);
            TABBY_ASSERT(page->state.getState() == VMPageState::Locked);
            page->state.unlockX();
            new_pte = (PTE_FLAGS(old_pte) & (~PTE_USR1)) | PTE_P | PTE_W | PTE_ADDR(dune_va_to_pa((void *) page));
            //new_pte |= PTE_P | PTE_W | PTE_A | PTE_U | PTE_ADDR(dune_va_to_pa((void *) page));
            physUsedCount++;
            readCount++;
            //cerr << "handlePageFault " << (Page*)addr << " " << pf << " " << newpage << " aligned_addr " << (Page*)aligned_addr << " loaded frame " << (void *) page << endl;
         }
      }
      std::atomic_ref(*pte).store(new_pte);
   }
   dune_flush_tlb_one(addr);
}

Page* BufferManager::fixX(PID pid) {
   u64 expectedVMAddress = (u64)(virtMem + pid);
   for (u64 repeatCounter=0; ; repeatCounter++) {
      VMPageState& ps = getPageState(pid); // This access could trigger page-fault handler
      u64 stateAndVersion = ps.getWord(); 
      if (VMPageState::getVMAddress(stateAndVersion) != expectedVMAddress) {
         handlePageFault(expectedVMAddress, false, false);
         continue;
      }
      assert(VMPageState::getState(stateAndVersion) != VMPageState::Evicted);
      switch (VMPageState::getState(stateAndVersion)) {
         // case PageState::Evicted: {
         //    if (ps.tryLockX(stateAndVersion)) {
         //       handleFault(pid);
         //       return virtMem + pid;
         //    }
         //    break;
         // }
         case VMPageState::Evicted: // Impossible to be in Evicted state
         assert(false);
         break;
         case VMPageState::Marked: case VMPageState::Unlocked: {
            if (ps.tryLockX(stateAndVersion))
               return virtMem + pid;
            break;
         }
      }
      yield(repeatCounter);
   }
}

Page* BufferManager::fixS(PID pid) {
   u64 expectedVMAddress = (u64)(virtMem + pid);
   for (u64 repeatCounter=0; ; repeatCounter++) {
      VMPageState& ps = getPageState(pid); // This access could trigger page-fault handler
      u64 stateAndVersion = ps.getWord(); 
      if (VMPageState::getVMAddress(stateAndVersion) != expectedVMAddress) {
         handlePageFault(expectedVMAddress, false, false);
         continue;
      }
      switch (VMPageState::getState(stateAndVersion)) {
         case VMPageState::Locked: {
            break;
         } 
         // case VMPageState::Evicted: {
         //    if (ps.tryLockX(stateAndVersion)) {
         //       handleFault(pid);
         //       ps.unlockX();
         //    }
         //    break;
         // }
         default: {
            if (ps.tryLockS(stateAndVersion))
               return virtMem + pid;
         }
      }
      yield(repeatCounter);
   }
}

void BufferManager::unfixS(PID pid) {
   getPageState(pid).unlockS();
}

void BufferManager::unfixX(PID pid) {
   getPageState(pid).unlockX();
}

void BufferManager::readPage(PID pid) {
   readCount++;
}

atomic<long> evict_rounds{0};
atomic<long> evict_written_pages{0};
atomic<long> evict_clean_pages{0};
void BufferManager::evict() {
   //cerr << "BufferManager::evict" << endl;
   //vector<PID> toEvict;
   vector<Page*> toEvict;
   toEvict.reserve(batch);
   //vector<PID> toWrite;
   vector<Page*> toWrite;
   toWrite.reserve(batch);
   int loops = 0;
   int toEvictSize = 0;
   int toWriteSize = 0;
   int shared = 0;
   int locked = 0;
   int total = 0;
   // 0. find candidates, lock dirty ones in shared mode
   while (toEvict.size()+toWrite.size() < batch) {
      frameMemManager->iterateClockBatch(batch, [&](Page * page) {
         total++;
         VMPageState& ps = page->state;
         u64 v = ps.getWord();
         if (VMPageState::getVMAddress(v) == kTabbyInvalidAddr) // Skip free frames
            return;
         TABBY_ASSERT(VMPageState::getState(v) != VMPageState::Evicted);
         switch (VMPageState::getState(v)) {
            case VMPageState::Marked:
               if (page->dirty) {
                  if (ps.tryLockX(v))
                     toWrite.push_back(page);
               } else {
                  toEvict.push_back(page);
               }
               break;
            case VMPageState::Unlocked:
               ps.tryMark(v);
               break;
            case VMPageState::Locked:
               ++locked;
               break;
            default:
               ++shared;
               break; // skip
         };
      });
      ++loops;
      if (loops > 100000) {
         cerr << "evict scan failed to find enough pages physUsedCount " << physUsedCount << " physCount" << physCount << endl;
         loops = 0;
         // using namespace backward;
         // StackTrace st; st.load_here(32);
         // Printer p; p.print(st);
      }
   }

   //cerr << "evict scan found " << toWrite.size() << " dirty pages, " << toEvict.size() << " clean evictable pages" << endl;
   // 1. write dirty pages
   libaioInterface[workerThreadId].writePages(toWrite);
   writeCount += toWrite.size();

   toEvictSize = toEvict.size();
   toWriteSize = toWrite.size();
   // 2. try to lock clean page candidates
   toEvict.erase(std::remove_if(toEvict.begin(), toEvict.end(), [&](Page * page) {
      VMPageState& ps = page->state;
      u64 v = ps.getWord();
      return (VMPageState::getState(v) != VMPageState::Marked) || !ps.tryLockX(v);
   }), toEvict.end());


   evict_clean_pages += toEvict.size();

   evict_written_pages += toWrite.size();
   evict_rounds += 1;
   toEvict.insert(toEvict.end(), toWrite.begin(), toWrite.end());

   // 5. remove from hash table and unlock
   for (Page* page : toEvict) {
      VMPageState& ps = page->state;
      u64 v = ps.getWord();
      assert(VMPageState::getState(v) == VMPageState::Locked);
      assert(VMPageState::getVMAddress(v) != kTabbyInvalidAddr);
      ps.unlockXInvalidated();
      //frameMemManager->freeFrame(page);
   }
   frameMemManager->freeFrames(toEvict);
   //cerr << "evicted " << toEvict.size() << " toEvictSize " << toEvictSize << " toWriteSize " << toWriteSize << " shared " << shared << " locked " << locked << " scanned " << total << " physUsedCount " << physUsedCount << endl;

   physUsedCount -= toEvict.size();
}

//---------------------------------------------------------------------------

struct BTreeNode;

struct BTreeNodeHeader {
   static const unsigned underFullSize = (pageSize/2) + (pageSize/4);  // merge nodes more empty
   static const u64 noNeighbour = ~0ull;

   struct FenceKeySlot {
      u16 offset;
      u16 len;
   };
   VMPageState state;
   bool dirty;
   union {
      PID upperInnerNode; // inner
      PID nextLeafNode = noNeighbour; // leaf
   };

   bool hasRightNeighbour() { return nextLeafNode != noNeighbour; }

   FenceKeySlot lowerFence = {0, 0};  // exclusive
   FenceKeySlot upperFence = {0, 0};  // inclusive

   bool hasLowerFence() { return !!lowerFence.len; };

   u16 count = 0;
   bool isLeaf;
   u16 spaceUsed = 0;
   u16 dataOffset = static_cast<u16>(pageSize);
   u16 prefixLen = 0;

   static const unsigned hintCount = 16;
   u32 hint[hintCount];
   u32 padding;

   BTreeNodeHeader(bool isLeaf) : isLeaf(isLeaf) {}
   ~BTreeNodeHeader() {}
};

static unsigned min(unsigned a, unsigned b)
{
   return a < b ? a : b;
}

template <class T>
static T loadUnaligned(void* p)
{
   T x;
   memcpy(&x, p, sizeof(T));
   return x;
}

// Get order-preserving head of key (assuming little endian)
static u32 head(u8* key, unsigned keyLen)
{
   switch (keyLen) {
      case 0:
         return 0;
      case 1:
         return static_cast<u32>(key[0]) << 24;
      case 2:
         return static_cast<u32>(__builtin_bswap16(loadUnaligned<u16>(key))) << 16;
      case 3:
         return (static_cast<u32>(__builtin_bswap16(loadUnaligned<u16>(key))) << 16) | (static_cast<u32>(key[2]) << 8);
      default:
         return __builtin_bswap32(loadUnaligned<u32>(key));
   }
}

struct BTreeNode : public BTreeNodeHeader {
   struct Slot {
      u16 offset;
      u16 keyLen;
      u16 payloadLen;
      union {
         u32 head;
         u8 headBytes[4];
      };
   } __attribute__((packed));
   union {
      Slot slot[(pageSize - sizeof(BTreeNodeHeader)) / sizeof(Slot)];  // grows from front
      u8 heap[pageSize - sizeof(BTreeNodeHeader)];                // grows from back
   };

   static constexpr unsigned maxKVSize = ((pageSize - sizeof(BTreeNodeHeader) - (2 * sizeof(Slot)))) / 4;

   BTreeNode(bool isLeaf) : BTreeNodeHeader(isLeaf) { dirty = true; }

   u8* ptr() { return reinterpret_cast<u8*>(this); }
   bool isInner() { return !isLeaf; }
   span<u8> getLowerFence() { return { ptr() + lowerFence.offset, lowerFence.len}; }
   span<u8> getUpperFence() { return { ptr() + upperFence.offset, upperFence.len}; }
   u8* getPrefix() { return ptr() + lowerFence.offset; } // any key on page is ok

   unsigned freeSpace() { return dataOffset - (reinterpret_cast<u8*>(slot + count) - ptr()); }
   unsigned freeSpaceAfterCompaction() { return pageSize - (reinterpret_cast<u8*>(slot + count) - ptr()) - spaceUsed; }

   bool hasSpaceFor(unsigned keyLen, unsigned payloadLen)
   {
      return spaceNeeded(keyLen, payloadLen) <= freeSpaceAfterCompaction();
   }

   u8* getKey(unsigned slotId) { return ptr() + slot[slotId].offset; }
   span<u8> getPayload(unsigned slotId) { return {ptr() + slot[slotId].offset + slot[slotId].keyLen, slot[slotId].payloadLen}; }

   PID getChild(unsigned slotId) { return loadUnaligned<PID>(getPayload(slotId).data()); }

   // How much space would inserting a new key of len "keyLen" require?
   unsigned spaceNeeded(unsigned keyLen, unsigned payloadLen) {
      return sizeof(Slot) + (keyLen - prefixLen) + payloadLen;
   }

   void makeHint()
   {
      unsigned dist = count / (hintCount + 1);
      for (unsigned i = 0; i < hintCount; i++)
         hint[i] = slot[dist * (i + 1)].head;
   }

   void updateHint(unsigned slotId)
   {
      unsigned dist = count / (hintCount + 1);
      unsigned begin = 0;
      if ((count > hintCount * 2 + 1) && (((count - 1) / (hintCount + 1)) == dist) && ((slotId / dist) > 1))
         begin = (slotId / dist) - 1;
      for (unsigned i = begin; i < hintCount; i++)
         hint[i] = slot[dist * (i + 1)].head;
   }

   void searchHint(u32 keyHead, u16& lowerOut, u16& upperOut)
   {
      if (count > hintCount * 2) {
         u16 dist = upperOut / (hintCount + 1);
         u16 pos, pos2;
         for (pos = 0; pos < hintCount; pos++)
            if (hint[pos] >= keyHead)
               break;
         for (pos2 = pos; pos2 < hintCount; pos2++)
            if (hint[pos2] != keyHead)
               break;
         lowerOut = pos * dist;
         if (pos2 < hintCount)
            upperOut = (pos2 + 1) * dist;
      }
   }

   // lower bound search, foundExactOut indicates if there is an exact match, returns slotId
   u16 lowerBound(span<u8> skey, bool& foundExactOut)
   {
      foundExactOut = false;

      // check prefix
      int cmp = memcmp(skey.data(), getPrefix(), min(skey.size(), prefixLen));
      if (cmp < 0) // key is less than prefix
         return 0;
      if (cmp > 0) // key is greater than prefix
         return count;
      if (skey.size() < prefixLen) // key is equal but shorter than prefix
         return 0;
      u8* key = skey.data() + prefixLen;
      unsigned keyLen = skey.size() - prefixLen;

      // check hint
      u16 lower = 0;
      u16 upper = count;
      u32 keyHead = head(key, keyLen);
      searchHint(keyHead, lower, upper);

      // binary search on remaining range
      while (lower < upper) {
         u16 mid = ((upper - lower) / 2) + lower;
         if (keyHead < slot[mid].head) {
            upper = mid;
         } else if (keyHead > slot[mid].head) {
            lower = mid + 1;
         } else { // head is equal, check full key
            int cmp = memcmp(key, getKey(mid), min(keyLen, slot[mid].keyLen));
            if (cmp < 0) {
               upper = mid;
            } else if (cmp > 0) {
               lower = mid + 1;
            } else {
               if (keyLen < slot[mid].keyLen) { // key is shorter
                  upper = mid;
               } else if (keyLen > slot[mid].keyLen) { // key is longer
                  lower = mid + 1;
               } else {
                  foundExactOut = true;
                  return mid;
               }
            }
         }
      }
      return lower;
   }

   // lowerBound wrapper ignoring exact match argument (for convenience)
   u16 lowerBound(span<u8> key)
   {
      bool ignore;
      return lowerBound(key, ignore);
   }

   // insert key/value pair
   void insertInPage(span<u8> key, span<u8> payload)
   {
      unsigned needed = spaceNeeded(key.size(), payload.size());
      if (needed > freeSpace()) {
         assert(needed <= freeSpaceAfterCompaction());
         compactify();
      }
      unsigned slotId = lowerBound(key);
      memmove(slot + slotId + 1, slot + slotId, sizeof(Slot) * (count - slotId));
      storeKeyValue(slotId, key, payload);
      count++;
      updateHint(slotId);
   }

   bool removeSlot(unsigned slotId)
   {
      spaceUsed -= slot[slotId].keyLen;
      spaceUsed -= slot[slotId].payloadLen;
      memmove(slot + slotId, slot + slotId + 1, sizeof(Slot) * (count - slotId - 1));
      count--;
      makeHint();
      return true;
   }

   bool removeInPage(span<u8> key)
   {
      bool found;
      unsigned slotId = lowerBound(key, found);
      if (!found)
         return false;
      return removeSlot(slotId);
   }

   void copyNode(BTreeNodeHeader* dst, BTreeNodeHeader* src) {
      u64 ofs = offsetof(BTreeNodeHeader, upperInnerNode);
      memcpy(reinterpret_cast<u8*>(dst)+ofs, reinterpret_cast<u8*>(src)+ofs, sizeof(BTreeNode)-ofs);
   }

   void compactify()
   {
      unsigned should = freeSpaceAfterCompaction();
      static_cast<void>(should);
      BTreeNode tmp(isLeaf);
      tmp.setFences(getLowerFence(), getUpperFence());
      copyKeyValueRange(&tmp, 0, 0, count);
      tmp.upperInnerNode = upperInnerNode;
      copyNode(this, &tmp);
      makeHint();
      assert(freeSpace() == should);
   }

   // merge right node into this node
   bool mergeNodes(unsigned slotId, BTreeNode* parent, BTreeNode* right)
   {
      if (!isLeaf)
         // TODO: implement inner merge
         return true;

      assert(right->isLeaf);
      assert(parent->isInner());
      BTreeNode tmp(isLeaf);
      tmp.setFences(getLowerFence(), right->getUpperFence());
      unsigned leftGrow = (prefixLen - tmp.prefixLen) * count;
      unsigned rightGrow = (right->prefixLen - tmp.prefixLen) * right->count;
      unsigned spaceUpperBound =
         spaceUsed + right->spaceUsed + (reinterpret_cast<u8*>(slot + count + right->count) - ptr()) + leftGrow + rightGrow;
      if (spaceUpperBound > pageSize)
         return false;
      copyKeyValueRange(&tmp, 0, 0, count);
      right->copyKeyValueRange(&tmp, count, 0, right->count);
      PID pid = bm->toPID(this);
      memcpy(parent->getPayload(slotId+1).data(), &pid, sizeof(PID));
      parent->removeSlot(slotId);
      tmp.makeHint();
      tmp.nextLeafNode = right->nextLeafNode;

      copyNode(this, &tmp);
      return true;
   }

   // store key/value pair at slotId
   void storeKeyValue(u16 slotId, span<u8> skey, span<u8> payload)
   {
      // slot
      u8* key = skey.data() + prefixLen;
      unsigned keyLen = skey.size() - prefixLen;
      slot[slotId].head = head(key, keyLen);
      slot[slotId].keyLen = keyLen;
      slot[slotId].payloadLen = payload.size();
      // key
      unsigned space = keyLen + payload.size();
      dataOffset -= space;
      spaceUsed += space;
      slot[slotId].offset = dataOffset;
      assert(getKey(slotId) >= reinterpret_cast<u8*>(&slot[slotId]));
      memcpy(getKey(slotId), key, keyLen);
      memcpy(getPayload(slotId).data(), payload.data(), payload.size());
   }

   void copyKeyValueRange(BTreeNode* dst, u16 dstSlot, u16 srcSlot, unsigned srcCount)
   {
      if (prefixLen <= dst->prefixLen) {  // prefix grows
         unsigned diff = dst->prefixLen - prefixLen;
         for (unsigned i = 0; i < srcCount; i++) {
            unsigned newKeyLen = slot[srcSlot + i].keyLen - diff;
            unsigned space = newKeyLen + slot[srcSlot + i].payloadLen;
            dst->dataOffset -= space;
            dst->spaceUsed += space;
            dst->slot[dstSlot + i].offset = dst->dataOffset;
            u8* key = getKey(srcSlot + i) + diff;
            memcpy(dst->getKey(dstSlot + i), key, space);
            dst->slot[dstSlot + i].head = head(key, newKeyLen);
            dst->slot[dstSlot + i].keyLen = newKeyLen;
            dst->slot[dstSlot + i].payloadLen = slot[srcSlot + i].payloadLen;
         }
      } else {
         for (unsigned i = 0; i < srcCount; i++)
            copyKeyValue(srcSlot + i, dst, dstSlot + i);
      }
      dst->count += srcCount;
      assert((dst->ptr() + dst->dataOffset) >= reinterpret_cast<u8*>(dst->slot + dst->count));
   }

   void copyKeyValue(u16 srcSlot, BTreeNode* dst, u16 dstSlot)
   {
      unsigned fullLen = slot[srcSlot].keyLen + prefixLen;
      u8 key[fullLen];
      memcpy(key, getPrefix(), prefixLen);
      memcpy(key+prefixLen, getKey(srcSlot), slot[srcSlot].keyLen);
      dst->storeKeyValue(dstSlot, {key, fullLen}, getPayload(srcSlot));
   }

   void insertFence(FenceKeySlot& fk, span<u8> key)
   {
      assert(freeSpace() >= key.size());
      dataOffset -= key.size();
      spaceUsed += key.size();
      fk.offset = dataOffset;
      fk.len = key.size();
      memcpy(ptr() + dataOffset, key.data(), key.size());
   }

   void setFences(span<u8> lower, span<u8> upper)
   {
      insertFence(lowerFence, lower);
      insertFence(upperFence, upper);
      for (prefixLen = 0; (prefixLen < min(lower.size(), upper.size())) && (lower[prefixLen] == upper[prefixLen]); prefixLen++)
         ;
   }

   void splitNode(BTreeNode* parent, unsigned sepSlot, span<u8> sep)
   {
      assert(sepSlot > 0);
      assert(sepSlot < (pageSize / sizeof(PID)));

      BTreeNode tmp(isLeaf);
      BTreeNode* nodeLeft = &tmp;

      AllocGuard<BTreeNode> newNode(isLeaf);
      BTreeNode* nodeRight = newNode.ptr;

      nodeLeft->setFences(getLowerFence(), sep);
      nodeRight->setFences(sep, getUpperFence());

      PID leftPID = bm->toPID(this);
      u16 oldParentSlot = parent->lowerBound(sep);
      if (oldParentSlot == parent->count) {
         assert(parent->upperInnerNode == leftPID);
         parent->upperInnerNode = newNode.pid;
      } else {
         assert(parent->getChild(oldParentSlot) == leftPID);
         memcpy(parent->getPayload(oldParentSlot).data(), &newNode.pid, sizeof(PID));
      }
      parent->insertInPage(sep, {reinterpret_cast<u8*>(&leftPID), sizeof(PID)});

      if (isLeaf) {
         copyKeyValueRange(nodeLeft, 0, 0, sepSlot + 1);
         copyKeyValueRange(nodeRight, 0, nodeLeft->count, count - nodeLeft->count);
         nodeLeft->nextLeafNode = newNode.pid;
         nodeRight->nextLeafNode = this->nextLeafNode;
      } else {
         // in inner node split, separator moves to parent (count == 1 + nodeLeft->count + nodeRight->count)
         copyKeyValueRange(nodeLeft, 0, 0, sepSlot);
         copyKeyValueRange(nodeRight, 0, nodeLeft->count + 1, count - nodeLeft->count - 1);
         nodeLeft->upperInnerNode = getChild(nodeLeft->count);
         nodeRight->upperInnerNode = upperInnerNode;
      }
      nodeLeft->makeHint();
      nodeRight->makeHint();
      copyNode(this, nodeLeft);
   }

   struct SeparatorInfo {
      unsigned len;      // len of new separator
      unsigned slot;     // slot at which we split
      bool isTruncated;  // if true, we truncate the separator taking len bytes from slot+1
   };

   unsigned commonPrefix(unsigned slotA, unsigned slotB)
   {
      assert(slotA < count);
      unsigned limit = min(slot[slotA].keyLen, slot[slotB].keyLen);
      u8 *a = getKey(slotA), *b = getKey(slotB);
      unsigned i;
      for (i = 0; i < limit; i++)
         if (a[i] != b[i])
            break;
      return i;
   }

   SeparatorInfo findSeparator(bool splitOrdered)
   {
      assert(count > 1);
      if (isInner()) {
         // inner nodes are split in the middle
         unsigned slotId = count / 2;
         return SeparatorInfo{static_cast<unsigned>(prefixLen + slot[slotId].keyLen), slotId, false};
      }

      // find good separator slot
      unsigned bestPrefixLen, bestSlot;

      if (splitOrdered) {
         bestSlot = count - 2;
      } else if (count > 16) {
         unsigned lower = (count / 2) - (count / 16);
         unsigned upper = (count / 2);

         bestPrefixLen = commonPrefix(lower, 0);
         bestSlot = lower;

         if (bestPrefixLen != commonPrefix(upper - 1, 0))
            for (bestSlot = lower + 1; (bestSlot < upper) && (commonPrefix(bestSlot, 0) == bestPrefixLen); bestSlot++)
               ;
      } else {
         bestSlot = (count-1) / 2;
      }


      // try to truncate separator
      unsigned common = commonPrefix(bestSlot, bestSlot + 1);
      if ((bestSlot + 1 < count) && (slot[bestSlot].keyLen > common) && (slot[bestSlot + 1].keyLen > (common + 1)))
         return SeparatorInfo{prefixLen + common + 1, bestSlot, true};

      return SeparatorInfo{static_cast<unsigned>(prefixLen + slot[bestSlot].keyLen), bestSlot, false};
   }

   void getSep(u8* sepKeyOut, SeparatorInfo info)
   {
      memcpy(sepKeyOut, getPrefix(), prefixLen);
      memcpy(sepKeyOut + prefixLen, getKey(info.slot + info.isTruncated), info.len - prefixLen);
   }

   PID lookupInner(span<u8> key)
   {
      unsigned pos = lowerBound(key);
      if (pos == count)
         return upperInnerNode;
      return getChild(pos);
   }
};

static_assert(sizeof(BTreeNode) == pageSize, "btree node size problem");

struct BTree {
   private:

   void trySplit(GuardX<BTreeNode>&& node, GuardX<BTreeNode>&& parent, span<u8> key, unsigned payloadLen);
   void ensureSpace(BTreeNode* toSplit, span<u8> key, unsigned payloadLen);

   public:
   unsigned slotId;
   atomic<bool> splitOrdered;

   BTree();
   ~BTree();

   GuardO<BTreeNode> findLeafO(span<u8> key) {
      GuardO<MetaDataPage> meta(metadataPageId);
      GuardO<BTreeNode> node(meta->getRoot(slotId), meta);
      meta.release();

      while (node->isInner())
         node = GuardO<BTreeNode>(node->lookupInner(key), node);
      return node;
   }

   // point lookup, returns payload len on success, or -1 on failure
   int lookup(span<u8> key, u8* payloadOut, unsigned payloadOutSize) {
      for (u64 repeatCounter=0; ; repeatCounter++) {
         try {
            GuardO<BTreeNode> node = findLeafO(key);
            bool found;
            unsigned pos = node->lowerBound(key, found);
            if (!found)
               return -1;

            // key found, copy payload
            memcpy(payloadOut, node->getPayload(pos).data(), min(node->slot[pos].payloadLen, payloadOutSize));
            return node->slot[pos].payloadLen;
         } catch(const OLCRestartException&) {}
      }
   }

   template<class Fn>
   bool lookup(span<u8> key, Fn fn) {
      for (u64 repeatCounter=0; ; repeatCounter++) {
         try {
            GuardO<BTreeNode> node = findLeafO(key);
            bool found;
            unsigned pos = node->lowerBound(key, found);
            if (!found)
               return false;

            // key found
            fn(node->getPayload(pos));
            return true;
         } catch(const OLCRestartException&) {}
      }
   }

   void insert(span<u8> key, span<u8> payload);
   bool remove(span<u8> key);

   template<class Fn>
   bool updateInPlace(span<u8> key, Fn fn) {
      for (u64 repeatCounter=0; ; repeatCounter++) {
         try {
            GuardO<BTreeNode> node = findLeafO(key);
            bool found;
            unsigned pos = node->lowerBound(key, found);
            if (!found)
               return false;

            {
               GuardX<BTreeNode> nodeLocked(move(node));
               fn(nodeLocked->getPayload(pos));
               return true;
            }
         } catch(const OLCRestartException&) {}
      }
   }

   GuardS<BTreeNode> findLeafS(span<u8> key) {
      for (u64 repeatCounter=0; ; repeatCounter++) {
         try {
            GuardO<MetaDataPage> meta(metadataPageId);
            GuardO<BTreeNode> node(meta->getRoot(slotId), meta);
            meta.release();

            while (node->isInner())
               node = GuardO<BTreeNode>(node->lookupInner(key), node);

            return GuardS<BTreeNode>(move(node));
         } catch(const OLCRestartException&) {}
      }
   }

   template<class Fn>
   void scanAsc(span<u8> key, Fn fn) {
      GuardS<BTreeNode> node = findLeafS(key);
      bool found;
      unsigned pos = node->lowerBound(key, found);
      for (u64 repeatCounter=0; ; repeatCounter++) {
         if (pos<node->count) {
            if (!fn(*node.ptr, pos))
               return;
            pos++;
         } else {
            if (!node->hasRightNeighbour())
               return;
            pos = 0;
            node = GuardS<BTreeNode>(node->nextLeafNode);
         }
      }
   }

   template<class Fn>
   void scanDesc(span<u8> key, Fn fn) {
      GuardS<BTreeNode> node = findLeafS(key);
      bool exactMatch;
      int pos = node->lowerBound(key, exactMatch);
      if (pos == node->count) {
         pos--;
         exactMatch = true; // XXX:
      }
      for (u64 repeatCounter=0; ; repeatCounter++) {
         while (pos>=0) {
            if (!fn(*node.ptr, pos, exactMatch))
               return;
            pos--;
         }
         if (!node->hasLowerFence())
            return;
         node = findLeafS(node->getLowerFence());
         pos = node->count-1;
      }
   }
};

static unsigned btreeslotcounter = 0;

BTree::BTree() : splitOrdered(false) {
   GuardX<MetaDataPage> page(metadataPageId);
   AllocGuard<BTreeNode> rootNode(true);
   slotId = btreeslotcounter++;
   page->roots[slotId] = rootNode.pid;
   cerr << "BTree root allocated at slot " << slotId  << " pid " << rootNode.pid << " " << rootNode.ptr << " count " << rootNode->count << " isLeaf " << rootNode->isLeaf << endl;
}

BTree::~BTree() {}

void BTree::trySplit(GuardX<BTreeNode>&& node, GuardX<BTreeNode>&& parent, span<u8> key, unsigned payloadLen)
{

   // create new root if necessary
   if (parent.pid == metadataPageId) {
      MetaDataPage* metaData = reinterpret_cast<MetaDataPage*>(parent.ptr);
      AllocGuard<BTreeNode> newRoot(false);
      newRoot->upperInnerNode = node.pid;
      metaData->roots[slotId] = newRoot.pid;
      parent = move(newRoot);
   }

   // split
   BTreeNode::SeparatorInfo sepInfo = node->findSeparator(splitOrdered.load());
   u8 sepKey[sepInfo.len];
   node->getSep(sepKey, sepInfo);

   if (parent->hasSpaceFor(sepInfo.len, sizeof(PID))) {  // is there enough space in the parent for the separator?
      node->splitNode(parent.ptr, sepInfo.slot, {sepKey, sepInfo.len});
      return;
   }

   // must split parent to make space for separator, restart from root to do this
   node.release();
   parent.release();
   ensureSpace(parent.ptr, {sepKey, sepInfo.len}, sizeof(PID));
}

void BTree::ensureSpace(BTreeNode* toSplit, span<u8> key, unsigned payloadLen)
{
   for (u64 repeatCounter=0; ; repeatCounter++) {
      try {
         GuardO<BTreeNode> parent(metadataPageId);
         GuardO<BTreeNode> node(reinterpret_cast<MetaDataPage*>(parent.ptr)->getRoot(slotId), parent);

         while (node->isInner() && (node.ptr != toSplit)) {
            parent = move(node);
            node = GuardO<BTreeNode>(parent->lookupInner(key), parent);
         }
         if (node.ptr == toSplit) {
            if (node->hasSpaceFor(key.size(), payloadLen))
               return; // someone else did split concurrently
            GuardX<BTreeNode> parentLocked(move(parent));
            GuardX<BTreeNode> nodeLocked(move(node));
            trySplit(move(nodeLocked), move(parentLocked), key, payloadLen);
         }
         return;
      } catch(const OLCRestartException&) {}
   }
}

void BTree::insert(span<u8> key, span<u8> payload)
{
   assert((key.size()+payload.size()) <= BTreeNode::maxKVSize);

   for (u64 repeatCounter=0; ; repeatCounter++) {
      try {
         GuardO<BTreeNode> parent(metadataPageId);
         GuardO<BTreeNode> node(reinterpret_cast<MetaDataPage*>(parent.ptr)->getRoot(slotId), parent);

         //cerr << "BTree::insert descending started at " << node.ptr << " " << node->isInner() << node->count << endl;
         while (node->isInner()) {
            parent = move(node);
            node = GuardO<BTreeNode>(parent->lookupInner(key), parent);
         }
         //cerr << "BTree::insert descending ended" << endl;
         if (node->hasSpaceFor(key.size(), payload.size())) {
            // only lock leaf
            //cerr << "BTree::insert locking leaf" << endl;
            GuardX<BTreeNode> nodeLocked(move(node));
            //cerr << "BTree::insert locked leaf" << endl;
            parent.release();
            nodeLocked->insertInPage(key, payload);
            return; // success
         }

         // lock parent and leaf
         //cerr << "BTree::insert locking leaf and parent" << endl;
         GuardX<BTreeNode> parentLocked(move(parent));
         GuardX<BTreeNode> nodeLocked(move(node));
         //cerr << "BTree::insert locked leaf and parent" << endl;
         trySplit(move(nodeLocked), move(parentLocked), key, payload.size());
         // insert hasn't happened, restart from root
      } catch(const OLCRestartException&) {}
   }
}

bool BTree::remove(span<u8> key)
{
   for (u64 repeatCounter=0; ; repeatCounter++) {
      try {
         GuardO<BTreeNode> parent(metadataPageId);
         GuardO<BTreeNode> node(reinterpret_cast<MetaDataPage*>(parent.ptr)->getRoot(slotId), parent);

         u16 pos;
         while (node->isInner()) {
            pos = node->lowerBound(key);
            PID nextPage = (pos == node->count) ? node->upperInnerNode : node->getChild(pos);
            parent = move(node);
            node = GuardO<BTreeNode>(nextPage, parent);
         }

         bool found;
         unsigned slotId = node->lowerBound(key, found);
         if (!found)
            return false;

         unsigned sizeEntry = node->slot[slotId].keyLen + node->slot[slotId].payloadLen;
         if ((node->freeSpaceAfterCompaction()+sizeEntry >= BTreeNodeHeader::underFullSize) && (parent.pid != metadataPageId) && (parent->count >= 2) && ((pos + 1) < parent->count)) {
            // underfull
            GuardX<BTreeNode> parentLocked(move(parent));
            GuardX<BTreeNode> nodeLocked(move(node));
            GuardX<BTreeNode> rightLocked(parentLocked->getChild(pos + 1));
            nodeLocked->removeSlot(slotId);
            if (rightLocked->freeSpaceAfterCompaction() >= BTreeNodeHeader::underFullSize) {
               if (nodeLocked->mergeNodes(pos, parentLocked.ptr, rightLocked.ptr)) {
               }
            }
         } else {
            GuardX<BTreeNode> nodeLocked(move(node));
            parent.release();
            nodeLocked->removeSlot(slotId);
         }
         return true;
      } catch(const OLCRestartException&) {}
   }
}

typedef u64 KeyType;

template <class Record>
struct vmcacheAdapter
{
   BTree tree;

   public:
   void scan(const typename Record::Key& key, const std::function<bool(const typename Record::Key&, const Record&)>& found_record_cb, std::function<void()> reset_if_scan_failed_cb) {
      u8 k[Record::maxFoldLength()];
      u16 l = Record::foldKey(k, key);
      u8 kk[Record::maxFoldLength()];
      tree.scanAsc({k, l}, [&](BTreeNode& node, unsigned slot) {
         memcpy(kk, node.getPrefix(), node.prefixLen);
         memcpy(kk+node.prefixLen, node.getKey(slot), node.slot[slot].keyLen);
         typename Record::Key typedKey;
         Record::unfoldKey(kk, typedKey);
         return found_record_cb(typedKey, *reinterpret_cast<const Record*>(node.getPayload(slot).data()));
      });
   }
   // -------------------------------------------------------------------------------------
   void scanDesc(const typename Record::Key& key, const std::function<bool(const typename Record::Key&, const Record&)>& found_record_cb, std::function<void()> reset_if_scan_failed_cb) {
      u8 k[Record::maxFoldLength()];
      u16 l = Record::foldKey(k, key);
      u8 kk[Record::maxFoldLength()];
      bool first = true;
      tree.scanDesc({k, l}, [&](BTreeNode& node, unsigned slot, bool exactMatch) {
         if (first) { // XXX: hack
            first = false;
            if (!exactMatch)
               return true;
         }
         memcpy(kk, node.getPrefix(), node.prefixLen);
         memcpy(kk+node.prefixLen, node.getKey(slot), node.slot[slot].keyLen);
         typename Record::Key typedKey;
         Record::unfoldKey(kk, typedKey);
         return found_record_cb(typedKey, *reinterpret_cast<const Record*>(node.getPayload(slot).data()));
      });
   }
   // -------------------------------------------------------------------------------------
   void insert(const typename Record::Key& key, const Record& record) {
      u8 k[Record::maxFoldLength()];
      u16 l = Record::foldKey(k, key);
      tree.insert({k, l}, {(u8*)(&record), sizeof(Record)});
   }
   // -------------------------------------------------------------------------------------
   template<class Fn>
   void lookup1(const typename Record::Key& key, Fn fn) {
      u8 k[Record::maxFoldLength()];
      u16 l = Record::foldKey(k, key);
      bool succ = tree.lookup({k, l}, [&](span<u8> payload) {
         fn(*reinterpret_cast<const Record*>(payload.data()));
      });
      assert(succ);
   }
   // -------------------------------------------------------------------------------------
   template<class Fn>
   void update1(const typename Record::Key& key, Fn fn) {
      u8 k[Record::maxFoldLength()];
      u16 l = Record::foldKey(k, key);
      tree.updateInPlace({k, l}, [&](span<u8> payload) {
         fn(*reinterpret_cast<Record*>(payload.data()));
      });
   }
   // -------------------------------------------------------------------------------------
   // Returns false if the record was not found
   bool erase(const typename Record::Key& key) {
      u8 k[Record::maxFoldLength()];
      u16 l = Record::foldKey(k, key);
      return tree.remove({k, l});
   }
   // -------------------------------------------------------------------------------------
   template <class Field>
   Field lookupField(const typename Record::Key& key, Field Record::*f) {
      Field value;
      lookup1(key, [&](const Record& r) { value = r.*f; });
      return value;
   }

   u64 count() {
      u64 cnt = 0;
      tree.scanAsc({(u8*)nullptr, 0}, [&](BTreeNode& node, unsigned slot) { cnt++; return true; } );
      return cnt;
   }

   u64 countw(Integer w_id) {
      u8 k[sizeof(Integer)];
      fold(k, w_id);
      u64 cnt = 0;
      u8 kk[Record::maxFoldLength()];
      tree.scanAsc({k, sizeof(Integer)}, [&](BTreeNode& node, unsigned slot) {
         memcpy(kk, node.getPrefix(), node.prefixLen);
         memcpy(kk+node.prefixLen, node.getKey(slot), node.slot[slot].keyLen);
         if (memcmp(k, kk, sizeof(Integer))!=0)
            return false;
         cnt++;
         return true;
      });
      return cnt;
   }
};


static int pgflt_count = 0;
static void
tabby_pgflt_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	int ret;
	ptent_t *pte;
	bool was_user = (tf->cs & 0x3);
   bool nonpresent = (fec & FEC_P) == 0;
   bool protection = fec & FEC_P;
   bool write = fec & FEC_W;
   bool created = false;
   dune_printf("tabby_pgflt_handler on %lx\n", addr);
   if (addr >= kTabbyAddressSpaceStart) {
      bm->handlePageFault(addr, true, false);
      return;
   }
   ret = dune_vm_lookup2(pgroot, (void *) addr, CREATE_NORMAL, &created, &pte);
   assert(!ret);
   *pte |= PTE_U | PTE_P | PTE_W;// | PTE_ADDR(dune_va_to_pa((void *) addr)
   if (created) {
      dune_printf("physical page created for vm %lx\n", addr);
   }
   
   ++pgflt_count;
}

#define USER_FUNC_1 668
typedef void (*user_func_t)(void* arg);
static void syscall_handler(struct dune_tf *tf)
{
   assert(false);
  int syscall_num = (int) tf->rax;

   ++pgflt_count;
  if (syscall_num == 666) {
    dune_ret_from_user(0);
  } else if (syscall_num == USER_FUNC_1) {
    ((user_func_t)(tf->rdi))((void*)tf->rsi);
  } else if (syscall_num == 555) {
    return;
  } else {
    dune_passthrough_syscall(tf);
  }
}

int num_cores = 0;

int stick_this_thread_to_core(int core_id) {
   core_id = core_id % num_cores;
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(core_id, &cpuset);

   pthread_t current_thread = pthread_self();    
   return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

class ThreadPool {
public:
   static constexpr u64 MAX_WORKER_THREADS = 256;
   // -------------------------------------------------------------------------------------
   std::atomic<u64> running_threads = 0;
   std::atomic<bool> keep_running = true;
   // -------------------------------------------------------------------------------------
   struct WorkerThread {
      std::mutex mutex;
      std::condition_variable cv;
      std::function<void()> job;
      bool wt_ready = true;   // Idle
      bool job_set = false;   // Has job
      bool job_done = false;  // Job done
   };
   std::vector<std::thread> worker_threads;
   WorkerThread worker_threads_meta[MAX_WORKER_THREADS];
   u32 workers_count;
   ThreadPool(int workers) {
      workers_count = workers;
      assert(workers_count < MAX_WORKER_THREADS);
      // -------------------------------------------------------------------------------------
      worker_threads.reserve(workers_count);
      for (u64 t_i = 0; t_i < workers_count; t_i++) {
         worker_threads.emplace_back([&, t_i]() {
            stick_this_thread_to_core(t_i);
            #ifdef ENABLE_DUNE
            if (dune_enter()) {
               printf("failed to enter dune mode\n");
               exit(1);
            }
            #endif
            dune_set_cpu_id(t_i);
            // -------------------------------------------------------------------------------------
            running_threads++;
            while (running_threads != (workers_count))
               ;
            auto& meta = worker_threads_meta[t_i];

            while (keep_running) {
               std::unique_lock guard(meta.mutex);
               meta.cv.wait(guard, [&]() { return keep_running == false || meta.job_set; });
               if (!keep_running) {
                  break;
               }
               meta.wt_ready = false;
               meta.job();
               meta.wt_ready = true;
               meta.job_done = true;
               meta.job_set = false;
               meta.cv.notify_one();
            }
            running_threads--;
         });
      }
      for (auto& t : worker_threads) {
         t.detach();
      }
      // -------------------------------------------------------------------------------------
      // Wait until all worker threads are initialized
      while (running_threads < workers_count) {
      }
   }

   // -------------------------------------------------------------------------------------
   void scheduleJobSync(u64 t_i, std::function<void()> job)
   {
      setJob(t_i, job);
      joinOne(t_i, [&](WorkerThread& meta) { return meta.job_done; });
   }
   // -------------------------------------------------------------------------------------
   void scheduleJobAsync(u64 t_i, std::function<void()> job)
   {
      setJob(t_i, job);
   }
   // -------------------------------------------------------------------------------------
   void scheduleJobs(u64 workers, std::function<void()> job)
   {
      for (u32 t_i = 0; t_i < workers; t_i++) {
         setJob(t_i, job);
      }
   }
   void scheduleJobs(u64 workers, std::function<void(u64 t_i)> job)
   {
      for (u32 t_i = 0; t_i < workers; t_i++) {
         setJob(t_i, [=]() { return job(t_i); });
      }
   }

   // -------------------------------------------------------------------------------------
   void joinAll()
   {
      for (u32 t_i = 0; t_i < workers_count; t_i++) {
         joinOne(t_i, [&](WorkerThread& meta) { return meta.wt_ready && !meta.job_set; });
      }
   }
   // -------------------------------------------------------------------------------------
   void setJob(u64 t_i, std::function<void()> job)
   {
      assert(t_i < workers_count);
      auto& meta = worker_threads_meta[t_i];
      std::unique_lock guard(meta.mutex);
      meta.cv.wait(guard, [&]() { return !meta.job_set && meta.wt_ready; });
      meta.job_set = true;
      meta.job_done = false;
      meta.job = job;
      guard.unlock();
      meta.cv.notify_one();
   }
   // -------------------------------------------------------------------------------------
   void joinOne(u64 t_i, std::function<bool(WorkerThread&)> condition)
   {
      assert(t_i < workers_count);
      auto& meta = worker_threads_meta[t_i];
      std::unique_lock guard(meta.mutex);
      meta.cv.wait(guard, [&]() { return condition(meta); });
   }

   ~ThreadPool()
   {
      keep_running = false;
      for (u64 t_i = 0; t_i < workers_count; t_i++) {
         worker_threads_meta[t_i].cv.notify_one();
      }
      while (running_threads) {
      }
   }

   template<class Fn>
   void parallel_for(uint64_t begin, uint64_t end, uint64_t nthreads, Fn fn) {
      uint64_t n = end-begin;
      if (n<nthreads)
         nthreads = n;
      uint64_t perThread = n/nthreads;
      for (unsigned i=0; i<nthreads; i++) {
         scheduleJobAsync(i, [&,i]() {
            uint64_t b = (perThread*i) + begin;
            uint64_t e = (i==(nthreads-1)) ? end : ((b+perThread) + begin);
            fn(i, b, e);
         });
      }
   }
};

template<class Fn>
void parallel_for(uint64_t begin, uint64_t end, uint64_t nthreads, Fn fn) {
   std::vector<std::thread> threads;
   uint64_t n = end-begin;
   if (n<nthreads)
      nthreads = n;
   uint64_t perThread = n/nthreads;
   for (unsigned i=0; i<nthreads; i++) {
      threads.emplace_back([&,i]() {
         uint64_t b = (perThread*i) + begin;
         uint64_t e = (i==(nthreads-1)) ? end : ((b+perThread) + begin);
         fn(i, b, e);
      });
   }
   for (auto& t : threads)
      t.join();
}

int main(int argc, char** argv) {
   exception_hack::init_phdr_cache();
   num_cores = sysconf(_SC_NPROCESSORS_ONLN);
   #ifdef ENABLE_DUNE
   int ret = dune_init_and_enter();
   if (ret) {
      cerr << "failed to initialize dune" << endl;
      exit(1);
   } else {
      cerr << "entered dune-mode, num_cores " << num_cores << endl;
   }
   dune_register_syscall_handler(syscall_handler);
   dune_register_pgflt_handler(tabby_pgflt_handler);
   #endif
   dune_set_cpu_id(0);
   bm = new BufferManager();
   bm->init();

   cerr << "THREADS" << num_cores << endl;
   unsigned nthreads = envOr("THREADS", 1);
   ThreadPool pool(nthreads);

   
   u64 n = envOr("DATASIZE", 10);
   u64 runForSec = envOr("RUNFOR", 30);
   bool isRndread = envOr("RNDREAD", 0);

   u64 statDiff = 1e8;
   atomic<u64> txProgress(0);
   atomic<bool> keepRunning(true);
   auto systemName = "tabby";
   u64 txnUpperBound = envOr("txnUpperBound", 1000000000);

   auto statFn = [&]() {
      cout << "ts,tx,rmb,wmb,system,threads,datasize,workload,batch,phy_cnt" << endl;
      u64 cnt = 0;
      for (uint64_t i=0; i<runForSec; i++) {
         sleep(1);
         float rmb = (bm->readCount.exchange(0)*pageSize)/(1024.0*1024);
         float wmb = (bm->writeCount.exchange(0)*pageSize)/(1024.0*1024);
         u64 prog = txProgress.exchange(0);
         cout << cnt++ << "," << prog << "," << rmb << "," << wmb << "," << systemName << "," << nthreads << "," << n << "," << (isRndread?"rndread":"tpcc") << "," << bm->batch << "," << bm->physUsedCount.load() << endl;
      }
      keepRunning = false;
      cerr << "Benchmark finished " << endl;
      cerr << "pgflt count " << pgflt_count << " hardware_page fault on tabby area " << hardware_page_faults << endl;
      cerr << "evict_written_pages " << evict_written_pages << " evict_rounds " << evict_rounds <<  " write-batch size " << evict_written_pages / evict_rounds <<  endl;
      cerr << "evict_clean_pages " << evict_clean_pages << " evict_rounds " << evict_rounds <<  " clean-batch size " << evict_clean_pages / evict_rounds <<  endl;
      //sleep(1000);
      cerr << "space: " << (bm->allocCount.load()*pageSize)/(float)bm->gb << " GB " << endl;
      exit(0);
   };
   
   cerr << "BufferManager inited isRndread " << isRndread << endl;

   if (isRndread) {
      u64 readRatio = envOr("READRATIO", 100);
      bool isZipfian = envOr("zipfian", 0);
      double theta = getenv("theta") ? atof(getenv("theta")) : 0.0;
      cerr << "random lookup" << endl;
      BTree bt;
      bt.splitOrdered = true;
      {
         // insert
         pool.parallel_for(0, n, nthreads, [&](uint64_t worker, uint64_t begin, uint64_t end) {
            
            //cerr << "insert worker " << worker  << " entered dune-mode" << endl;
            workerThreadId = worker;
            array<u8, 120> payload;
            for (u64 i=begin; i<end; i++) {
               union { u64 v1; u8 k1[sizeof(u64)]; };
               v1 = __builtin_bswap64(i);
               memcpy(payload.data(), k1, sizeof(u64));
               bt.insert({k1, sizeof(KeyType)}, payload);
               if (i % 1000000 == 0) {
                  cerr << "loaded " << i << endl;
               }
            }
            //cerr << "insert worker " << worker  << " finished" << endl;
         });
         pool.joinAll();
      }
   
      cerr << "space: " << (bm->allocCount.load()*pageSize)/(float)bm->gb << " GB " << endl;
      cerr << "theta " << theta << endl;

      bm->readCount = 0;
      bm->writeCount = 0;
      
      ScrambledZipfGenerator generator = ScrambledZipfGenerator(0, n, theta);
      thread statThread(statFn);

      pool.parallel_for(0, nthreads, nthreads, [&](uint64_t worker, uint64_t begin, uint64_t end) {
         workerThreadId = worker;
         u64 cnt = 0;
         u64 start = rdtsc();

         while (keepRunning.load()) {
            union { u64 v1; u8 k1[sizeof(u64)]; };
            if (!isZipfian) {
               v1 = __builtin_bswap64(RandomGenerator::getRand<u64>(0, n));
            }
            else {
               v1 = __builtin_bswap64(generator.rand());
            }

            array<u8, 120> payload;

            if (readRatio == 100 || RandomGenerator::getRand<u64>(0, 100) < readRatio) {
               bool succ = bt.lookup({k1, sizeof(u64)}, [&](span<u8> p) {
                  memcpy(payload.data(), p.data(), p.size());
               });
               assert(succ);
               //assert(memcmp(k1, payload.data(), sizeof(u64))==0);
            } else {
               RandomGenerator::getRandString(reinterpret_cast<u8*>(payload.data()), sizeof(u8) * 120);
               //bt.insert({k1, sizeof(KeyType)}, payload);
               bt.updateInPlace({k1, sizeof(u64)}, [&](span<u8> payload_buf) {
                  memcpy(payload_buf.data(), payload.data(), payload.size());
                  //fn(*reinterpret_cast<Record*>(payload.data()));
               });
            }

            cnt++;
            u64 stop = rdtsc();
            if ((stop-start) > statDiff) {
               txProgress += cnt;
               start = stop;
               cnt = 0;
            }
         }
         txProgress += cnt;
      });
      pool.joinAll();
      statThread.join();
      cerr << "pgflt count " << pgflt_count << endl;
      //dune_procmap_dump();
      return 0;
   }

   cerr << "TPC-C" << endl;
   // TPC-C
   Integer warehouseCount = n;
   vmcacheAdapter<warehouse_t> warehouse;
   vmcacheAdapter<district_t> district;
   vmcacheAdapter<customer_t> customer;
   vmcacheAdapter<customer_wdl_t> customerwdl;
   vmcacheAdapter<history_t> history;
   vmcacheAdapter<neworder_t> neworder;
   vmcacheAdapter<order_t> order;
   vmcacheAdapter<order_wdc_t> order_wdc;
   vmcacheAdapter<orderline_t> orderline;
   vmcacheAdapter<item_t> item;
   vmcacheAdapter<stock_t> stock;

   TPCCWorkload<vmcacheAdapter> tpcc(warehouse, district, customer, customerwdl, history, neworder, order, order_wdc, orderline, item, stock, true, warehouseCount, true);

   {
      tpcc.loadItem();
      tpcc.loadWarehouse();

      pool.parallel_for(1, warehouseCount+1, nthreads, [&](uint64_t worker, uint64_t begin, uint64_t end) {
         workerThreadId = worker;
         for (Integer w_id=begin; w_id<end; w_id++) {
            tpcc.loadStock(w_id);
            tpcc.loadDistrinct(w_id);
            for (Integer d_id = 1; d_id <= 10; d_id++) {
               tpcc.loadCustomer(w_id, d_id);
               tpcc.loadOrders(w_id, d_id);
            }
         }
      });
      pool.joinAll();
   }
   evict_rounds = 0;
   evict_written_pages = 0;
   evict_clean_pages = 0;
   cerr << "space: " << (bm->allocCount.load()*pageSize)/(float)bm->gb << " GB " << endl;

   bm->readCount = 0;
   bm->writeCount = 0;
   thread statThread(statFn);

   pool.parallel_for(0, nthreads, nthreads, [&](uint64_t worker, uint64_t begin, uint64_t end) {
      workerThreadId = worker;
      u64 cnt = 0;
      u64 timeCnt = 0;
      u64 start = rdtsc();
      while (keepRunning.load()) {
         int w_id = tpcc.urand(1, warehouseCount); // wh crossing
         tpcc.tx(w_id);
         cnt++;
         timeCnt++;
         u64 stop = rdtsc();
         if ((stop-start) > statDiff) {
            txProgress += cnt;
            start = stop;
            cnt = 0;
         }
      }
      txProgress += cnt;
   });

   statThread.join();
   cerr << "space: " << (bm->allocCount.load()*pageSize)/(float)bm->gb << " GB " << endl;
   cerr << "pgflt count " << pgflt_count << endl;
   return 0;
}
