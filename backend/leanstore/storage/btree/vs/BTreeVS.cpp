#include "BTreeVS.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
using namespace std;
using namespace leanstore::buffermanager;
// -------------------------------------------------------------------------------------
namespace leanstore {
namespace btree {
namespace vs {
// -------------------------------------------------------------------------------------
BTree::BTree()
{
}
// -------------------------------------------------------------------------------------
void BTree::init(DTID dtid)
{
   this->dtid = dtid;
   auto root_write_guard = WritePageGuard<BTreeNode>::allocateNewPage(dtid);
   root_write_guard.init(true);
   root_swip = root_write_guard.bf;
}
// -------------------------------------------------------------------------------------
bool BTree::lookup(u8 *key, unsigned key_length, u64 &payload_length, u8 *result)
{
   u16 level = 0;
   while ( true ) {
      try {
         auto p_guard = ReadPageGuard<BTreeNode>::makeRootGuard(root_lock);
         ReadPageGuard c_guard(p_guard, root_swip);
         while ( !c_guard->is_leaf ) {
            Swip<BTreeNode> &c_swip = c_guard->lookupInner(key, key_length);
            p_guard = std::move(c_guard);
            c_guard = ReadPageGuard(p_guard, c_swip);
            level++;
         }
         p_guard.recheck_done();
         s32 pos = c_guard->lowerBound<true>(key, key_length);
         if ( pos != -1 ) {
            payload_length = c_guard->getPayloadLength(pos);
            memcpy(result, (c_guard->isLarge(pos)) ? c_guard->getPayloadLarge(pos) : c_guard->getPayload(pos), payload_length);
            c_guard.recheck_done();
            return true;
         } else {
            c_guard.recheck_done();
            return false;
         }
      } catch ( RestartException e ) {
         restarts_counter++;
      }
   }
}
// -------------------------------------------------------------------------------------
void BTree::insert(u8 *key, unsigned key_length, u64 payloadLength, u8 *payload)
{
   u32 mask = 1;
   u32 const max = 64; //MAX_BACKOFF
   while ( true ) {
      try {
         auto p_guard = ReadPageGuard<BTreeNode>::makeRootGuard(root_lock);
         ReadPageGuard c_guard(p_guard, root_swip);
         while ( !c_guard->is_leaf ) {
            Swip<BTreeNode> &c_swip = c_guard->lookupInner(key, key_length);
            p_guard = std::move(c_guard);
            c_guard = ReadPageGuard(p_guard, c_swip);
         }
         // -------------------------------------------------------------------------------------
         auto c_x_guard = WritePageGuard(std::move(c_guard));
         p_guard.recheck_done();
         if ( c_x_guard->insert(key, key_length, ValueType(reinterpret_cast<BufferFrame *>(payloadLength)), payload)) {
            return;
         }
         // -------------------------------------------------------------------------------------
         // Release lock
         c_guard = ReadPageGuard(std::move(c_x_guard));
         c_guard.kill();
         // -------------------------------------------------------------------------------------
         trySplit(*c_x_guard.bf);
         continue;
      } catch ( RestartException e ) {
         for ( u32 i = mask; i; --i ) {
            _mm_pause();
         }
         mask = mask < max ? mask << 1 : max;
         restarts_counter++;
      }
   }
}
// -------------------------------------------------------------------------------------
void BTree::trySplit(BufferFrame &to_split)
{
   auto parent_handler = findParent(this, to_split);
   ReadPageGuard<BTreeNode> p_guard = parent_handler.getParentReadPageGuard<BTreeNode>();
   ReadPageGuard<BTreeNode> c_guard = ReadPageGuard(p_guard, parent_handler.swip.cast<BTreeNode>());
   // -------------------------------------------------------------------------------------
   BTreeNode::SeparatorInfo sep_info = c_guard->findSep();
   u8 sep_key[sep_info.length];
   if ( !p_guard.hasBf()) {
      auto p_x_guard = WritePageGuard(std::move(p_guard));
      auto c_x_guard = WritePageGuard(std::move(c_guard));
      assert(height == 1 || !c_x_guard->is_leaf);
      assert(root_swip.bf == c_x_guard.bf);
      // create new root
      auto new_root = WritePageGuard<BTreeNode>::allocateNewPage(dtid, false);
      auto new_left_node = WritePageGuard<BTreeNode>::allocateNewPage(dtid);
      new_root.keepAlive();
      new_left_node.init(c_x_guard->is_leaf);
      new_root.init(false);
      // -------------------------------------------------------------------------------------
      new_root->upper = c_x_guard.bf;
      root_swip.swizzle(new_root.bf);
      // -------------------------------------------------------------------------------------
      c_x_guard->getSep(sep_key, sep_info);
      // -------------------------------------------------------------------------------------
      c_x_guard->split(new_root, new_left_node, sep_info.slot, sep_key, sep_info.length);
      // -------------------------------------------------------------------------------------
      height++;
      return;
   }
   unsigned spaced_need_for_separator = BTreeNode::spaceNeeded(sep_info.length, p_guard->prefix_length);
   if ( p_guard->hasEnoughSpaceFor(spaced_need_for_separator)) { // Is there enough space in the parent for the separator?
      auto p_x_guard = WritePageGuard(std::move(p_guard));
      auto c_x_guard = WritePageGuard(std::move(c_guard));
      p_guard->requestSpaceFor(spaced_need_for_separator);
      assert(p_x_guard.hasBf());
      assert(!p_x_guard->is_leaf);
      // -------------------------------------------------------------------------------------
      auto new_left_node = WritePageGuard<BTreeNode>::allocateNewPage(dtid);
      new_left_node.init(c_x_guard->is_leaf);
      // -------------------------------------------------------------------------------------
      c_x_guard->getSep(sep_key, sep_info);
      // -------------------------------------------------------------------------------------
      c_x_guard->split(p_x_guard, new_left_node, sep_info.slot, sep_key, sep_info.length);
   } else {
      p_guard.kill();
      c_guard.kill();
      trySplit(*p_guard.bf); // Must split parent first to make space for separator
   }
}
// -------------------------------------------------------------------------------------
void BTree::update(u8 *key, unsigned key_length, u64 payloadLength, u8 *payload)
{
   u32 mask = 1;
   u32 const max = 64; //MAX_BACKOFF
   while ( true ) {
      try {
         auto p_guard = ReadPageGuard<BTreeNode>::makeRootGuard(root_lock);
         ReadPageGuard c_guard(p_guard, root_swip);
         while ( !c_guard->is_leaf ) {
            Swip<BTreeNode> &c_swip = c_guard->lookupInner(key, key_length);
            p_guard = std::move(c_guard);
            c_guard = ReadPageGuard(p_guard, c_swip);
         }
         auto c_x_guard = WritePageGuard(std::move(c_guard));
         p_guard.kill();
         if ( c_x_guard->update(key, key_length, payloadLength, payload)) {
            return;
         }
         // no more space, need to split
         // -------------------------------------------------------------------------------------
         // Release lock
         c_guard = ReadPageGuard(std::move(c_x_guard));
         c_guard.kill();
         // -------------------------------------------------------------------------------------
         trySplit(*c_x_guard.bf);
         continue;
      } catch ( RestartException e ) {
         for ( u32 i = mask; i; --i ) {
            _mm_pause();
         }
         mask = mask < max ? mask << 1 : max;
         restarts_counter++;
      }
   }
}
// -------------------------------------------------------------------------------------
bool BTree::remove(u8 *key, unsigned key_length)
{
   /*
    * Plan:
    * check the right (only one) node if it is under filled
    * if yes, then lock exclusively
    * if there was not, and after deletion we got an empty
    * */
   u32 mask = 1;
   u32 const max = 64; //MAX_BACKOFF
   while ( true ) {
      try {
         int pos = -1;
         auto p_guard = ReadPageGuard<BTreeNode>::makeRootGuard(root_lock);
         ReadPageGuard c_guard(p_guard, root_swip);
         while ( !c_guard->is_leaf ) {
            pos = c_guard->lowerBound<false>(key, key_length);
            Swip<BTreeNode> &c_swip = (pos == c_guard->count) ? c_guard->upper : c_guard->getValue(pos);
            p_guard = std::move(c_guard);
            c_guard = ReadPageGuard(p_guard, c_swip);
         }
         auto c_x_guard = WritePageGuard(std::move(c_guard));
         if ( !c_x_guard->remove(key, key_length)) {
            return false;
         }
         if ( !p_guard.hasBf()) {
            // we are root, no merge is possible
            return true;
         }
         if ( c_x_guard->freeSpaceAfterCompaction() >= BTreeNodeHeader::underFullSize ) {
            c_guard = ReadPageGuard(std::move(c_x_guard));
            c_guard.kill();
            try {
               tryMerge(*c_guard.bf);
            } catch ( RestartException e ) {
               // nothing, it is fine not to merge
            }
         }
         return true;
      } catch ( RestartException
                e ) {
         for ( u32 i = mask; i; --i ) {
            _mm_pause();
         }
         mask = mask < max ? mask << 1 : max;
         restarts_counter++;
      }
   }
}
// -------------------------------------------------------------------------------------
void BTree::tryMerge(BufferFrame &to_split)
{
   auto parent_handler = findParent(this, to_split);
   ReadPageGuard<BTreeNode> p_guard = parent_handler.getParentReadPageGuard<BTreeNode>();
   ReadPageGuard<BTreeNode> c_guard = ReadPageGuard(p_guard, parent_handler.swip.cast<BTreeNode>());
   if ( !p_guard.hasBf() || c_guard->freeSpaceAfterCompaction() < BTreeNodeHeader::underFullSize ) {
      return;
   }
   auto p_x_guard = WritePageGuard(std::move(p_guard));
   auto c_x_guard = WritePageGuard(std::move(c_guard));
   assert(parent_handler.swip.bf == &to_split);
   // -------------------------------------------------------------------------------------
   u16 key_length = c_x_guard->getFullKeyLength(0);
   auto key = make_unique<u8[]>(key_length);
   c_x_guard->copyFullKey(0, key.get(), key_length);
   int pos = p_x_guard->lowerBound<false>(key.get(), key_length);
   assert(pos != -1);
   // -------------------------------------------------------------------------------------
   auto merge_left = [&]() {
      Swip<BTreeNode> &l_swip = p_x_guard->getValue(pos - 1);
      auto l_guard = ReadPageGuard(p_x_guard, l_swip);
      if ( l_guard->freeSpaceAfterCompaction() < BTreeNodeHeader::underFullSize ) {
         return false;
      }
      auto l_x_guard = WritePageGuard(std::move(l_guard));
      l_x_guard->merge(pos - 1, p_x_guard, c_x_guard);
      l_x_guard.reclaim();
      removed_bfs++;
      return true;
   };
   auto merge_right = [&]() {
      Swip<BTreeNode> &r_swip = p_x_guard->getValue(pos + 1);
      auto r_guard = ReadPageGuard(p_x_guard, r_swip);
      if ( r_guard->freeSpaceAfterCompaction() < BTreeNodeHeader::underFullSize ) {
         return false;
      }
      auto r_x_guard = WritePageGuard(std::move(r_guard));
      c_x_guard->merge(pos, p_x_guard, r_x_guard);
      c_x_guard.reclaim();
      removed_bfs++;
      return true;
   };
   // -------------------------------------------------------------------------------------
   bool merged_right = false;
   if ( p_x_guard->count >= 2 ) {
      if ( pos + 1 < p_x_guard->count ) {
         if ( merge_right()) {
            merged_right = true;
         }
      }
      if ( !merged_right ) {
         c_guard = ReadPageGuard<BTreeNode>(std::move(c_x_guard));
         c_guard.kill();
      }
      if ( !merged_right && pos > 0 ) {
         merge_left();
      }
   }
   // -------------------------------------------------------------------------------------
   p_guard = ReadPageGuard<BTreeNode>(std::move(p_x_guard));
   if ( p_guard.hasBf() && p_guard->freeSpaceAfterCompaction() >= BTreeNode::underFullSize && root_swip.bf != p_guard.bf ) {
      tryMerge(*p_x_guard.bf);
   }
}
// -------------------------------------------------------------------------------------
BTree::~BTree()
{
}
// -------------------------------------------------------------------------------------
struct DTRegistry::DTMeta BTree::getMeta()
{
   DTRegistry::DTMeta btree_meta = {
           .iterate_children=iterateChildrenSwips, .find_parent = findParent
   };
   return btree_meta;
}
// -------------------------------------------------------------------------------------
struct ParentSwipHandler BTree::findParent(void *btree_object, BufferFrame &to_find)
{
   // Pre: bf is write locked TODO: but trySplit does not ex lock !
   auto &c_node = *reinterpret_cast<BTreeNode *>(to_find.page.dt);
   auto &btree = *reinterpret_cast<BTree *>(btree_object);
   Swip<BTreeNode> *c_swip = &btree.root_swip;
   u16 level = 0;
   // -------------------------------------------------------------------------------------
   auto p_guard = ReadPageGuard<BTreeNode>::makeRootGuard(btree.root_lock);
   // -------------------------------------------------------------------------------------
   const bool infinity = (c_node.upper_fence.offset == 0) ? true : false;
   u16 key_length = c_node.upper_fence.length;
   u8 *key = c_node.getUpperFenceKey();
   // -------------------------------------------------------------------------------------
   // check if bf is the root node
   if ( c_swip->bf == &to_find ) {
      p_guard.kill();
      return {
              .swip = c_swip->cast<BufferFrame>(), .guard = p_guard.bf_s_lock, .parent = nullptr
      };
   }
   // -------------------------------------------------------------------------------------
   ReadPageGuard c_guard(p_guard, btree.root_swip);
   s32 pos;
   auto search_condition = [&]() {
      if ( infinity ) {
         c_swip = &(c_guard->upper);
      } else {
         pos = c_guard->lowerBound<false>(key, key_length);
         if ( pos == c_guard->count ) {
            c_swip = &(c_guard->upper);
         } else {
            c_swip = &(c_guard->getValue(pos));
         }
      }
      return (c_swip->bf != &to_find);
   };
   while ( !c_guard->is_leaf && search_condition()) {
      p_guard = std::move(c_guard);
      c_guard = ReadPageGuard(p_guard, c_swip->cast<BTreeNode>());
      level++;
   }
   p_guard.kill();
   if ( c_swip->bf != &to_find ) {
      throw RestartException();
   }
   return {.swip = c_swip->cast<BufferFrame>(), .guard = c_guard.bf_s_lock, .parent = c_guard.bf};
}
// -------------------------------------------------------------------------------------
void BTree::iterateChildrenSwips(void *, BufferFrame &bf, std::function<bool(Swip<BufferFrame> &)> callback)
{
   // Pre: bf is read locked
   auto &c_node = *reinterpret_cast<BTreeNode *>(bf.page.dt);
   if ( c_node.is_leaf ) {
      return;
   }
   for ( u16 i = 0; i < c_node.count; i++ ) {
      if ( !callback(c_node.getValue(i).cast<BufferFrame>())) {
         return;
      }
   }
   callback(c_node.upper.cast<BufferFrame>());
}
// Helpers
// -------------------------------------------------------------------------------------
unsigned BTree::countPages()
{
   auto p_guard = ReadPageGuard<BTreeNode>::makeRootGuard(root_lock);
   ReadPageGuard c_guard(p_guard, root_swip);
   p_guard.recheck_done();
   c_guard.recheck_done();
   return iterateAllPages(c_guard, [](BTreeNode &) {
      return 1;
   }, [](BTreeNode &) {
      return 1;
   });
}
// -------------------------------------------------------------------------------------
s64 BTree::iterateAllPages(ReadPageGuard<BTreeNode> &node_guard, std::function<s64(BTreeNode &)> inner, std::function<s64(BTreeNode &)> leaf)
{
   if ( node_guard->is_leaf ) {
      return leaf(node_guard.ref());
   }
   s64 res = inner(node_guard.ref());
   for ( u16 i = 0; i < node_guard->count; i++ ) {
      Swip<BTreeNode> &c_swip = node_guard->getValue(i);
      auto c_guard = ReadPageGuard(node_guard, c_swip);
      c_guard.recheck_done();
      res += iterateAllPages(c_guard, inner, leaf);
   }
   // -------------------------------------------------------------------------------------
   Swip<BTreeNode> &c_swip = node_guard->upper;
   auto c_guard = ReadPageGuard(node_guard, c_swip);
   c_guard.recheck_done();
   res += iterateAllPages(c_guard, inner, leaf);
   // -------------------------------------------------------------------------------------
   return res;
}
// -------------------------------------------------------------------------------------
unsigned BTree::countInner()
{
   auto p_guard = ReadPageGuard<BTreeNode>::makeRootGuard(root_lock);
   ReadPageGuard c_guard(p_guard, root_swip);
   p_guard.recheck_done();
   c_guard.recheck_done();
   return iterateAllPages(c_guard, [](BTreeNode &) {
      return 1;
   }, [](BTreeNode &) {
      return 0;
   });
}
// -------------------------------------------------------------------------------------
unsigned BTree::bytesFree()
{
   auto p_guard = ReadPageGuard<BTreeNode>::makeRootGuard(root_lock);
   ReadPageGuard c_guard(p_guard, root_swip);
   p_guard.recheck_done();
   c_guard.recheck_done();
   return iterateAllPages(c_guard, [](BTreeNode &inner) {
      return inner.freeSpaceAfterCompaction();
   }, [](BTreeNode &leaf) {
      return leaf.freeSpaceAfterCompaction();
   });
}
// -------------------------------------------------------------------------------------
void BTree::printInfos(uint64_t totalSize)
{
   auto p_guard = ReadPageGuard<BTreeNode>::makeRootGuard(root_lock);
   ReadPageGuard r_guard(p_guard, root_swip);
   p_guard.recheck_done();
   r_guard.recheck_done();
   uint64_t cnt = countPages();
   cout << "nodes:" << cnt << " innerNodes:" << countInner() << " space:" << (cnt * EFFECTIVE_PAGE_SIZE) / (float) totalSize << " height:" << height << " rootCnt:" << r_guard->count << " bytesFree:" << bytesFree() << endl;
}
// -------------------------------------------------------------------------------------
}
}
}