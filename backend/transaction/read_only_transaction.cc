//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "backend/transaction/read_only_transaction.h"

#include <memory>

#include "absl/random/random.h"
#include "zetasql/base/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key_set.h"
#include "backend/locking/manager.h"
#include "backend/storage/in_memory_iterator.h"
#include "backend/storage/storage.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_util.h"
#include "backend/transaction/row_cursor.h"
#include "common/clock.h"
#include "zetasql/base/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

absl::Duration kMaxStaleReadDuration = absl::Hours(1);

}  // namespace

ReadOnlyTransaction::ReadOnlyTransaction(
    const ReadOnlyOptions& options, TransactionID transaction_id, Clock* clock,
    Storage* storage, LockManager* lock_manager,
    const VersionedCatalog* const versioned_catalog)
    : options_(options),
      id_(transaction_id),
      clock_(clock),
      base_storage_(storage),
      lock_manager_(lock_manager) {
  lock_handle_ = lock_manager_->CreateHandle(transaction_id, /*priority=*/1);
  read_timestamp_ = PickReadTimestamp();
  // Wait for any concurrent schema change or read-write transactions to commit
  // before accessing any database state.
  // TODO : Remove the wait from the constructor.
  lock_handle_->WaitForSafeRead(read_timestamp_);
  schema_ = versioned_catalog->GetSchema(read_timestamp_);
}

zetasql_base::Status ReadOnlyTransaction::Read(const ReadArg& read_arg,
                                       std::unique_ptr<RowCursor>* cursor) {
  absl::MutexLock lock(&mu_);
  if (clock_->Now() - read_timestamp_ >= kMaxStaleReadDuration) {
    return error::ReadTimestampPastVersionGCLimit(read_timestamp_);
  }

  const Table* table;
  std::vector<const Column*> columns;
  std::vector<ColumnID> column_ids;
  ZETASQL_RETURN_IF_ERROR(
      ExtractTableAndColumnsFromReadArg(read_arg, schema_, &table, &columns));
  column_ids.reserve(columns.size());
  for (const Column* column : columns) {
    column_ids.push_back(column->id());
  }

  std::vector<std::unique_ptr<StorageIterator>> iterators;
  std::vector<KeyRange> key_ranges;
  CanonicalizeKeySetForTable(read_arg.key_set, table, &key_ranges);
  for (const auto& key_range : key_ranges) {
    std::unique_ptr<StorageIterator> itr;
    ZETASQL_RETURN_IF_ERROR(base_storage_->Read(read_timestamp_, table->id(), key_range,
                                        column_ids, &itr));
    iterators.push_back(std::move(itr));
  }
  *cursor = absl::make_unique<StorageIteratorRowCursor>(std::move(iterators),
                                                        std::move(columns));
  return zetasql_base::OkStatus();
}

absl::Time ReadOnlyTransaction::PickReadTimestamp() {
  auto get_random_stale_timestamp =
      [this](absl::Time min_timestamp) -> absl::Time {
    // Any reads performed on or before last_commit_timestamp are guaranteed to
    // see a consistent snapshots of all the commits that have already finished.
    // Thus, picked read timestamp need not be older than last_commit_timestamp.
    absl::Time last_commit_timestamp = lock_manager_->LastCommitTimestamp();
    if (min_timestamp < last_commit_timestamp) {
      min_timestamp = last_commit_timestamp;
    }
    absl::BitGen gen;
    int64_t random_staleness = absl::Uniform<int64_t>(
        gen, 0, absl::ToInt64Microseconds(clock_->Now() - min_timestamp));
    return clock_->Now() - absl::Microseconds(random_staleness);
  };
  switch (options_.bound) {
    case TimestampBound::kStrongRead: {
      read_timestamp_ = clock_->Now();
      break;
    }
    case TimestampBound::kExactTimestamp: {
      read_timestamp_ = options_.timestamp;
      break;
    }
    case TimestampBound::kExactStaleness: {
      read_timestamp_ = clock_->Now() - options_.staleness;
      break;
    }
    case TimestampBound::kMinTimestamp: {
      // Randomly choose staleness to mimic production behavior of reading
      // from potentially lagging replicas.
      read_timestamp_ = get_random_stale_timestamp(options_.timestamp);
      break;
    }
    case TimestampBound::kMaxStaleness: {
      // Randomly choose staleness to mimick production behavior of reading
      // from potentially lagging replicas.
      read_timestamp_ =
          get_random_stale_timestamp(clock_->Now() - options_.staleness);
      break;
    }
  }
  return read_timestamp_;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
