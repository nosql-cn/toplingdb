//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/two_level_iterator.h"
#include "db/pinned_iterators_manager.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "table/block.h"
#include "table/format.h"
#include "util/arena.h"
#include "util/heap.h"

namespace rocksdb {

namespace {

class TwoLevelIndexIterator : public InternalIteratorBase<BlockHandle> {
 public:
  explicit TwoLevelIndexIterator(
      TwoLevelIteratorState* state,
      InternalIteratorBase<BlockHandle>* first_level_iter);

  virtual ~TwoLevelIndexIterator() {
    first_level_iter_.DeleteIter(false /* is_arena_mode */);
    second_level_iter_.DeleteIter(false /* is_arena_mode */);
    delete state_;
  }

  virtual void Seek(const Slice& target) override;
  virtual void SeekForPrev(const Slice& target) override;
  virtual void SeekToFirst() override;
  virtual void SeekToLast() override;
  virtual void Next() override;
  virtual void Prev() override;

  virtual bool Valid() const override { return second_level_iter_.Valid(); }
  virtual Slice key() const override {
    assert(Valid());
    return second_level_iter_.key();
  }
  virtual BlockHandle value() const override {
    assert(Valid());
    return second_level_iter_.value();
  }
  virtual Status status() const override {
    if (!first_level_iter_.status().ok()) {
      assert(second_level_iter_.iter() == nullptr);
      return first_level_iter_.status();
    } else if (second_level_iter_.iter() != nullptr &&
               !second_level_iter_.status().ok()) {
      return second_level_iter_.status();
    } else {
      return status_;
    }
  }
  virtual void SetPinnedItersMgr(
      PinnedIteratorsManager* /*pinned_iters_mgr*/) override {}
  virtual bool IsKeyPinned() const override { return false; }
  virtual bool IsValuePinned() const override { return false; }

 private:
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }
  void SkipEmptyDataBlocksForward();
  void SkipEmptyDataBlocksBackward();
  void SetSecondLevelIterator(InternalIteratorBase<BlockHandle>* iter);
  void InitDataBlock();

  TwoLevelIteratorState* state_;
  IteratorWrapperBase<BlockHandle> first_level_iter_;
  IteratorWrapperBase<BlockHandle> second_level_iter_;  // May be nullptr
  Status status_;
  // If second_level_iter is non-nullptr, then "data_block_handle_" holds the
  // "index_value" passed to block_function_ to create the second_level_iter.
  BlockHandle data_block_handle_;
};

TwoLevelIndexIterator::TwoLevelIndexIterator(
    TwoLevelIteratorState* state,
    InternalIteratorBase<BlockHandle>* first_level_iter)
    : state_(state), first_level_iter_(first_level_iter) {}

void TwoLevelIndexIterator::Seek(const Slice& target) {
  first_level_iter_.Seek(target);

  InitDataBlock();
  if (second_level_iter_.iter() != nullptr) {
    second_level_iter_.Seek(target);
  }
  SkipEmptyDataBlocksForward();
}

void TwoLevelIndexIterator::SeekForPrev(const Slice& target) {
  first_level_iter_.Seek(target);
  InitDataBlock();
  if (second_level_iter_.iter() != nullptr) {
    second_level_iter_.SeekForPrev(target);
  }
  if (!Valid()) {
    if (!first_level_iter_.Valid() && first_level_iter_.status().ok()) {
      first_level_iter_.SeekToLast();
      InitDataBlock();
      if (second_level_iter_.iter() != nullptr) {
        second_level_iter_.SeekForPrev(target);
      }
    }
    SkipEmptyDataBlocksBackward();
  }
}

void TwoLevelIndexIterator::SeekToFirst() {
  first_level_iter_.SeekToFirst();
  InitDataBlock();
  if (second_level_iter_.iter() != nullptr) {
    second_level_iter_.SeekToFirst();
  }
  SkipEmptyDataBlocksForward();
}

void TwoLevelIndexIterator::SeekToLast() {
  first_level_iter_.SeekToLast();
  InitDataBlock();
  if (second_level_iter_.iter() != nullptr) {
    second_level_iter_.SeekToLast();
  }
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIndexIterator::Next() {
  assert(Valid());
  second_level_iter_.Next();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIndexIterator::Prev() {
  assert(Valid());
  second_level_iter_.Prev();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIndexIterator::SkipEmptyDataBlocksForward() {
  while (second_level_iter_.iter() == nullptr ||
         (!second_level_iter_.Valid() && second_level_iter_.status().ok())) {
    // Move to next block
    if (!first_level_iter_.Valid()) {
      SetSecondLevelIterator(nullptr);
      return;
    }
    first_level_iter_.Next();
    InitDataBlock();
    if (second_level_iter_.iter() != nullptr) {
      second_level_iter_.SeekToFirst();
    }
  }
}

void TwoLevelIndexIterator::SkipEmptyDataBlocksBackward() {
  while (second_level_iter_.iter() == nullptr ||
         (!second_level_iter_.Valid() && second_level_iter_.status().ok())) {
    // Move to next block
    if (!first_level_iter_.Valid()) {
      SetSecondLevelIterator(nullptr);
      return;
    }
    first_level_iter_.Prev();
    InitDataBlock();
    if (second_level_iter_.iter() != nullptr) {
      second_level_iter_.SeekToLast();
    }
  }
}

void TwoLevelIndexIterator::SetSecondLevelIterator(
    InternalIteratorBase<BlockHandle>* iter) {
  InternalIteratorBase<BlockHandle>* old_iter = second_level_iter_.Set(iter);
  delete old_iter;
}

void TwoLevelIndexIterator::InitDataBlock() {
  if (!first_level_iter_.Valid()) {
    SetSecondLevelIterator(nullptr);
  } else {
    BlockHandle handle = first_level_iter_.value();
    if (second_level_iter_.iter() != nullptr &&
        !second_level_iter_.status().IsIncomplete() &&
        handle.offset() == data_block_handle_.offset()) {
      // second_level_iter is already constructed with this iterator, so
      // no need to change anything
    } else {
      InternalIteratorBase<BlockHandle>* iter =
          state_->NewSecondaryIterator(handle);
      data_block_handle_ = handle;
      SetSecondLevelIterator(iter);
    }
  }
}

struct IteratorCache {
  std::function<InternalIterator*(uint64_t, Arena*)> create_iterator_;
  Arena arena_;
  PinnedIteratorsManager* pinned_iters_mgr_;
  std::unordered_map<uint64_t, InternalIterator*> iterator_cache_;

  IteratorCache(
      std::function<InternalIterator*(uint64_t, Arena*)> create_iterator)
      : create_iterator_(create_iterator),
        pinned_iters_mgr_(nullptr) {}

  ~IteratorCache() {
    for (auto pair : iterator_cache_) {
      pair.second->~InternalIterator();
    }
  }

  InternalIterator* GetIterator(uint64_t sst_id) {
    auto find = iterator_cache_.find(sst_id);
    if (find != iterator_cache_.end()) {
      return find->second;
    }
    auto iter = create_iterator_(sst_id, &arena_);
    if (iter != nullptr) {
      iter->SetPinnedItersMgr(pinned_iters_mgr_);
      iterator_cache_.emplace(sst_id, iter);
    }
    return iter;
  }

  void SetPinnedItersMgr(PinnedIteratorsManager* pinned_iters_mgr) {
    pinned_iters_mgr_ = pinned_iters_mgr;
    for (auto pair : iterator_cache_) {
      pair.second->SetPinnedItersMgr(pinned_iters_mgr);
    }
  }
};

class LinkSstIterator final : public InternalIterator {
 private:
  InternalIterator* first_level_iter_;
  InternalIterator* second_level_iter_;
  Slice bound_;
  bool has_bound_;
  bool is_backword_;
  Status status_;
  const InternalKeyComparator& icomp_;
  IteratorCache iterator_cache_;

  bool InitFirstLevelIter() {
    if (!first_level_iter_->Valid()) {
      second_level_iter_ = nullptr;
      return false;
    }
    bound_ = first_level_iter_->key();
    return InitSecondLevelIter();
  }

  bool InitSecondLevelIter() {
    // Manual inline LinkSstElement::Decode
    uint64_t sst_id;
    Slice value_copy = first_level_iter_->value();
    if (!GetFixed64(&value_copy, &sst_id)) {
      status_ = Status::Corruption("Link sst invalid value");
      second_level_iter_ = nullptr;
      return false;
    }
    second_level_iter_ = iterator_cache_.GetIterator(sst_id);
    if (second_level_iter_ == nullptr) {
      status_ = Status::Corruption("Link sst depend files missing");
      return false;
    }
    return true;
  }

 public:
  LinkSstIterator(
      InternalIterator* iter,
      const InternalKeyComparator& icomp,
      const std::function<InternalIterator*(uint64_t, Arena*)>& create)
      : first_level_iter_(iter),
        second_level_iter_(nullptr),
        has_bound_(false),
        is_backword_(false),
        icomp_(icomp),
        iterator_cache_(create) {}

  virtual bool Valid() const override {
    return second_level_iter_ != nullptr && second_level_iter_->Valid();
  }
  virtual void SeekToFirst() override {
    first_level_iter_->SeekToFirst();
    if (InitFirstLevelIter()) {
      second_level_iter_->SeekToFirst();
      is_backword_ = false;
    }
  }
  virtual void SeekToLast() override {
    first_level_iter_->SeekToLast();
    if (InitFirstLevelIter()) {
      second_level_iter_->SeekToLast();
      is_backword_ = false;
    }
  }
  virtual void Seek(const Slice& target) override {
    first_level_iter_->Seek(target);
    if (InitFirstLevelIter()) {
      second_level_iter_->Seek(target);
      is_backword_ = false;
    }
  }
  virtual void SeekForPrev(const Slice& target) override {
    LinkSstIterator::Seek(target);
    if (!LinkSstIterator::Valid()) {
      LinkSstIterator::SeekToLast();
    } else if (icomp_.Compare(LinkSstIterator::key(), target) != 0) {
      LinkSstIterator::Prev();
    }
  }
  virtual void Next() override {
    if (is_backword_) {
      if (has_bound_) {
        first_level_iter_->Next();
      } else {
        first_level_iter_->SeekToFirst();
      }
      bound_ = first_level_iter_->key();
      is_backword_ = false;
    }
    second_level_iter_->Next();
    if (second_level_iter_->Valid() &&
        icomp_.Compare(second_level_iter_->key(), bound_) <= 0) {
      return;
    }
    InternalKey where;
    where.DecodeFrom(bound_);
    first_level_iter_->Next();
    if (InitFirstLevelIter()) {
      second_level_iter_->Seek(where.Encode());
    }
  }
  virtual void Prev() override {
    if (!is_backword_) {
      first_level_iter_->Prev();
      has_bound_ = first_level_iter_->Valid();
      if (has_bound_) {
        bound_ = first_level_iter_->key();
      }
      is_backword_ = true;
    }
    if (!has_bound_) {
      second_level_iter_->Prev();
      return;
    }
    second_level_iter_->Prev();
    if (second_level_iter_->Valid() &&
        icomp_.Compare(second_level_iter_->key(), bound_) > 0) {
      return;
    }
    if (InitSecondLevelIter()) {
      second_level_iter_->SeekForPrev(bound_);
      first_level_iter_->Prev();
      has_bound_ = first_level_iter_->Valid();
      if (has_bound_) {
        bound_ = first_level_iter_->key();
      }
    }
  }
  virtual Slice key() const override {
    return second_level_iter_->key();
  }
  virtual Slice value() const override {
    return second_level_iter_->value();
  }
  virtual Status status() const override {
    return status_;
  }
  virtual IteratorSource source() const override {
    return second_level_iter_->source();
  }
  virtual void SetPinnedItersMgr(
      PinnedIteratorsManager* pinned_iters_mgr) override {
    iterator_cache_.SetPinnedItersMgr(pinned_iters_mgr);
  }
  virtual bool IsKeyPinned() const override {
    return second_level_iter_ != nullptr &&
           second_level_iter_->IsKeyPinned();
  }
  virtual bool IsValuePinned() const override {
    return second_level_iter_ != nullptr &&
           second_level_iter_->IsValuePinned();
  }
};

 
class MapSstIterator final : public InternalIterator {
 private:
  InternalIterator* first_level_iter_;
  bool is_backword_;
  Status status_;
  IteratorCache iterator_cache_;
  Slice smallest_key_;
  Slice largest_key_;
  std::vector<uint64_t> link_;
  struct HeapElement {
    InternalIterator* iter;
    Slice key;
  };
  template<bool is_less>
  class HeapComparator {
   public:
    HeapComparator(const InternalKeyComparator& comparator)
        : c_(comparator) {}

    bool operator()(const HeapElement& a, const HeapElement& b) const {
      return is_less ? c_.Compare(a.key, b.key) < 0
                     : c_.Compare(a.key, b.key) > 0;
    }

    const InternalKeyComparator& icomp() const { return c_; }
   private:
    const InternalKeyComparator& c_;
  };
  typedef std::vector<HeapElement> HeapVectorType;
  union {
    // They have same layout, but we only use one of them at same time
    BinaryHeap<HeapElement, HeapComparator<0>, HeapVectorType> min_heap_;
    BinaryHeap<HeapElement, HeapComparator<1>, HeapVectorType> max_heap_;
  };

  bool InitFirstLevelIter() {
    min_heap_.clear();
    if (!first_level_iter_->Valid()) {
      return false;
    }
    // Manual inline MapSstElement::Decode
    Slice map_input = first_level_iter_->value();
    link_.clear();
    largest_key_ = first_level_iter_->key();
    uint64_t link_count;
    if (!GetLengthPrefixedSlice(&map_input, &smallest_key_) ||
        !GetVarint64(&map_input, &link_count) ||
        map_input.size() < link_count * sizeof(uint64_t)) {
      status_ = Status::Corruption("Map sst invalid value");
      return false;
    }
    link_.resize(link_count);
    for (uint64_t i = 0; i < link_count; ++i) {
      GetFixed64(&map_input, &link_[i]);
    }
    return true;
  }

  void InitSecondLevelMinHeap(const Slice& target) {
    assert(min_heap_.empty());
    for (auto sst_id : link_) {
      auto it = iterator_cache_.GetIterator(sst_id);
      if (it == nullptr) {
        status_ = Status::Corruption("Map sst depend files missing");
        min_heap_.clear();
        return;
      }
      it->Seek(target);
      if (it->Valid()) {
        min_heap_.push(HeapElement{it, it->key()});
      }
    }
    assert(!min_heap_.empty());
  }

  void InitSecondLevelMaxHeap(const Slice& target) {
    assert(max_heap_.empty());
    for (auto sst_id : link_) {
      auto it = iterator_cache_.GetIterator(sst_id);
      if (it == nullptr) {
        status_ = Status::Corruption("Map sst depend files missing");
        max_heap_.clear();
        return;
      }
      it->SeekForPrev(target);
      if (it->Valid()) {
        max_heap_.push(HeapElement{it, it->key()});
      }
    }
    assert(!max_heap_.empty());
  }

 public:
  MapSstIterator(
      InternalIterator* iter,
      const InternalKeyComparator& icomp,
      const std::function<InternalIterator*(uint64_t, Arena*)>& create)
      : first_level_iter_(iter),
        is_backword_(false),
        iterator_cache_(create),
        min_heap_(icomp) {}

  ~MapSstIterator() {
    min_heap_.~BinaryHeap();
  }

  virtual bool Valid() const override {
    return !min_heap_.empty();
  }
  virtual void SeekToFirst() override {
    first_level_iter_->SeekToFirst();
    if (InitFirstLevelIter()) {
      InitSecondLevelMinHeap(smallest_key_);
      is_backword_ = false;
    }
  }
  virtual void SeekToLast() override {
    first_level_iter_->SeekToLast();
    if (InitFirstLevelIter()) {
      InitSecondLevelMaxHeap(largest_key_);
      is_backword_ = false;
    }
  }
  virtual void Seek(const Slice& target) override {
    first_level_iter_->Seek(target);
    if (!InitFirstLevelIter()) {
      return;
    }
    auto& icomp = min_heap_.comparator().icomp();
    Slice seek_target = target;
    if (icomp.Compare(target, smallest_key_) < 0) {
      seek_target = smallest_key_;
    }
    InitSecondLevelMinHeap(seek_target);
    is_backword_ = false;
  }
  virtual void SeekForPrev(const Slice& target) override {
    first_level_iter_->Seek(target);
    if (!InitFirstLevelIter()) {
      MapSstIterator::SeekToLast();
      return;
    }
    auto& icomp = min_heap_.comparator().icomp();
    Slice seek_target = target;
    if (icomp.Compare(target, smallest_key_) < 0) {
      first_level_iter_->Prev();
      if (!InitFirstLevelIter()) {
        return;
      }
      seek_target = largest_key_;
    }
    InitSecondLevelMaxHeap(seek_target);
    is_backword_ = true;
  }
  virtual void Next() override {
    if (is_backword_) {
      InternalKey where;
      where.DecodeFrom(max_heap_.top().key);
      max_heap_.clear();
      InitSecondLevelMinHeap(where.Encode());
      is_backword_ = false;
    }
    auto current = min_heap_.top();
    current.iter->Next();
    if (current.iter->Valid()) {
      current.key = current.iter->key();
      min_heap_.replace_top(current);
    } else {
      min_heap_.pop();
    }
    auto& icomp = min_heap_.comparator().icomp();
    if (!min_heap_.empty() &&
        icomp.Compare(min_heap_.top().key, largest_key_) <= 0) {
      return;
    }
    first_level_iter_->Next();
    if (InitFirstLevelIter()) {
      InitSecondLevelMinHeap(smallest_key_);
    }
  }
  virtual void Prev() override {
    if (!is_backword_) {
      InternalKey where;
      where.DecodeFrom(min_heap_.top().key);
      min_heap_.clear();
      InitSecondLevelMaxHeap(where.Encode());
      is_backword_ = true;
    }
    auto current = max_heap_.top();
    current.iter->Prev();
    if (current.iter->Valid()) {
      current.key = current.iter->key();
      max_heap_.replace_top(current);
    } else {
      max_heap_.pop();
    }
    auto& icomp = min_heap_.comparator().icomp();
    if (!max_heap_.empty() &&
        icomp.Compare(max_heap_.top().key, smallest_key_) >= 0) {
      return;
    }
    first_level_iter_->Prev();
    if (InitFirstLevelIter()) {
      InitSecondLevelMaxHeap(largest_key_);
    }
  }
  virtual Slice key() const override {
    return min_heap_.top().key;
  }
  virtual Slice value() const override {
    return min_heap_.top().iter->value();
  }
  virtual Status status() const override {
    return status_;
  }
  virtual IteratorSource source() const override {
    return min_heap_.top().iter->source();
  }
  virtual void SetPinnedItersMgr(
      PinnedIteratorsManager* pinned_iters_mgr) override {
    iterator_cache_.SetPinnedItersMgr(pinned_iters_mgr);
  }
  virtual bool IsKeyPinned() const override {
    return !min_heap_.empty() &&
           min_heap_.top().iter->IsKeyPinned();
  }
  virtual bool IsValuePinned() const override {
    return !min_heap_.empty() &&
           min_heap_.top().iter->IsValuePinned();
  }
};

template<class IteratorType>
InternalIterator* NewVarietySstIterator(
    InternalIterator* link_sst_iter,
    const InternalKeyComparator& icomp,
    const std::function<InternalIterator*(uint64_t, Arena*)>& create_iter,
    Arena* arena) {
  if (arena == nullptr) {
    return new IteratorType(link_sst_iter, icomp, create_iter);
  } else {
    void* buffer = arena->AllocateAligned(sizeof(IteratorType));
    return new(buffer) IteratorType(link_sst_iter, icomp, create_iter);
  }
}

}  // namespace

InternalIteratorBase<BlockHandle>* NewTwoLevelIterator(
    TwoLevelIteratorState* state,
    InternalIteratorBase<BlockHandle>* first_level_iter) {
  return new TwoLevelIndexIterator(state, first_level_iter);
}

InternalIterator* NewLinkSstIterator(
    InternalIterator* link_sst_iter,
    const InternalKeyComparator& icomp,
    const std::function<InternalIterator*(uint64_t, Arena*)>& create_iter,
    Arena* arena) {
  return NewVarietySstIterator<LinkSstIterator>(link_sst_iter, icomp,
                                                create_iter, arena);
}


InternalIterator* NewMapSstIterator(
    InternalIterator* link_sst_iter,
    const InternalKeyComparator& icomp,
    const std::function<InternalIterator*(uint64_t, Arena*)>& create_iter,
    Arena* arena) {
  return NewVarietySstIterator<MapSstIterator>(link_sst_iter, icomp,
                                               create_iter, arena);
}

}  // namespace rocksdb
