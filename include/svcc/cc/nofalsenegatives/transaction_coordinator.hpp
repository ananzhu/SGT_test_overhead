//
// No False Negatives Database - A prototype database to test concurrency control that scales to many cores.
// Copyright (C) 2019 Dominik Durner <dominik.durner@tum.de>
//
// This file is part of No False Negatives Database.
//
// No False Negatives Database is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// No False Negatives Database is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with No False Negatives Database.  If not, see <http://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once
#include "common/chunk_allocator.hpp"
#include "common/epoch_manager.hpp"
#include "ds/atomic_singly_linked_list.hpp"
#include "ds/atomic_unordered_map.hpp"
#include "ds/atomic_unordered_set.hpp"
#include "svcc/cc/nofalsenegatives/serialization_graph.hpp"
#include "svcc/cc/nofalsenegatives/transaction_information.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <thread>
#include <stdint.h>

#ifndef NDEBUG
#define verify(expression) assert(expression)
#else
#define verify(expression) ((void)(expression))
#endif

namespace nofalsenegatives {
namespace transaction {

using namespace nofalsenegatives;
using namespace nofalsenegatives::transaction;

/*
 *
 * The Transaction Coordinator does coordinate transaction s.t. it denies
 * transactions that would result in a conflict. It is modular s.t. the
 * transaction coordinator can be used by multiple conflict resolution
 * strategies.
 *
 */
template <typename Allocator>
class TransactionCoordinator {
 public:
  using Alloc = Allocator;

 private:
  serial::SerializationGraph sg_;
  Allocator* alloc_;
  atom::EpochManagerBase<Allocator>* emb_;
  tbb::spin_mutex mut;

  static thread_local std::atomic<uint64_t> transaction_counter_;
  static thread_local std::unordered_set<uint64_t> not_alive_;
  static thread_local uint8_t current_core_;
  static thread_local std::unordered_set<uint64_t> abort_transaction_;
  static thread_local std::list<TransactionInformationBase<Allocator>*>* atom_info_;
  static thread_local atom::EpochGuard<atom::EpochManagerBase<Allocator>, atom::EpochManager<Allocator>>* eg_;

 public:
  TransactionCoordinator(Allocator* alloc, atom::EpochManagerBase<Allocator>* emb, bool online = false)
      : sg_(alloc, emb, online), alloc_(alloc), emb_(emb), mut(){};

  /* The highest bit is used to determine read or write accesses the lower 63 for the actual transaction id */
  /* Returns the encoded bitstring for a given transaction and there action */
  inline static constexpr uint64_t access(const uint64_t transaction, const bool rw) {
    return rw ? 0x8000000000000000 | transaction : 0x7FFFFFFFFFFFFFFF & transaction;
  }

  /* Returns the transaction and the used transaction given an encoded bitstring */
  inline static constexpr std::tuple<uint64_t, bool> find(const uint64_t encoded_id) {
    return std::make_tuple(0x7FFFFFFFFFFFFFFF & encoded_id, encoded_id >> 63);
  }

  template <typename Value,
            template <typename>
            class ValueVector,
            template <typename>
            class Vector,
            template <typename>
            class List>
  bool readValue(Value& readValue,
                 ValueVector<Value>& column,
                 Vector<uint64_t>& lsn_column,
                 Vector<List<uint64_t>*>& rw_table,
                 Vector<uint64_t>& locked,
                 uint64_t offset,
                 uint64_t transaction) {
    /*
     * transaction numbering enough since multiple writes would automatically come to an error if someone is in
     * between cause he is either before or after me.
     */

    verify(transaction > 0);

    auto not_alive = not_alive_.find(transaction);
    if (not_alive != not_alive_.end()) {
      return false;
    }

    if (sg_.needsAbort(transaction)) {
      this->abort(transaction);
      return false;
    }

    uint64_t lockInfo = locked[offset];
    while(__sync_bool_compare_and_swap(&lockInfo, 0, 1)) {;}
    // uint64_t info = access(transaction, false);
    // verify(info > 0);

    // uint64_t prv = rw_table[offset]->push_front(info);
    // if (prv > 0) {
    //   /*
    //    * expected_id is fine for coluom check since if the prv transaction is not yet finished in the
    //    * rw_table[offset]
    //    * vector the values are initialized with 0. Since the transaction ids must be greater 0, the following
    //    * for loop is only evaluated positive iff the transaction id was set beforehand and the vector operation is
    //    * already finished.
    //    */
    //   for (uint32_t i = 0; lsn_column[offset] != prv; i++) {
    //     if (i >= 10000)
    //       std::this_thread::yield();
    //   }
    // }
    

    // bool cyclic = false;

    // auto it = rw_table[offset]->begin();
    // for (; it != rw_table[offset]->end(); ++it) {
    //   if (it.getId() < prv) {
    //     if (std::get<1>(find(*it)) && !sg_.insert_and_check(std::get<0>(find(*it)), false)) {
    //       cyclic = true;
    //     }
    //   }
    // }

#ifdef SGLOGGER
    sg_.log(common::LogInfo{transaction, prv, reinterpret_cast<uintptr_t>(&rw_table), offset, 'r'});
#endif

    // if (cyclic) {
    //   rw_table[offset]->erase(prv);
    //   lsn_column.atomic_replace(offset, prv + 1);
    //   this->abort(transaction);
    //   return false;
    // }

    readValue = column[offset];

    // lsn_column.atomic_replace(offset, prv + 1);

    // auto rti = alloc_->template allocate<ReadTransactionInformation<Vector, List, Allocator>>(1);
    // atom_info_->emplace_front(
    //     new (rti) ReadTransactionInformation<Vector, List, Allocator>(rw_table, locked, prv, offset, transaction));

    __sync_bool_compare_and_swap(&lockInfo, 1, 0);
    
    return true;
  }

  template <template <typename> class Vector, template <typename> class List>
  uint64_t read(Vector<uint64_t>& lsn_column,
                Vector<List<uint64_t>*>& rw_table,
                Vector<uint64_t>& locked,
                uint64_t offset,
                uint64_t transaction) {
    /*
     * transaction numbering enough since multiple writes would automatically come to an error if someone is in
     * between cause he is either before or after me.
     */

    verify(transaction > 0);

    auto not_alive = not_alive_.find(transaction);
    if (not_alive != not_alive_.end()) {
      return false;
    }

    if (sg_.needsAbort(transaction)) {
      this->abort(transaction);
      return false;
    }

    uint64_t lockInfo = locked[offset];
    while(__sync_bool_compare_and_swap(&lockInfo, 0, 1)) {;}
    
    //std::lock_guard<tbb::spin_mutex> lock(mut);

    // uint64_t info = access(transaction, false);
    // verify(info > 0);

    // uint64_t prv = rw_table[offset]->push_front(info);
    // if (prv > 0) {
    //   /*
    //    * expected_id is fine for coluom check since if the prv transaction is not yet finished in the
    //    * rw_table[offset]
    //    * vector the values are initialized with 0. Since the transaction ids must be greater 0, the following
    //    * for loop is only evaluated positive iff the transaction id was set beforehand and the vector operation is
    //    * already finished.
    //    */
    //   for (uint32_t i = 0; lsn_column[offset] != prv; i++) {
    //     if (i >= 10000)
    //       std::this_thread::yield();
    //   }
    // }

    // bool cyclic = false;
    // auto it = rw_table[offset]->begin();

    // for (; it != rw_table[offset]->end(); ++it) {
    //   if (it.getId() < prv) {
    //     if (std::get<1>(find(*it)) && !sg_.insert_and_check(std::get<0>(find(*it)), false)) {
    //       cyclic = true;
    //     }
    //   }
    // }

#ifdef SGLOGGER
    sg_.log(common::LogInfo{transaction, prv, reinterpret_cast<uintptr_t>(&rw_table), offset, 'r'});
#endif

    // if (cyclic) {
    //   rw_table[offset]->erase(prv);
    //   lsn_column.atomic_replace(offset, prv + 1);
    //   this->abort(transaction);
    //   return 0;
    // }

    __sync_bool_compare_and_swap(&lockInfo, 1, 0);

    return true;
  }

  template <template <typename> class Vector, template <typename> class List>
  void readUndo(uint64_t prv,
                Vector<uint64_t>& lsn_column,
                Vector<List<uint64_t>*>& rw_table,
                Vector<uint64_t>& locked,
                uint64_t offset,
                uint64_t transaction) {
    lsn_column.atomic_replace(offset, prv);

    auto rti = alloc_->template allocate<ReadTransactionInformation<Vector, List, Allocator>>(1);
    atom_info_->emplace_front(
        new (rti) ReadTransactionInformation<Vector, List, Allocator>(rw_table, locked, prv - 1, offset, transaction));
  }

  template <typename Value,
            template <typename>
            class ValueVector,
            template <typename>
            class Vector,
            template <typename>
            class List,
            bool abort = false>
  bool writeValue(Value& writeValue,
                  ValueVector<Value>& column,
                  Vector<uint64_t>& lsn_column,
                  Vector<List<uint64_t>*>& rw_table,
                  Vector<uint64_t>& locked,
                  uint64_t offset,
                  uint64_t transaction) {
    /*
     * transaction numbering enough since multiple writes would automatically come to an error if someone is in
     * between cause he is either before or after me.
     */

  begin_write:
    verify(transaction > 0);

    auto not_alive = not_alive_.find(transaction);
    if (!abort && not_alive != not_alive_.end()) {
      return false;
    }

    if (!abort && sg_.needsAbort(transaction)) {
      this->abort(transaction);
      return false;
    }
    //tbb::spin_mutex wrt_mut;
    //std::lock_guard<tbb::spin_mutex> lock(wrt_mut);
    uint64_t lockInfo = locked[offset];
    while(__sync_bool_compare_and_swap(&lockInfo, 0, 1)) {;}

    // uint64_t info = access(transaction, true);
    // verify(info > 0);

    // uint64_t prv = rw_table[offset]->push_front(info);
    // if (prv > 0) {
    //   /*
    //    * expected_id is fine for coluom check since if the prv transaction is not yet finished in the
    //    * rw_table[offset]
    //    * vector the values are initialized with 0. Since the transaction ids must be greater 0, the following
    //    * for loop is only evaluated positive iff the transaction id was set beforehand and the vector operation is
    //    * already finished.
    //    */
    //   for (auto i = 0; lsn_column[offset] != prv; i++) {
    //     if (i >= 10000)
    //       std::this_thread::yield();
    //   }
    // }

    // We need to delay w,w conflicts to be able to serialize the graph
//     auto it_wait = rw_table[offset]->begin();
//     auto it_end = rw_table[offset]->end();
//     verify(it_wait != rw_table[offset]->end());
//     bool cyclic = false, wait = false;

//     while (!abort && it_wait != it_end) {
//       if (it_wait.getId() < prv && std::get<1>(find(*it_wait)) && std::get<0>(find(*it_wait)) != transaction) {
//         if (!sg_.isCommited(std::get<0>(find(*it_wait)))) {
//           // ww-edge hence cascading abort necessary
//           if (!sg_.insert_and_check(std::get<0>(find(*it_wait)), false)) {
//             cyclic = true;
//           }
//           wait = true;
//         }
//       }
//       ++it_wait;
//     }

//     if (!abort) {
//       if (cyclic) {
//       cyclic_write:
//         rw_table[offset]->erase(prv);
//         lsn_column.atomic_replace(offset, prv + 1);
//         this->abort(transaction);

//         // std::cout << "abort(" << transaction << ") | rw" << std::endl;
//         return false;
//       }

//       if (wait) {
//         rw_table[offset]->erase(prv);
//         lsn_column.atomic_replace(offset, prv + 1);
//         goto begin_write;
//       }

//       auto it = rw_table[offset]->begin();
//       auto end = rw_table[offset]->end();
//       verify(rw_table[offset]->size() > 0);
//       verify(it != rw_table[offset]->end());

//       while (it != end) {
//         if (it.getId() < prv) {
//           // if it is read access this a r-w edge, hence no cascading abort necessary
//           if (!sg_.insert_and_check(std::get<0>(find(*it)), !std::get<1>(find(*it)))) {
//             cyclic = true;
//           }
//         }
//         ++it;
//       }
// #ifdef SGLOGGER
//       if (!(wait && !cyclic)) {
//         char c = 'w';
//         if (cyclic) {
//           c = 'e';
//         }
//         sg_.log(common::LogInfo{transaction, val, reinterpret_cast<uintptr_t>(&rw_table), offset, c});
//       }
// #endif

//       if (cyclic) {
//         goto cyclic_write;
//       }
//     }

    Value old = column.replace(offset, writeValue);

    __sync_bool_compare_and_swap(&lockInfo, 1, 0);

    // lsn_column.atomic_replace(offset, prv + 1);

    // auto wti = alloc_->template allocate<WriteTransactionInformation<Value, ValueVector, Vector, List, Allocator>>(1);
    // atom_info_->emplace_front(new (wti) WriteTransactionInformation<Value, ValueVector, Vector, List, Allocator>(
    //     writeValue, old, column, lsn_column, rw_table, locked, prv, offset, transaction, abort));
    return true;
  }

  /*
   * Abort: Needs to redo all write operations of the aborted transaction and needs to abort all transactions
   * read or write on data used in this transaction -> cascading aborts
   */
  // template <template <typename> class Vector>
  void abort(uint64_t transaction) {
    /*
     * Idea: Do undo of the operations in the undo log. Then check which transaction where in between the first
     * transaction of the to aborting transaction and the last undo of the abort process. Following, abort the
     * transaction found in between.
     */
    not_alive_.insert(transaction);

    for (auto t : *atom_info_) {
      if (!t->isWriteTransaction() || t->isAbort())
        continue;
      t->writeValue(*this);
    }

    sg_.abort(abort_transaction_);

    /*
     * As long as uncomment expect mem leakage!
     */
    for (auto t : *atom_info_) {
      t->deleteFromRWTable();
      t->deallocate(alloc_);
    }
    atom_info_->~list();
    eg_->~EpochGuard();
  }

  /* Commit: Needs to wait for the commit of all transactions in the read / write set of this transaction
   * to avoid w_1(x) r_2(x) w_2(x) c_2 a_1 and therefore inconsistent data in the database
   */
  bool commit(uint64_t transaction, std::unordered_set<uint64_t>& oset) {
    /*
     * Idea: Check if there are nodes in the SGT reachable from my node within 1 hop that haven't committed yet. If
     * not
     * all more hop nodes need to have committed already by induction. Hence, it is save to also commit this
     * transaction.
     * Otherwise, wait and yield for the other transactions having committed or aborted.
     */

    bool all_pending_transactions_commited = true;
    while (!all_pending_transactions_commited) {
      auto not_alive = not_alive_.find(transaction);
      if (not_alive != not_alive_.end()) {
        not_alive_.erase(transaction);
        oset = abort_transaction_;
        return false;
      }

      if (sg_.needsAbort(transaction)) {
        this->abort(transaction);
        not_alive_.erase(transaction);
        oset = abort_transaction_;
        return false;
      }

      all_pending_transactions_commited = sg_.checkCommited();

      if (all_pending_transactions_commited) {
        for (auto t : *atom_info_) {
          t->deleteFromRWTable();
          t->deallocate(alloc_);
        }
      }
    }

    // for (auto t : *atom_info_){
    //   t->deleteFromRWTable();
    //   t->deallocate(alloc_);
    // }

    // atom_info_->~list();
    // eg_->~EpochGuard();

    return true;
  }

  inline uint64_t start() {
    auto tc = transaction_counter_.fetch_add(1) + 1;
    if (current_core_ == std::numeric_limits<uint8_t>::max()) {
      current_core_ = sched_getcpu();
    }
    uint64_t core = current_core_;
    tc &= 0x00FFFFFFFFFFFFFF;
    tc |= (core << 56);

    if (atom_info_ == nullptr) {
      atom_info_ = new std::list<TransactionInformationBase<Allocator>*>();
    } else {
      atom_info_ = new (atom_info_) std::list<TransactionInformationBase<Allocator>*>();
    }
    abort_transaction_.clear();

    if (eg_ == nullptr) {
      eg_ = new atom::EpochGuard<atom::EpochManagerBase<Allocator>, atom::EpochManager<Allocator>>{emb_};
    } else {
      eg_ = new (eg_) atom::EpochGuard<atom::EpochManagerBase<Allocator>, atom::EpochManager<Allocator>>{emb_};
    }
    return sg_.createNode();
  }

  inline void bot(uint64_t transaction) {}
};
};  // namespace transaction
};  // namespace nofalsenegatives
