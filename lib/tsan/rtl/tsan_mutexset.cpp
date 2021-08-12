//===-- tsan_mutexset.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
//===----------------------------------------------------------------------===//
#include "tsan_mutexset.h"
#include "tsan_rtl.h"

namespace __tsan {

MutexSet::MutexSet() {
}

void MutexSet::Add(u64 id, bool write, u64 epoch) {
  // Look up existing mutex with the same id.
  for (uptr i = 0; i < size_; i++) {
    if (descs_[i].id == id) {
      descs_[i].count++;
      descs_[i].epoch = epoch;
      return;
    }
  }
  // On overflow, find the oldest mutex and drop it.
  if (size_ == kMaxSize) {
    u64 minepoch = (u64)-1;
    u64 mini = (u64)-1;
    for (uptr i = 0; i < size_; i++) {
      if (descs_[i].epoch < minepoch) {
        minepoch = descs_[i].epoch;
        mini = i;
      }
    }
    RemovePos(mini);
    CHECK_EQ(size_, kMaxSize - 1);
  }
  // Add new mutex descriptor.
  descs_[size_].addr = 0;
  descs_[size_].stack_id = kInvalidStackID;
  descs_[size_].id = id;
  descs_[size_].write = write;
  descs_[size_].epoch = epoch;
  descs_[size_].seq = seq_++;
  descs_[size_].count = 1;
  size_++;
}

void MutexSet::Del(u64 id, bool write) {
  for (uptr i = 0; i < size_; i++) {
    if (descs_[i].id == id) {
      if (--descs_[i].count == 0)
        RemovePos(i);
      return;
    }
  }
}

void MutexSet::Remove(u64 id) {
  for (uptr i = 0; i < size_; i++) {
    if (descs_[i].id == id) {
      RemovePos(i);
      return;
    }
  }
}

void MutexSet::AddAddr(uptr addr, StackID stack_id, bool write) {
  // Look up existing mutex with the same id.
  for (uptr i = 0; i < size_; i++) {
    if (descs_[i].addr == addr) {
      descs_[i].count++;
      descs_[i].seq = seq_++;
      return;
    }
  }
  // On overflow, find the oldest mutex and drop it.
  if (size_ == kMaxSize) {
    uptr min = 0;
    for (uptr i = 0; i < size_; i++) {
      if (descs_[i].seq < descs_[min].seq)
        min = i;
    }
    RemovePos(min);
    CHECK_EQ(size_, kMaxSize - 1);
  }
  // Add new mutex descriptor.
  descs_[size_].addr = addr;
  descs_[size_].stack_id = stack_id;
  descs_[size_].id = 0;
  descs_[size_].write = write;
  descs_[size_].epoch = 0;
  descs_[size_].seq = seq_++;
  descs_[size_].count = 1;
  size_++;
}

void MutexSet::DelAddr(uptr addr, bool destroy) {
  for (uptr i = 0; i < size_; i++) {
    if (descs_[i].addr == addr) {
      if (destroy || --descs_[i].count == 0)
        RemovePos(i);
      return;
    }
  }
}

void MutexSet::RemovePos(uptr i) {
  CHECK_LT(i, size_);
  descs_[i] = descs_[size_ - 1];
  size_--;
}

uptr MutexSet::Size() const {
  return size_;
}

MutexSet::Desc MutexSet::Get(uptr i) const {
  CHECK_LT(i, size_);
  return descs_[i];
}

}  // namespace __tsan
