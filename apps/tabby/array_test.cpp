#include <iostream>
#include <cstdlib>

#include <cstdint>
#include <random>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <cstdlib>
#include <malloc.h>
#include <sys/mman.h>
#ifdef __cplusplus
extern "C"{
#endif 
#include "dune.h"
#ifdef __cplusplus
}
#endif
using namespace std;
/// Zipf-like random distribution.
///
/// "Rejection-inversion to generate variates from monotone discrete
/// distributions", Wolfgang Hörmann and Gerhard Derflinger
/// ACM TOMACS 6.3 (1996): 169-184
///
/// Implementation from: https://stackoverflow.com/a/44154095/4777124
template <class IntType = int64_t, class RealType = double>
class zipf_distribution {
 public:
  typedef RealType input_type;
  typedef IntType result_type;

  static_assert(std::numeric_limits<IntType>::is_integer, "");
  static_assert(!std::numeric_limits<RealType>::is_integer, "");

  explicit zipf_distribution(
      const IntType n = std::numeric_limits<IntType>::max(),
      const RealType q = 1.0)
      : n(n), q(q), H_x1(H(1.5) - 1.0), H_n(H(n + 0.5)), dist(H_x1, H_n) {}

  IntType operator()(std::mt19937& rng) {
    while (true) {
      const RealType u = dist(rng);
      const RealType x = H_inv(u);
      const IntType k = clamp<IntType>(std::round(x), 1, n);
      if (u >= H(k + 0.5) - h(k)) {
        return k;
      }
    }
  }

 private:
  /// Clamp x to [min, max].
  template <typename T>
  static constexpr T clamp(const T x, const T min, const T max) {
    return std::max(min, std::min(max, x));
  }

  /// exp(x) - 1 / x
  static double expxm1bx(const double x) {
    return (std::abs(x) > epsilon)
               ? std::expm1(x) / x
               : (1.0 + x / 2.0 * (1.0 + x / 3.0 * (1.0 + x / 4.0)));
  }

  /// H(x) = log(x) if q == 1, (x^(1-q) - 1)/(1 - q) otherwise.
  /// H(x) is an integral of h(x).
  ///
  /// Note the numerator is one less than in the paper order to work with all
  /// positive q.
  const RealType H(const RealType x) {
    const RealType log_x = std::log(x);
    return expxm1bx((1.0 - q) * log_x) * log_x;
  }

  /// log(1 + x) / x
  static RealType log1pxbx(const RealType x) {
    return (std::abs(x) > epsilon)
               ? std::log1p(x) / x
               : 1.0 - x * ((1 / 2.0) - x * ((1 / 3.0) - x * (1 / 4.0)));
  }

  /// The inverse function of H(x)
  const RealType H_inv(const RealType x) {
    const RealType t = std::max(-1.0, x * (1.0 - q));
    return std::exp(log1pxbx(t) * x);
  }

  /// That hat function h(x) = 1 / (x ^ q)
  const RealType h(const RealType x) { return std::exp(-q * std::log(x)); }

  static constexpr RealType epsilon = 1e-8;

  IntType n;                                      //< Number of elements
  RealType q;                                     //< Exponent
  RealType H_x1;                                  //< H(x_1)
  RealType H_n;                                   //< H(n)
  std::uniform_real_distribution<RealType> dist;  //< [H(x_1), H(n)]
};
// -------------------------------------------------------------------------------------
// A Zipf distributed random number generator
// Based on Jim Gray Algorithm as described in "Quickly Generating Billion-Record..."
// -------------------------------------------------------------------------------------
class ZipfDistributionGenerator
{
   // -------------------------------------------------------------------------------------
  private:
   zipf_distribution<> dist;
   std::mt19937 gen;
  public:
   // [0, n)
   ZipfDistributionGenerator(uint64_t ex_n, double theta, uint64_t seed = 42) : dist(ex_n, theta), gen(seed) {}
   // uint64_t rand(u64 new_n);
   uint64_t rand() {
      return dist(gen) - 1;
   }
};

#define PAGE_SIZE 4096

struct mypage {
    char data[PAGE_SIZE];
};

struct mypage2 {
    uint64_t cnt;
    uint64_t state;
    char data[PAGE_SIZE - sizeof(uint64_t) * 2];
};
//mypage2 page_array[1000000];// = new mypage2[npages]

uint64_t vm_array_test(uint64_t npages, uint64_t nops, double skew, int nworkers) {
    #ifdef ENABLE_DUNE
        uint64_t len = sizeof(mypage2) * npages;
        void * ptr = mmap(NULL, len, PROT_READ | PROT_WRITE,
        		   MAP_HUGETLB | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            perror("mmap");
            exit(1);
        }
        mypage2 * vm_array = (mypage2 *)ptr;
        //mypage2 * vm_array = (struct mypage2 *)::memalign(sizeof(mypage2), npages * sizeof(mypage2));
        //mypage2 * vm_array = new mypage2[npages];
    #else
        mypage2 * vm_array = (struct mypage2 *)::memalign(sizeof(mypage2), npages * sizeof(mypage2));
    #endif
    
    uint64_t rip;
    asm volatile("1: lea 1b(%%rip), %0;": "=a"(rip));
    uint64_t sp;
    asm( "mov %%rsp, %0" : "=rm" ( sp ));
    printf("rip %lx vm_array %lx sp %lx\n", rip, (uintptr_t)vm_array, sp);
    #ifdef ENABLE_DUNE
    printf("pgroot %lx\n", (uintptr_t)pgroot);
    #endif
    uint64_t s =0;
    // skip first 10% of the ops as warmup
    uint64_t skip = nops * 0.1;
    //memset(vm_array, 0, sizeof(mypage2) * npages);
    std::vector<std::thread> threads;
    for (int i = 0; i < nworkers; ++i) {
        threads.emplace_back(std::thread([&](int id) {
            #ifdef ENABLE_DUNE
            if (dune_enter()) {
                printf("failed to enter dune mode\n");
                exit(1);
            }
            #endif
            
            //auto id = i;
            uint64_t locals = 0;
            auto start = std::chrono::steady_clock::now();
            uint64_t pid_count = 0;
            ZipfDistributionGenerator g(npages, skew, id);
            for (int j = 0; j < nops; ++j) {
                uint64_t pid = g.rand();
                //uint64_t pid = (pid_count++) % npages;
                if (j == skip) {
                    start = std::chrono::steady_clock::now();
                    locals = 0;
                }
                if (vm_array[pid].state == 0) {
                    locals += vm_array[pid].cnt;
                    vm_array[pid].state = 1;
                } else {
                   locals -= vm_array[pid].cnt;
                    vm_array[pid].state = 0;
                }
            }
            auto end = std::chrono::steady_clock::now();
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

            std::cerr << "vm array worker " << id << " " << nanos / (nops - skip) << " ns/op" << std::endl;
            s += locals;
        },i));
    }

    for (size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }
    //delete[] vm_array;
    #ifdef ENABLE_DUNE
    munmap(ptr, len);
    //free(vm_array);
    //delete[]vm_array;
    #else
    free(vm_array);
    #endif
    return s;
}


uint64_t vm_and_state_array_test(uint64_t npages, uint64_t nops, double skew, int nworkers) {
    uint64_t * state_array = new uint64_t[npages];
    memset(state_array, 0, sizeof(uint64_t) * npages);
    mypage * vm_array = new mypage[npages];
    memset(vm_array, 1, sizeof(mypage) * npages);

    uint64_t s =0;
    std::vector<std::thread> threads;
    for (int i = 0; i < nworkers; ++i) {
        threads.emplace_back(std::thread([&](int id) {
            #ifdef ENABLE_DUNE
            if (dune_enter()) {
                printf("failed to enter dune mode\n");
                exit(1);
            }
            #endif
            uint64_t locals = 0;
            uint64_t idx = 0;
            ZipfDistributionGenerator g(npages, skew, id);
            auto start = std::chrono::steady_clock::now();
            for (int j = 0; j < nops; ++j) {
                uint64_t pid = g.rand();
                if (state_array[pid] == 0) {
                    locals += *(uint64_t*)vm_array[pid].data;
                    state_array[pid] = 1;
                } else {
                    locals -= *(uint64_t*)vm_array[pid].data;
                    state_array[pid] = 0;
                }
            }
            auto end = std::chrono::steady_clock::now();
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

            std::cerr << "vm array + state array worker " << id << " " << nanos / nops << " ns/op" << std::endl;
            s += locals;
        },i));
    }

    for (int i = 0; i < nworkers; ++i) {
        threads[i].join();
    }
    delete[] state_array;
    delete[] vm_array;
    return s;
}

uint64_t indirection_array_test(int npages, uint64_t nops, double skew, uint64_t nworkers) {
    uint64_t *t = new uint64_t[npages];
    mypage2** indirection_array = (mypage2**)(t);
    memset(indirection_array, 0, sizeof(uint64_t) * npages);
    mypage2 * page_frame_array = new mypage2[npages];
    memset(page_frame_array, 1, sizeof(mypage2) * npages);
    for (int i = 0; i < npages; ++i) {
        indirection_array[i] = &page_frame_array[i];
        page_frame_array[i].state = 0;
    }

    uint64_t s =0;
    std::vector<std::thread> threads;
    for (int i = 0; i < nworkers; ++i) {
        threads.emplace_back(std::thread([&](int id) {
            #ifdef ENABLE_DUNE
            if (dune_enter()) {
                printf("failed to enter dune mode\n");
                exit(1);
            }
            #endif
            uint64_t locals = 0;
            ZipfDistributionGenerator g(npages, skew, id);
            auto start = std::chrono::steady_clock::now();
            for (int j = 0; j < nops; ++j) {
                uint64_t pid = g.rand();
                mypage2 * pg = indirection_array[pid];

                if (pg->state == 0) {
                    locals += pg->cnt;
                    pg->state = 1;
                } else {
                    locals -= pg->cnt;
                    pg->state = 0;
                }
            }
            auto end = std::chrono::steady_clock::now();
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

            std::cerr << "PFA worker " << id << " " << nanos / nops << " ns/op" << std::endl;
            s += locals;
        },i));
    }

    for (int i = 0; i < nworkers; ++i) {
        threads[i].join();
    }
    delete[] t;
    delete[] page_frame_array;
    return s;
}


static int pgflt_count = 0;
static void
pgflt_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	int ret;
	ptent_t *pte;
	bool was_user = (tf->cs & 0x3);
    bool nonpresent = (fec & FEC_P) == 0;
    bool protection = fec & FEC_P;
    bool write = fec & FEC_W;
	// if (was_user) {
  //   ret = dune_vm_lookup(pgroot, (void *) addr, nonpresent ? CREATE_NORMAL, &pte);
	// 	assert(!ret);
  //   if (nonpresent) {
  //     *pte |= PTE_W | PTE_ADDR(dune_va_to_pa((void *) addr));
  //   }
  //    |= PTE_U | PTE_P | PTE_W | PTE_ADDR(dune_va_to_pa((void *) addr));
  //   //dune_flush_tlb_one(addr);
	// } else {
		/* XXX use mem lock */
    bool created = false;
    ret = dune_vm_lookup2(pgroot, (void *) addr, CREATE_NORMAL, &created, &pte);
    assert(!ret);

    *pte = PTE_U | PTE_P | PTE_W | PTE_ADDR(dune_va_to_pa((void *) addr));
    //*pte |= PTE_U | PTE_P | PTE_W;// | PTE_ADDR(dune_va_to_pa((void *) addr)
    if (created) {
        //dune_printf("physical page %lx mapped for virtual address %lx\n", PTE_ADDR(*pte), addr);
    }
    ++pgflt_count;
    if (pgflt_count < 10) {
        dune_printf("page fault at v_addr %lx mapped to p_addr %lx pgroot %lx\n", addr, dune_va_to_pa((void *) addr) ,pgroot);
    }
    //dune_flush_tlb_one(addr);
	//}
}

#define USER_FUNC_1 668
typedef void (*user_func_t)(void* arg);
static void syscall_handler(struct dune_tf *tf)
{
  int syscall_num = (int) tf->rax;

	//dune_printf("Got syscall %d\n", syscall_num);
   ++pgflt_count;
  if (syscall_num == 666) {
    dune_ret_from_user(0);
  } else if (syscall_num == 668) {
    ((user_func_t)(tf->rdi))((void*)tf->rsi);
  } else if (syscall_num == 555) {
    return;
  } else {
    dune_passthrough_syscall(tf);
  }
}


int main(int argc, char const *argv[])
{
    #ifdef ENABLE_DUNE
    int ret = dune_init(true);
    if (ret) {
        cerr << "failed to initialize dune" << endl;
        exit(1);
    }
    ret = dune_enter();
    if (ret) {
        cerr << "failed to enter dune" << endl;
        exit(1);
    } else {
        cerr << "entered dune-mode" << endl;
    }
    dune_register_syscall_handler(syscall_handler);
    dune_register_pgflt_handler(pgflt_handler);
    //dune_procmap_dump();
    #endif
    double skew = 0;
    if (argc < 4) {
        std::cerr << "usage: <program> npages nops nworkers [skew]" << std::endl;
        return 1;
    }
    uint64_t npages = std::stoull(argv[1]);
    uint64_t nops = std::stoull(argv[2]);
    uint64_t nworkers = std::stoull(argv[3]);
    if (argc >= 5) {
        skew = std::stof(argv[4]);
    }

    uint64_t s = 0;
    {
        s += vm_array_test(npages, nops, skew, nworkers);
    }

    // {
    //     s += vm_and_state_array_test(npages, nops, skew, nworkers);
    // }
    // {
    //     s += indirection_array_test(npages, nops, skew, nworkers);
    // }
    #ifdef ENABLE_DUNE
    dune_page_stats();
    #endif
    printf("pgflt_count %d\n", pgflt_count);
    return 0;
}
