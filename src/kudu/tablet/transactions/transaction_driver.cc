// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/tablet/transactions/transaction_driver.h"

#include <mutex>

#include "kudu/consensus/consensus.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/rpc/result_tracker.h"
#include "kudu/tablet/tablet_peer.h"
#include "kudu/tablet/transactions/transaction_tracker.h"
#include "kudu/util/debug-util.h"
#include "kudu/util/debug/trace_event.h"
#include "kudu/util/logging.h"
#include "kudu/util/threadpool.h"
#include "kudu/util/trace.h"

namespace kudu {
namespace tablet {

using consensus::CommitMsg;
using consensus::Consensus;
using consensus::ConsensusRound;
using consensus::ReplicateMsg;
using consensus::CommitMsg;
using consensus::DriverType;
using log::Log;
using rpc::RequestIdPB;
using rpc::ResultTracker;
using std::shared_ptr;

static const char* kTimestampFieldName = "timestamp";

class FollowerTransactionCompletionCallback : public TransactionCompletionCallback {
 public:
  FollowerTransactionCompletionCallback(const RequestIdPB& request_id,
                                        const google::protobuf::Message* response,
                                        const scoped_refptr<ResultTracker>& result_tracker)
      : request_id_(request_id),
        response_(response),
        result_tracker_(result_tracker) {}

  virtual void TransactionCompleted() {
    if (status_.ok()) {
      result_tracker_->RecordCompletionAndRespond(request_id_, response_);
    } else {
      // For now we always respond with TOO_BUSY, meaning the client will retry (even if
      // this is an unretryable failure), that works as the client-driven version of this
      // transaction will get the right error.
      result_tracker_->FailAndRespond(request_id_,
                                      rpc::ErrorStatusPB::ERROR_SERVER_TOO_BUSY,
                                      status_);
    }
  }

  virtual ~FollowerTransactionCompletionCallback() {}

 private:
  const RequestIdPB& request_id_;
  const google::protobuf::Message* response_;
  scoped_refptr<ResultTracker> result_tracker_;
};


////////////////////////////////////////////////////////////
// TransactionDriver
////////////////////////////////////////////////////////////

TransactionDriver::TransactionDriver(TransactionTracker *txn_tracker,
                                     Consensus* consensus,
                                     Log* log,
                                     ThreadPool* prepare_pool,
                                     ThreadPool* apply_pool,
                                     TransactionOrderVerifier* order_verifier)
    : txn_tracker_(txn_tracker),
      consensus_(consensus),
      log_(log),
      prepare_pool_(prepare_pool),
      apply_pool_(apply_pool),
      order_verifier_(order_verifier),
      trace_(new Trace()),
      start_time_(MonoTime::Now()),
      replication_state_(NOT_REPLICATING),
      prepare_state_(NOT_PREPARED) {
  if (Trace::CurrentTrace()) {
    Trace::CurrentTrace()->AddChildTrace("txn", trace_.get());
  }
}

Status TransactionDriver::Init(gscoped_ptr<Transaction> transaction,
                               DriverType type) {
  transaction_ = std::move(transaction);

  if (type == consensus::REPLICA) {
    std::lock_guard<simple_spinlock> lock(opid_lock_);
    op_id_copy_ = transaction_->state()->op_id();
    DCHECK(op_id_copy_.IsInitialized());
    replication_state_ = REPLICATING;
    replication_start_time_ = MonoTime::Now();
    if (state()->are_results_tracked()) {
      // If this is a follower transaction, make sure to set the transaction completion callback
      // before the transaction has a chance to fail.
      const rpc::RequestIdPB& request_id = state()->request_id();
      const google::protobuf::Message* response = state()->response();
      gscoped_ptr<TransactionCompletionCallback> callback(
          new FollowerTransactionCompletionCallback(request_id,
                                                    response,
                                                    state()->result_tracker()));
      mutable_state()->set_completion_callback(callback.Pass());
    }
  } else {
    DCHECK_EQ(type, consensus::LEADER);
    gscoped_ptr<ReplicateMsg> replicate_msg;
    transaction_->NewReplicateMsg(&replicate_msg);
    if (consensus_) { // sometimes NULL in tests
      // Unretained is required to avoid a refcount cycle.
      mutable_state()->set_consensus_round(
        consensus_->NewRound(std::move(replicate_msg),
                             Bind(&TransactionDriver::ReplicationFinished, Unretained(this))));
    }
  }

  RETURN_NOT_OK(txn_tracker_->Add(this));

  return Status::OK();
}

consensus::OpId TransactionDriver::GetOpId() {
  std::lock_guard<simple_spinlock> lock(opid_lock_);
  return op_id_copy_;
}

const TransactionState* TransactionDriver::state() const {
  return transaction_ != nullptr ? transaction_->state() : nullptr;
}

TransactionState* TransactionDriver::mutable_state() {
  return transaction_ != nullptr ? transaction_->state() : nullptr;
}

Transaction::TransactionType TransactionDriver::tx_type() const {
  return transaction_->tx_type();
}

string TransactionDriver::ToString() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  return ToStringUnlocked();
}

string TransactionDriver::ToStringUnlocked() const {
  string ret = StateString(replication_state_, prepare_state_);
  if (transaction_ != nullptr) {
    ret += " " + transaction_->ToString();
  } else {
    ret += "[unknown txn]";
  }
  return ret;
}


Status TransactionDriver::ExecuteAsync() {
  VLOG_WITH_PREFIX(4) << "ExecuteAsync()";
  TRACE_EVENT_FLOW_BEGIN0("txn", "ExecuteAsync", this);
  ADOPT_TRACE(trace());

  Status s;
  if (replication_state_ == NOT_REPLICATING) {
    // We're a leader transaction. Before submitting, check that we are the leader and
    // determine the current term.
    s = consensus_->CheckLeadershipAndBindTerm(mutable_state()->consensus_round());
  }

  if (s.ok()) {
    s = prepare_pool_->SubmitClosure(
      Bind(&TransactionDriver::PrepareAndStartTask, Unretained(this)));
  }

  if (!s.ok()) {
    HandleFailure(s);
  }

  // TODO: make this return void
  return Status::OK();
}

void TransactionDriver::PrepareAndStartTask() {
  TRACE_EVENT_FLOW_END0("txn", "PrepareAndStartTask", this);
  Status prepare_status = PrepareAndStart();
  if (PREDICT_FALSE(!prepare_status.ok())) {
    HandleFailure(prepare_status);
  }
}

void TransactionDriver::RegisterFollowerTransactionOnResultTracker() {
  // If this is a transaction being executed by a follower and its result is being
  // tracked, make sure that we're the driver of the transaction.
  if (!state()->are_results_tracked()) return;

  ResultTracker::RpcState rpc_state = state()->result_tracker()->TrackRpcOrChangeDriver(
      state()->request_id());
  switch (rpc_state) {
    case ResultTracker::RpcState::NEW:
      // We're the only ones trying to execute the transaction (normal case). Proceed.
      return;
      // If this RPC was previously completed or is already stale (like if the same tablet was
      // bootstrapped twice) stop tracking the result. Only follower transactions can observe these
      // states so we simply reset the callback and the result will not be tracked anymore.
    case ResultTracker::RpcState::STALE:
    case ResultTracker::RpcState::COMPLETED: {
      mutable_state()->set_completion_callback(
          gscoped_ptr<TransactionCompletionCallback>(new TransactionCompletionCallback()));
      VLOG(2) << state()->result_tracker() << " Follower Rpc was already COMPLETED or STALE: "
          << rpc_state << " OpId: " << state()->op_id().ShortDebugString()
          << " RequestId: " << state()->request_id().ShortDebugString();
      return;
    }
    default:
      LOG(FATAL) << "Unexpected state: " << rpc_state;
  }
}

Status TransactionDriver::PrepareAndStart() {
  TRACE_EVENT1("txn", "PrepareAndStart", "txn", this);
  VLOG_WITH_PREFIX(4) << "PrepareAndStart()";
  // Actually prepare and start the transaction.
  prepare_physical_timestamp_ = GetMonoTimeMicros();

  RETURN_NOT_OK(transaction_->Prepare());
  RETURN_NOT_OK(transaction_->Start());

  // Only take the lock long enough to take a local copy of the
  // replication state and set our prepare state. This ensures that
  // exactly one of Replicate/Prepare callbacks will trigger the apply
  // phase.
  ReplicationState repl_state_copy;
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    CHECK_EQ(prepare_state_, NOT_PREPARED);
    prepare_state_ = PREPARED;
    repl_state_copy = replication_state_;

    // If this is a follower transaction we need to register the transaction on the tracker here,
    // atomically with the change of the prepared state. Otherwise if the prepare thread gets
    // preempted after the state is prepared apply can be triggered by another thread without the
    // rpc being registered.
    if (transaction_->type() == consensus::REPLICA) {
      RegisterFollowerTransactionOnResultTracker();
    // ... else we're a client-started transaction. Make sure we're still the driver of the
    // RPC and give up if we aren't.
    } else {
      if (state()->are_results_tracked()
          && !state()->result_tracker()->IsCurrentDriver(state()->request_id())) {
        transaction_status_ = Status::AlreadyPresent(strings::Substitute(
            "There's already an attempt of the same operation on the server for request id: $0",
            state()->request_id().ShortDebugString()));
        replication_state_ = REPLICATION_FAILED;
        return transaction_status_;
      }
    }
  }

  switch (repl_state_copy) {
    case NOT_REPLICATING:
    {
      // Set the timestamp in the message, now that it's prepared.
      transaction_->state()->consensus_round()->replicate_msg()->set_timestamp(
          transaction_->state()->timestamp().ToUint64());

      VLOG_WITH_PREFIX(4) << "Triggering consensus repl";
      // Trigger the consensus replication.

      {
        std::lock_guard<simple_spinlock> lock(lock_);
        replication_state_ = REPLICATING;
        replication_start_time_ = MonoTime::Now();
      }

      Status s = consensus_->Replicate(mutable_state()->consensus_round());
      if (PREDICT_FALSE(!s.ok())) {
        std::lock_guard<simple_spinlock> lock(lock_);
        CHECK_EQ(replication_state_, REPLICATING);
        transaction_status_ = s;
        replication_state_ = REPLICATION_FAILED;
        return s;
      }
      break;
    }
    case REPLICATING:
    {
      // Already replicating - nothing to trigger
      break;
    }
    case REPLICATION_FAILED:
      DCHECK(!transaction_status_.ok());
      FALLTHROUGH_INTENDED;
    case REPLICATED:
    {
      // We can move on to apply.
      // Note that ApplyAsync() will handle the error status in the
      // REPLICATION_FAILED case.
      return ApplyAsync();
    }
  }

  return Status::OK();
}

void TransactionDriver::HandleFailure(const Status& s) {
  VLOG_WITH_PREFIX(2) << "Failed transaction: " << s.ToString();
  CHECK(!s.ok());
  TRACE("HandleFailure($0)", s.ToString());

  ReplicationState repl_state_copy;

  {
    std::lock_guard<simple_spinlock> lock(lock_);
    transaction_status_ = s;
    repl_state_copy = replication_state_;
  }

  switch (repl_state_copy) {
    case NOT_REPLICATING:
    case REPLICATION_FAILED:
    {
      VLOG_WITH_PREFIX(1) << "Transaction " << ToString() << " failed prior to "
          "replication success: " << s.ToString();
      transaction_->Finish(Transaction::ABORTED);
      mutable_state()->completion_callback()->set_error(transaction_status_);
      mutable_state()->completion_callback()->TransactionCompleted();
      txn_tracker_->Release(this);
      return;
    }

    case REPLICATING:
    case REPLICATED:
    {
      LOG_WITH_PREFIX(FATAL) << "Cannot cancel transactions that have already replicated"
          << ": " << transaction_status_.ToString()
          << " transaction:" << ToString();
    }
  }
}

void TransactionDriver::ReplicationFinished(const Status& status) {
  MonoTime replication_finished_time = MonoTime::Now();

  ADOPT_TRACE(trace());
  {
    std::lock_guard<simple_spinlock> op_id_lock(opid_lock_);
    // TODO: it's a bit silly that we have three copies of the opid:
    // one here, one in ConsensusRound, and one in TransactionState.

    op_id_copy_ = DCHECK_NOTNULL(mutable_state()->consensus_round())->id();
    DCHECK(op_id_copy_.IsInitialized());
    mutable_state()->mutable_op_id()->CopyFrom(op_id_copy_);
  }

  MonoDelta replication_duration;
  PrepareState prepare_state_copy;
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    CHECK_EQ(replication_state_, REPLICATING);
    if (status.ok()) {
      replication_state_ = REPLICATED;
    } else {
      replication_state_ = REPLICATION_FAILED;
      transaction_status_ = status;
    }
    prepare_state_copy = prepare_state_;
    replication_duration = replication_finished_time - replication_start_time_;
  }


  TRACE_COUNTER_INCREMENT("replication_time_us", replication_duration.ToMicroseconds());

  // If we have prepared and replicated, we're ready
  // to move ahead and apply this operation.
  // Note that if we set the state to REPLICATION_FAILED above,
  // ApplyAsync() will actually abort the transaction, i.e.
  // ApplyTask() will never be called and the transaction will never
  // be applied to the tablet.
  if (prepare_state_copy == PREPARED) {
    // We likely need to do cleanup if this fails so for now just
    // CHECK_OK
    CHECK_OK(ApplyAsync());
  }
}

void TransactionDriver::Abort(const Status& status) {
  CHECK(!status.ok());

  ReplicationState repl_state_copy;
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    repl_state_copy = replication_state_;
    transaction_status_ = status;
  }

  // If the state is not NOT_REPLICATING we abort immediately and the transaction
  // will never be replicated.
  // In any other state we just set the transaction status, if the transaction's
  // Apply hasn't started yet this prevents it from starting, but if it has then
  // the transaction runs to completion.
  if (repl_state_copy == NOT_REPLICATING) {
    HandleFailure(status);
  }
}

Status TransactionDriver::ApplyAsync() {
  {
    std::unique_lock<simple_spinlock> lock(lock_);
    DCHECK_EQ(prepare_state_, PREPARED);
    if (transaction_status_.ok()) {
      DCHECK_EQ(replication_state_, REPLICATED);
      order_verifier_->CheckApply(op_id_copy_.index(),
                                  prepare_physical_timestamp_);
      // Now that the transaction is committed in consensus advance the safe time.
      if (transaction_->state()->external_consistency_mode() != COMMIT_WAIT) {
        transaction_->state()->tablet_peer()->tablet()->mvcc_manager()->
            OfflineAdjustSafeTime(transaction_->state()->timestamp());
      }
    } else {
      DCHECK_EQ(replication_state_, REPLICATION_FAILED);
      DCHECK(!transaction_status_.ok());
      lock.unlock();
      HandleFailure(transaction_status_);
      return Status::OK();
    }
  }

  TRACE_EVENT_FLOW_BEGIN0("txn", "ApplyTask", this);
  return apply_pool_->SubmitClosure(Bind(&TransactionDriver::ApplyTask, Unretained(this)));
}

void TransactionDriver::ApplyTask() {
  TRACE_EVENT_FLOW_END0("txn", "ApplyTask", this);
  ADOPT_TRACE(trace());

  {
    std::lock_guard<simple_spinlock> lock(lock_);
    DCHECK_EQ(replication_state_, REPLICATED);
    DCHECK_EQ(prepare_state_, PREPARED);
  }

  // We need to ref-count ourself, since Commit() may run very quickly
  // and end up calling Finalize() while we're still in this code.
  scoped_refptr<TransactionDriver> ref(this);

  {
    gscoped_ptr<CommitMsg> commit_msg;
    CHECK_OK(transaction_->Apply(&commit_msg));
    commit_msg->mutable_commited_op_id()->CopyFrom(op_id_copy_);
    SetResponseTimestamp(transaction_->state(), transaction_->state()->timestamp());

    {
      TRACE_EVENT1("txn", "AsyncAppendCommit", "txn", this);
      CHECK_OK(log_->AsyncAppendCommit(std::move(commit_msg),
                                       Bind(CrashIfNotOkStatusCB,
                                       "Enqueued commit operation failed to write to WAL")));
    }

    // If the client requested COMMIT_WAIT as the external consistency mode
    // calculate the latest that the prepare timestamp could be and wait
    // until now.earliest > prepare_latest. Only after this are the locks
    // released.
    if (mutable_state()->external_consistency_mode() == COMMIT_WAIT) {
      // TODO: only do this on the leader side
      TRACE("APPLY: Commit Wait.");
      // If we can't commit wait and have already applied we might have consistency
      // issues if we still reply to the client that the operation was a success.
      // On the other hand we don't have rollbacks as of yet thus we can't undo the
      // the apply either, so we just CHECK_OK for now.
      CHECK_OK(CommitWait());
    }

    Finalize();
  }
}

void TransactionDriver::SetResponseTimestamp(TransactionState* transaction_state,
                                             const Timestamp& timestamp) {
  google::protobuf::Message* response = transaction_state->response();
  if (response) {
    const google::protobuf::FieldDescriptor* ts_field =
        response->GetDescriptor()->FindFieldByName(kTimestampFieldName);
    response->GetReflection()->SetUInt64(response, ts_field, timestamp.ToUint64());
  }
}

Status TransactionDriver::CommitWait() {
  MonoTime before = MonoTime::Now();
  DCHECK(mutable_state()->external_consistency_mode() == COMMIT_WAIT);
  // TODO: we could plumb the RPC deadline in here, and not bother commit-waiting
  // if the deadline is already expired.
  RETURN_NOT_OK(
      mutable_state()->tablet_peer()->clock()->WaitUntilAfter(mutable_state()->timestamp(),
                                                              MonoTime::Max()));
  mutable_state()->mutable_metrics()->commit_wait_duration_usec =
      (MonoTime::Now() - before).ToMicroseconds();
  return Status::OK();
}

void TransactionDriver::Finalize() {
  ADOPT_TRACE(trace());
  // TODO: this is an ugly hack so that the Release() call doesn't delete the
  // object while we still hold the lock.
  scoped_refptr<TransactionDriver> ref(this);
  std::lock_guard<simple_spinlock> lock(lock_);
  transaction_->Finish(Transaction::COMMITTED);
  mutable_state()->completion_callback()->TransactionCompleted();
  txn_tracker_->Release(this);
}


std::string TransactionDriver::StateString(ReplicationState repl_state,
                                           PrepareState prep_state) {
  string state_str;
  switch (repl_state) {
    case NOT_REPLICATING:
      StrAppend(&state_str, "NR-"); // For Not Replicating
      break;
    case REPLICATING:
      StrAppend(&state_str, "R-"); // For Replicating
      break;
    case REPLICATION_FAILED:
      StrAppend(&state_str, "RF-"); // For Replication Failed
      break;
    case REPLICATED:
      StrAppend(&state_str, "RD-"); // For Replication Done
      break;
    default:
      LOG(DFATAL) << "Unexpected replication state: " << repl_state;
  }
  switch (prep_state) {
    case PREPARED:
      StrAppend(&state_str, "P");
      break;
    case NOT_PREPARED:
      StrAppend(&state_str, "NP");
      break;
    default:
      LOG(DFATAL) << "Unexpected prepare state: " << prep_state;
  }
  return state_str;
}

std::string TransactionDriver::LogPrefix() const {

  ReplicationState repl_state_copy;
  PrepareState prep_state_copy;
  string ts_string;

  {
    std::lock_guard<simple_spinlock> lock(lock_);
    repl_state_copy = replication_state_;
    prep_state_copy = prepare_state_;
    ts_string = state()->has_timestamp() ? state()->timestamp().ToString() : "No timestamp";
  }

  string state_str = StateString(repl_state_copy, prep_state_copy);
  // We use the tablet and the peer (T, P) to identify ts and tablet and the timestamp (Ts) to
  // (help) identify the transaction. The state string (S) describes the state of the transaction.
  return strings::Substitute("T $0 P $1 S $2 Ts $3: ",
                             // consensus_ is NULL in some unit tests.
                             PREDICT_TRUE(consensus_) ? consensus_->tablet_id() : "(unknown)",
                             PREDICT_TRUE(consensus_) ? consensus_->peer_uuid() : "(unknown)",
                             state_str,
                             ts_string);
}

}  // namespace tablet
}  // namespace kudu
