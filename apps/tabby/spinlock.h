// #pragma once
 
//  #include <cassert>
//  #include <atomic>
//  #ifdef __LINUX
//  #include <new>
//  #endif
 
//  // Spinlock implementation that matches std::mutex interface.
//  // So you can use std::lock_guard, etc.
//  //
//  // I'm aware of all the discussions of why spinlocks are bad.
//  // The fact remains that timing tests consistently show
//  // F3 slows down considerably if you replace this with std::mutex.
 
//  // If you define this, then the spinlock is implemented in terms of a std::mutex
//  // shoudl be equivalent to std::mutex
//  //#define USEMUTEX
 
 
//  #ifdef USEMUTEX
//  #include <mutex>
//  class spinlock : public std::mutex {
//  public:
//   using mutex::mutex;
//  };
//  #else
//  class spinlock {
//  public:
//   spinlock() { new(buffer) std::atomic<bool>(false); }
 
//   ~spinlock() { assert(!get().load()); get().~atomic<bool>(); }
 
//   inline void lock() { while (get().exchange(true)) { } };
 
//   inline void unlock() { get().store(false); } // Added for standard conformity
 
//   inline bool try_lock() { return !get().exchange(true); } // Added for standard conformity
 
//  private:
//   spinlock(const spinlock &) = delete;
//   void operator=(const spinlock &) = delete;
 
//   std::atomic<bool> &get() { return *reinterpret_cast<std::atomic<bool> *>(buffer); }
 
//   char buffer[sizeof(std::atomic<bool>)];
//  };
//  #endif
 
//  // Standard library looking reverse_lock for people using lock_guard, unique_lock, etc.
//  template <typename T> 
//  class reverse_lock {
//  public:
//   reverse_lock(T &t) : lock_(t) { t.unlock(); }
//   ~reverse_lock() { lock_.lock(); }
//  private:
//   T &lock_;
//  };
 
//  // Standard library looking conditional_lock
//  template <typename T>
//  class conditional_lock {
//  public:
//   conditional_lock(T &t, bool use) : lock_(&t), use_(use) { if (use_) t.lock(); }
//   conditional_lock(T *t, bool use) : lock_(t), use_(use) { if (use_&&t) t->lock(); }
//   ~conditional_lock() { if (use_&&lock_) lock_->unlock(); }
//  private:
//   T *lock_;
//   bool use_;
//  };
 
//  // Standard library looking reverse_lock for people using lock_guard, unique_lock, etc.
//  template <typename T> 
//  class reverse_shared_lock {
//  public:
//   reverse_shared_lock(T &t) : lock_(t) { t.unlock_shared(); }
//   ~reverse_shared_lock() { lock_.lock_shared(); }
//  private:
//   T &lock_;
//  };
 
 
 
//  // EoF