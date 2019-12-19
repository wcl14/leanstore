#pragma once
#include "Units.hpp"
// -------------------------------------------------------------------------------------
#include "PerfEvent.hpp"
// -------------------------------------------------------------------------------------
#include <atomic>
#include <unordered_map>
// -------------------------------------------------------------------------------------
namespace leanstore
{
struct DebuggingCounters {
  atomic<s64> phase_1_ms = 0, phase_2_ms = 0, phase_3_ms = 0, poll_ms = 0;
  // Phase 3 detailed
  atomic<u64> async_wb_ms = 0, submit_ms = 0;
  atomic<u64> phase_1_counter = 0, phase_2_counter = 0, phase_3_counter = 0;
  // -------------------------------------------------------------------------------------
  atomic<u64> evicted_pages = 0, pp_thread_rounds = 0;
  // -------------------------------------------------------------------------------------
  // Only for page provider thread
  atomic<u64> flushed_pages_counter = 0;
  atomic<u64> unswizzled_pages_counter = 0;
  // -------------------------------------------------------------------------------------
  // For workers
  atomic<u64> read_operations_counter = 0;
  atomic<u64> hot_hit_counter = 0;
  atomic<u64> cold_hit_counter = 0;
  atomic<u64> allocate_operations_counter = 0;
  atomic<u64> tx_rate = 0;
  // -------------------------------------------------------------------------------------
  std::unordered_map<u64, atomic<u64>> dt_misses_counter;
  // -------------------------------------------------------------------------------------
  struct ThreadLocalCounters {
    // Does not have to be atomic, but I like exchange func
    atomic<u64> hot_hit_counter = 0;
    atomic<u64> cold_hit_counter = 0;
    atomic<u64> read_operations_counter = 0;
    atomic<u64> allocate_operations_counter = 0;
  };
  static thread_local ThreadLocalCounters thread_local_counters;
};
}  // namespace leanstore
   // -------------------------------------------------------------------------------------