#include "BTreeVI.hpp"
#include "leanstore/concurrency-recovery/CRMG.hpp"
// -------------------------------------------------------------------------------------
#include "gflags/gflags.h"
// -------------------------------------------------------------------------------------
#include <signal.h>

#include <map>
#include <set>
#include <unordered_map>
// -------------------------------------------------------------------------------------
using namespace std;
using namespace leanstore::storage;
using OP_RESULT = leanstore::OP_RESULT;
// -------------------------------------------------------------------------------------
namespace std
{
template <>
class hash<leanstore::UpdateSameSizeInPlaceDescriptor::Slot>
{
  public:
   size_t operator()(const leanstore::UpdateSameSizeInPlaceDescriptor::Slot& slot) const
   {
      size_t result = (slot.length << 16) | slot.offset;
      return result;
   }
};
}  // namespace std
// -------------------------------------------------------------------------------------
namespace leanstore::storage::btree
{
// -------------------------------------------------------------------------------------
void BTreeVI::FatTupleDifferentAttributes::undoLastUpdate()
{
   ensure(deltas_count >= 1);
   auto& delta = *reinterpret_cast<Delta*>(payload + value_length);
   worker_id = delta.worker_id;
   tx_id = delta.tx_id;
   deltas_count -= 1;
   const u32 total_freed_space = sizeof(Delta) + delta.getDescriptor().size() + delta.getDescriptor().diffLength();
   BTreeLL::applyDiff(delta.getDescriptor(), getValue(), delta.payload + delta.getDescriptor().size());
   {
      const u32 dest = value_length;
      const u32 src = value_length + total_freed_space;
      const u32 bytes_to_move = used_space - src;
      std::memmove(payload + dest, payload + src, bytes_to_move);
   }
   used_space -= total_freed_space;
}
// -------------------------------------------------------------------------------------
void BTreeVI::FatTupleDifferentAttributes::garbageCollection(BTreeVI& btree, bool heavyweight)
{
   if (deltas_count == 0) {
      return;
   }
   if (deltas_count >= cr::Worker::my().workers_count) {
      heavyweight = true;
   }
   // -------------------------------------------------------------------------------------
   const bool pgc = (FLAGS_pgc) && deltas_count >= 1;
   // -------------------------------------------------------------------------------------
   if (cr::Worker::my().isVisibleForAll(tx_id)) {
      COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_versions_removed[btree.dt_id] += deltas_count; }
      used_space = value_length;
      deltas_count = 0;
   } else if (pgc) {
      // -------------------------------------------------------------------------------------
      // Precise garbage collection
      // The idea behind it: check that all transaction that *could* (with larger tx_id than victim) see victim, see its successor (prev_tx_id,
      // prev_worker_id here)
      COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains_pgc[btree.dt_id]++; }
      COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_versions_visited[btree.dt_id] += deltas_count; }
      TXID prev_delta_worker_id = worker_id;
      TXID prev_delta_tx_id = tx_id;
      // -------------------------------------------------------------------------------------
      auto delta = reinterpret_cast<Delta*>(payload + value_length);
      u64 offset = value_length;
      u64 delta_i = 0;
      std::vector<Delta*> deltas_to_merge;
      // -------------------------------------------------------------------------------------
      auto can_we_delete_light = [&](const u64 prev_worker_id, const u64 prev_tx_id, const u64 cur_worker_id, const u64 cur_tx_id) {
         if (cr::Worker::my().isVisibleForAll(prev_tx_id)) {
            COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains_pgc_light_removed[btree.dt_id]++; }
            return true;
         }
         // -------------------------------------------------------------------------------------
         if (cr::Worker::my().global_oltp_lwm > prev_tx_id) {  // There is a chance to quickly determine the result
            for (const auto& olap_worker_id : cr::Worker::my().local_seen_olap_workers) {
               if (cr::Worker::my().isVisibleForIt(olap_worker_id, cur_worker_id, cur_tx_id) != cr::Worker::VISIBILITY::VISIBLE_NEXT_ROUND) {
                  return false;
               }
            }
            COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains_pgc_light_removed[btree.dt_id]++; }
            return true;
         }
         // -------------------------------------------------------------------------------------
         return false;
      };
      auto can_we_delete_heavy = [&](const u64 prev_worker_id, const u64 prev_tx_id, const u64 cur_worker_id, const u64 cur_tx_id) {
         if (cr::Worker::my().isVisibleForAll(prev_tx_id)) {
            COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains_pgc_light_removed[btree.dt_id]++; }
            return true;
         }
         // -------------------------------------------------------------------------------------
         for (u64 w_i = 0; w_i < cr::Worker::my().workers_count; w_i++) {
            const auto prev_visibility = cr::Worker::my().isVisibleForIt(w_i, prev_worker_id, prev_tx_id);
            if (prev_visibility == cr::Worker::VISIBILITY::VISIBLE_ALREADY) {
               continue;
            } else if (prev_visibility == cr::Worker::VISIBILITY::UNDETERMINED) {
               COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains_pgc_skipped[btree.dt_id]++; }
               return false;
            } else {  // prev is not visible atm but will be next round
               // cur must be visible next work
               const cr::Worker::VISIBILITY cur_visibility = cr::Worker::my().isVisibleForIt(w_i, cur_worker_id, cur_tx_id);
               if (cur_visibility == cr::Worker::VISIBILITY::UNDETERMINED) {
                  COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains_pgc_skipped[btree.dt_id]++; }
                  return false;
               } else if (cur_visibility == cr::Worker::VISIBILITY::VISIBLE_ALREADY) {
                  return false;
               } else {
                  continue;
               }
            }
         }
         COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains_pgc_heavy_removed[btree.dt_id]++; }
         return true;
      };
      while (true) {
         explainWhen(delta_i > 500);
         bool should_delete;
         if (delta->worker_id == cr::Worker::my().workerID() && delta->tx_id == cr::activeTX().TTS()) {
            should_delete = false;
         } else {
            if (heavyweight) {
               COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains_pgc_heavy[btree.dt_id]++; }
               should_delete = can_we_delete_heavy(prev_delta_worker_id, prev_delta_tx_id, delta->worker_id, delta->tx_id);
            } else {
               COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains_pgc_light[btree.dt_id]++; }
               should_delete = can_we_delete_light(prev_delta_worker_id, prev_delta_tx_id, delta->worker_id, delta->tx_id);
            }
         }
         if (should_delete) {
            deltas_to_merge.push_back(delta);
         } else if (deltas_to_merge.size()) {
            // Merge all deltas in deltas_to_merge in delta*
            // Check whether all attributes are the same
            bool same_attributes = true;
            UpdateSameSizeInPlaceDescriptor& last_descriptor = delta->getDescriptor();
            for (u64 d_i = 0; same_attributes && d_i < deltas_to_merge.size(); d_i++) {
               same_attributes &= last_descriptor == deltas_to_merge[d_i]->getDescriptor();
            }
            COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_versions_removed[btree.dt_id] += deltas_to_merge.size(); }
            if (same_attributes) {
               ensure(used_space >= (reinterpret_cast<u8*>(delta) - payload));
               std::memmove(deltas_to_merge.front(), delta, used_space - (reinterpret_cast<u8*>(delta) - payload));
               const u64 freed_space = reinterpret_cast<u8*>(delta) - reinterpret_cast<u8*>(deltas_to_merge.front());
               deltas_count -= deltas_to_merge.size();
               delta_i -= deltas_to_merge.size();
               used_space -= freed_space;
               delta = deltas_to_merge.front();
               offset = reinterpret_cast<u8*>(delta) - payload;
               deltas_to_merge.clear();
            } else {
               using Slot = UpdateSameSizeInPlaceDescriptor::Slot;
               std::unordered_map<Slot, std::basic_string<u8>> slots_map;
               for (auto m_delta : deltas_to_merge) {  // TODO: reverse order
                  auto& m_descriptor = m_delta->getDescriptor();
                  u8* delta_diff_ptr = m_delta->payload + m_descriptor.size();
                  for (u64 s_i = 0; s_i < m_delta->getDescriptor().count; s_i++) {
                     slots_map[m_descriptor.slots[s_i]] = std::basic_string<u8>(delta_diff_ptr, m_descriptor.slots[s_i].length);
                     delta_diff_ptr += m_descriptor.slots[s_i].length;
                  }
               }
               u8 merge_result[PAGE_SIZE];
               auto& merge_delta = *reinterpret_cast<Delta*>(merge_result);
               merge_delta = *delta;
               UpdateSameSizeInPlaceDescriptor& merge_descriptor = merge_delta.getDescriptor();
               merge_descriptor.count = slots_map.size();
               u8* merge_diff_ptr = merge_delta.payload + merge_descriptor.size();
               u32 s_i = 0;
               for (auto& slot_itr : slots_map) {
                  merge_descriptor.slots[s_i++] = slot_itr.first;
                  std::memcpy(merge_diff_ptr, slot_itr.second.c_str(), slot_itr.second.size());
                  merge_diff_ptr += slot_itr.second.size();
               }
               const u32 total_merge_delta_size = merge_diff_ptr - merge_result;
               const u32 space_to_replace = (reinterpret_cast<u8*>(delta) - reinterpret_cast<u8*>(deltas_to_merge.front())) + sizeof(Delta) +
                                            delta->getDescriptor().size() + delta->getDescriptor().diffLength();
               ensure(space_to_replace >= total_merge_delta_size);
               const u16 copy_dst = reinterpret_cast<u8*>(deltas_to_merge.front()) - payload;
               std::memcpy(payload + copy_dst, merge_result, total_merge_delta_size);
               std::memmove(payload + copy_dst + total_merge_delta_size, payload + copy_dst + space_to_replace,
                            used_space - (copy_dst + space_to_replace));
               used_space -= space_to_replace - total_merge_delta_size;
               // -------------------------------------------------------------------------------------
               delta_i -= deltas_to_merge.size();
               deltas_count -= deltas_to_merge.size();
               offset = copy_dst;
               delta = deltas_to_merge.front();
               // -------------------------------------------------------------------------------------
               deltas_to_merge.clear();
            }
         }
         // Next version
         delta_i++;
         offset += sizeof(Delta) + delta->getDescriptor().size() + delta->getDescriptor().diffLength();
         if (delta_i < deltas_count) {
            prev_delta_tx_id = delta->tx_id;
            prev_delta_worker_id = delta->worker_id;
            delta = reinterpret_cast<Delta*>(payload + offset);
            continue;
         } else {
            break;
         }
      }
      if (deltas_to_merge.size()) {
         used_space = reinterpret_cast<u8*>(deltas_to_merge.front()) - payload;
         deltas_count -= deltas_to_merge.size();
         COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_versions_removed[btree.dt_id] += deltas_to_merge.size(); }
      }
      // explainWhen(deltas_count > (cr::Worker::my().workers_count + 2));
   }
   // explainWhen(deltas_count > (cr::Worker::my().workers_count + 2));
}
// -------------------------------------------------------------------------------------
// Pre: tuple is write locked
bool BTreeVI::FatTupleDifferentAttributes::update(BTreeExclusiveIterator& iterator,
                                                  u8* o_key,
                                                  u16 o_key_length,
                                                  function<void(u8* value, u16 value_size)> cb,
                                                  UpdateSameSizeInPlaceDescriptor& update_descriptor,
                                                  BTreeVI& btree)
{
   bool heavyweight_gc = false;
cont : {
   auto fat_tuple = reinterpret_cast<FatTupleDifferentAttributes*>(iterator.mutableValue().data());
   if (FLAGS_vi_fupdate_fat_tuple) {
      cb(fat_tuple->getValue(), fat_tuple->value_length);
      return true;
   }
   // -------------------------------------------------------------------------------------
   // Attention: we have to disable garbage collection if the latest delta was from us and not committed yet!
   // Otherwise we would crash during undo although the end result is the same if the transaction would commit (overwrite)
   const u32 descriptor_and_diff_length = update_descriptor.size() + update_descriptor.diffLength();
   const u32 needed_space = sizeof(Delta) + descriptor_and_diff_length;
   if (fat_tuple->deltas_count > 0) {  // fat_tuple->total_space < (needed_space + fat_tuple->used_space) &&
                                       // Garbage collection first
      fat_tuple->garbageCollection(btree, heavyweight_gc);
   }
   if (fat_tuple->total_space < (needed_space + fat_tuple->used_space)) {
      const u32 fat_tuple_length = needed_space + fat_tuple->used_space + sizeof(FatTupleDifferentAttributes);
      u8 buffer[PAGE_SIZE];
      ensure(iterator.value().length() <= PAGE_SIZE);
      std::memcpy(buffer, iterator.value().data(), iterator.value().length());
      // -------------------------------------------------------------------------------------
      const bool did_extend = iterator.extendPayload(fat_tuple_length);
      if (did_extend == false) {
         if (heavyweight_gc == true) {
            TODOException();  // TODO; Fallback to chained (decompose)
         }
         heavyweight_gc = true;
         goto cont;
      }
      // -------------------------------------------------------------------------------------
      std::memcpy(iterator.mutableValue().data(), buffer, fat_tuple_length);
      fat_tuple = reinterpret_cast<FatTupleDifferentAttributes*>(iterator.mutableValue().data());  // ATTENTION
      fat_tuple->total_space = needed_space + fat_tuple->used_space;
   }
   if (fat_tuple->deltas_count > 0) {
      // Make place for a new delta
      const u64 src = fat_tuple->value_length;
      const u64 dst = fat_tuple->value_length + needed_space;
      std::memmove(fat_tuple->payload + dst, fat_tuple->payload + src, fat_tuple->used_space - src);
   }
   // -------------------------------------------------------------------------------------
   {
      // Insert the new delta
      auto& new_delta = *new (fat_tuple->payload + fat_tuple->value_length) Delta();
      new_delta.worker_id = fat_tuple->worker_id;
      new_delta.tx_id = fat_tuple->tx_id;
      new_delta.command_id = fat_tuple->command_id;
      std::memcpy(new_delta.payload, &update_descriptor, update_descriptor.size());
      BTreeLL::generateDiff(update_descriptor, new_delta.payload + update_descriptor.size(), fat_tuple->getValue());
      fat_tuple->used_space += needed_space;
      fat_tuple->deltas_count++;
   }
   ensure(fat_tuple->total_space >= fat_tuple->used_space);
   // -------------------------------------------------------------------------------------
   {
      // WAL
      const u16 delta_and_descriptor_size = update_descriptor.size() + update_descriptor.diffLength();
      auto wal_entry = iterator.leaf.reserveWALEntry<WALUpdateSSIP>(o_key_length + delta_and_descriptor_size);
      wal_entry->type = WAL_LOG_TYPE::WALUpdate;
      wal_entry->key_length = o_key_length;
      wal_entry->delta_length = delta_and_descriptor_size;
      wal_entry->before_worker_id = fat_tuple->worker_id;
      wal_entry->before_tx_id = fat_tuple->tx_id;
      // -------------------------------------------------------------------------------------
      fat_tuple->worker_id = cr::Worker::my().workerID();
      fat_tuple->tx_id = cr::activeTX().TTS();
      fat_tuple->command_id = cr::Worker::my().command_id++;  // A version is not inserted in versions space however. Needed for decompose
      // -------------------------------------------------------------------------------------
      std::memcpy(wal_entry->payload, o_key, o_key_length);
      std::memcpy(wal_entry->payload + o_key_length, &update_descriptor, update_descriptor.size());
      // Update the value in-place
      BTreeLL::generateDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), fat_tuple->getValue());
      cb(fat_tuple->getValue(), fat_tuple->value_length);
      BTreeLL::generateXORDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), fat_tuple->getValue());
      wal_entry.submit();
      // -------------------------------------------------------------------------------------
      if (cr::activeTX().isSerializable()) {
         fat_tuple->read_ts = cr::activeTX().TTS();
      }
   }
   assert(fat_tuple->deltas_count > 0);
   return true;
}
}
// -------------------------------------------------------------------------------------
std::tuple<OP_RESULT, u16> BTreeVI::FatTupleDifferentAttributes::reconstructTuple(std::function<void(Slice value)> cb) const
{
   if (cr::Worker::my().isVisibleForMe(worker_id, tx_id)) {
      // Latest version is visible
      cb(Slice(getValueConstant(), value_length));
      return {OP_RESULT::OK, 1};
   } else if (deltas_count > 0) {
      u8 materialized_value[value_length];
      std::memcpy(materialized_value, getValueConstant(), value_length);
      // we have to apply the diffs
      u16 delta_i = 0;
      u32 offset = value_length;
      auto delta = reinterpret_cast<const Delta*>(payload + offset);
      while (delta_i < deltas_count) {
         if (cr::Worker::my().isVisibleForMe(delta->worker_id, delta->tx_id)) {
            BTreeLL::applyDiff(delta->getConstantDescriptor(), materialized_value,
                               delta->payload + delta->getConstantDescriptor().size());  // Apply diff
            cb(Slice(materialized_value, value_length));
            return {OP_RESULT::OK, delta_i + 2};
         }
         // -------------------------------------------------------------------------------------
         delta_i++;
         offset += sizeof(Delta) + delta->getConstantDescriptor().size() + delta->getConstantDescriptor().diffLength();
         delta = reinterpret_cast<const Delta*>(payload + offset);
      }
      // -------------------------------------------------------------------------------------
      explainWhen(cr::activeTX().isOLTP());
      return {OP_RESULT::NOT_FOUND, delta_i + 1};
   } else {
      explainWhen(cr::activeTX().isOLTP());
      return {OP_RESULT::NOT_FOUND, 1};
   }
}
// -------------------------------------------------------------------------------------
bool BTreeVI::convertChainedToFatTupleDifferentAttributes(BTreeExclusiveIterator& iterator)
{
   u16 number_of_deltas_to_replace = 0;
   std::vector<u8> dynamic_buffer;
   dynamic_buffer.resize(PAGE_SIZE * 4);
   auto fat_tuple = new (dynamic_buffer.data()) FatTupleDifferentAttributes();
   fat_tuple->total_space = EFFECTIVE_PAGE_SIZE * 3.0 / 4.0;  // TODO: set a proper maximum
   fat_tuple->used_space = 0;
   // -------------------------------------------------------------------------------------
   WORKERID next_worker_id;
   TXID next_tx_id;
   COMMANDID next_command_id;
   // -------------------------------------------------------------------------------------
   // Process the chain head
   MutableSlice head = iterator.mutableValue();
   auto& chain_head = *reinterpret_cast<ChainedTuple*>(head.data());
   ensure(chain_head.isWriteLocked());
   // -------------------------------------------------------------------------------------
   fat_tuple->value_length = head.length() - sizeof(ChainedTuple);
   std::memcpy(fat_tuple->payload + fat_tuple->used_space, chain_head.payload, fat_tuple->value_length);
   fat_tuple->used_space += fat_tuple->value_length;
   fat_tuple->worker_id = chain_head.worker_id;
   fat_tuple->tx_id = chain_head.tx_id;
   // -------------------------------------------------------------------------------------
   next_worker_id = chain_head.worker_id;
   next_tx_id = chain_head.tx_id;
   next_command_id = chain_head.command_id;
   // TODO: check for used_space overflow
   while (true) {
      if (cr::Worker::my().isVisibleForAll(next_tx_id)) {  // Pruning versions space might get delayed
         break;
      }
      // -------------------------------------------------------------------------------------
      if (!cr::Worker::my().retrieveVersion(next_worker_id, next_tx_id, next_command_id, [&](const u8* version, [[maybe_unused]] u64 payload_length) {
             number_of_deltas_to_replace++;
             const auto& chain_delta = *reinterpret_cast<const UpdateVersion*>(version);
             ensure(chain_delta.type == Version::TYPE::UPDATE);
             ensure(chain_delta.is_delta);
             const auto& update_descriptor = *reinterpret_cast<const UpdateSameSizeInPlaceDescriptor*>(chain_delta.payload);
             const u32 descriptor_and_diff_length = update_descriptor.size() + update_descriptor.diffLength();
             const u32 needed_space = sizeof(FatTupleDifferentAttributes::Delta) + descriptor_and_diff_length;
             // -------------------------------------------------------------------------------------
             if ((fat_tuple->used_space + sizeof(FatTupleDifferentAttributes) + needed_space) >= dynamic_buffer.size()) {
                dynamic_buffer.resize(fat_tuple->used_space + sizeof(FatTupleDifferentAttributes) + needed_space);
                fat_tuple = reinterpret_cast<FatTupleDifferentAttributes*>(dynamic_buffer.data());
             }
             // -------------------------------------------------------------------------------------
             // Add a FatTuple::Delta
             auto& new_delta = *new (fat_tuple->payload + fat_tuple->used_space) FatTupleDifferentAttributes::Delta();
             fat_tuple->used_space += sizeof(FatTupleDifferentAttributes::Delta);
             new_delta.worker_id = chain_delta.worker_id;
             new_delta.tx_id = chain_delta.tx_id;
             new_delta.command_id = chain_delta.command_id;
             // -------------------------------------------------------------------------------------
             // Copy Descriptor + Diff
             std::memcpy(fat_tuple->payload + fat_tuple->used_space, &update_descriptor, descriptor_and_diff_length);
             fat_tuple->deltas_count++;
             fat_tuple->used_space += descriptor_and_diff_length;
             // -------------------------------------------------------------------------------------
             next_worker_id = chain_delta.worker_id;
             next_tx_id = chain_delta.tx_id;
             next_command_id = chain_delta.command_id;
             fat_tuple->garbageCollection(*this, true);
          })) {
         break;
      }
   }
   if (fat_tuple->used_space > fat_tuple->total_space) {
      chain_head.unlock();
      return false;
   }
   fat_tuple->total_space = fat_tuple->used_space;
   if (number_of_deltas_to_replace >= convertToFatTupleThreshold()) {
      // Finalize the new FatTuple
      // TODO: corner cases, more careful about space usage
      // -------------------------------------------------------------------------------------
      ensure(fat_tuple->total_space >= fat_tuple->used_space);
      const u16 fat_tuple_length = sizeof(FatTupleDifferentAttributes) + fat_tuple->total_space;
      if (iterator.value().length() < fat_tuple_length) {
         ensure(reinterpret_cast<const Tuple*>(iterator.value().data())->tuple_format == TupleFormat::CHAINED);
         const bool did_extend = iterator.extendPayload(fat_tuple_length);
         ensure(did_extend);
      } else {
         iterator.shorten(fat_tuple_length);
      }
      std::memcpy(iterator.mutableValue().data(), dynamic_buffer.data(), fat_tuple_length);
      iterator.markAsDirty();
      return true;
   } else {
      chain_head.unlock();
      return false;
   }
}
// -------------------------------------------------------------------------------------
}  // namespace leanstore::storage::btree