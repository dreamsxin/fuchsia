// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BLOBFS_TRANSACTION_MANAGER_H_
#define BLOBFS_TRANSACTION_MANAGER_H_

#ifndef __Fuchsia__
#error Fuchsia-only Header
#endif

#include <blobfs/allocator.h>
#include <blobfs/blob.h>
#include <blobfs/metrics.h>
#include <fbl/unique_ptr.h>
#include <fs/journal/journal.h>
#include <fs/transaction/block-transaction.h>
#include <fs/vnode.h>

namespace blobfs {

class WritebackWork;

// EnqueueType describes the classes of data which may be enqueued to the
// underlying storage medium.
enum class EnqueueType {
  kJournal,
  kData,
};

// An interface which controls access to the underlying storage.
class TransactionManager : public fs::TransactionHandler, public SpaceManager {
 public:
  virtual ~TransactionManager() = default;
  virtual BlobfsMetrics& Metrics() = 0;

  // Returns the capacity of the writeback buffer in blocks.
  virtual size_t WritebackCapacity() const = 0;

  virtual fs::Journal* journal() = 0;
};

}  // namespace blobfs

#endif  // BLOBFS_TRANSACTION_MANAGER_H_
