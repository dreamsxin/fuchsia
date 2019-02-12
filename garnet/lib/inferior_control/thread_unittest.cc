// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async/cpp/task.h>
#include <lib/zx/channel.h>

#include "garnet/lib/debugger_utils/threads.h"
#include "garnet/lib/inferior_control/process.h"
#include "garnet/lib/inferior_control/test_server.h"

#include "gtest/gtest.h"

namespace inferior_control {
namespace {

// TODO(dje): Obtain path more cleanly.
const char helper_program[] =
    "/pkgfs/packages/inferior_control_tests/0/bin/"
    "inferior_control_test_helper";

// Test resume from exception and try-next.
// Note: Exceptions are handled in the same thread as server.Run().

class TryNextThreadTest : public TestServer {
 public:
  TryNextThreadTest() = default;

  bool got_sw_breakpoint() const { return got_sw_breakpoint_; }
  bool got_unexpected_exception() const { return got_unexpected_exception_; }

  void OnArchitecturalException(
      Process* process, Thread* thread, zx_excp_type_t type,
      const zx_exception_context_t& context) {
    FXL_LOG(INFO) << "Got exception 0x" << std::hex << type;
    if (type == ZX_EXCP_SW_BREAKPOINT) {
      got_sw_breakpoint_ = true;
      thread->TryNext();
    } else {
      // We shouldn't get here, test has failed.
      // Record the fact for the test, and terminate the inferior, we don't
      // want the exception propagating to the system exception handler.
      got_unexpected_exception_ = true;
      zx_task_kill(process->handle());
    }
  }

 private:
  bool got_sw_breakpoint_ = false;
  bool got_unexpected_exception_ = false;
};

TEST_F(TryNextThreadTest, ResumeTryNextTest) {
  std::vector<std::string> argv{
    helper_program,
    "trigger-sw-bkpt",
  };
  ASSERT_TRUE(SetupInferior(argv));

  zx::channel our_channel, their_channel;
  auto status = zx::channel::create(0, &our_channel, &their_channel);
  ASSERT_EQ(status, ZX_OK);
  EXPECT_TRUE(RunHelperProgram(std::move(their_channel)));

  // The inferior is waiting for us to close our side of the channel.
  our_channel.reset();

  EXPECT_TRUE(Run());
  EXPECT_TRUE(TestSuccessfulExit());
  EXPECT_TRUE(got_sw_breakpoint());
  EXPECT_FALSE(got_unexpected_exception());
}

class SuspendResumeThreadTest : public TestServer {
 public:
  SuspendResumeThreadTest() = default;

  bool got_sw_breakpoint() const { return got_sw_breakpoint_; }
  bool got_unexpected_exception() const { return got_unexpected_exception_; }

  void OnThreadStarting(Process* process, Thread* thread,
                        const zx_exception_context_t& context) override {
    if (main_thread_id_ == ZX_KOID_INVALID) {
      // Must be the inferior's main thread.
      main_thread_id_ = thread->id();
      FXL_LOG(INFO) << "Main thread = " << main_thread_id_;
    } else {
      // Must be the exception handling thread.
      FXL_CHECK(exception_handling_thread_id_ == ZX_KOID_INVALID);
      exception_handling_thread_id_ = thread->id();
      FXL_LOG(INFO) << "Exception handling thread = "
                    << exception_handling_thread_id_;
    }
    TestServer::OnThreadStarting(process, thread, context);
  }

  void OnArchitecturalException(
      Process* process, Thread* thread, zx_excp_type_t type,
      const zx_exception_context_t& context) override {
    FXL_LOG(INFO) << "Got exception 0x" << std::hex << type;
    if (type == ZX_EXCP_SW_BREAKPOINT) {
      FXL_CHECK(thread->id() == main_thread_id_) << thread->id();
      got_sw_breakpoint_ = true;
      // The exception handling thread is either in |zx_port_wait()| or on
      // its way there.
      Thread* ethread = process->FindThreadById(exception_handling_thread_id_);
      FXL_CHECK(ethread);
      FXL_CHECK(ethread->RequestSuspend());
    } else {
      // We shouldn't get here, test has failed.
      // Record the fact for the test, and terminate the inferior, we don't
      // want the exception propagating to the system exception handler.
      got_unexpected_exception_ = true;
      zx_task_kill(process->handle());
    }
  }

  void OnThreadSuspension(Thread* thread) override {
    // This should be the exception-handling thread. The thread that got the
    // s/w breakpoint should still be in the breakpoint.
    FXL_CHECK(thread->id() == exception_handling_thread_id_) << thread->id();
    FXL_CHECK(debugger_utils::GetThreadOsState(thread->handle()) ==
              ZX_THREAD_STATE_SUSPENDED);
    Process* process = thread->process();
    Thread* mthread = process->FindThreadById(main_thread_id_);
    FXL_CHECK(mthread);
    FXL_CHECK(debugger_utils::GetThreadOsState(mthread->handle()) ==
              ZX_THREAD_STATE_BLOCKED_EXCEPTION);
    thread->ResumeFromSuspension();
  }

  void OnThreadResumption(Thread* thread) override {
    // We also get ZX_THREAD_RUNNING signals when threads are resumed from
    // exceptions. If this is the exception-handling thread then the thread
    // that got the s/w breakpoint should still be in the breakpoint.
    if (thread->id() == main_thread_id_) {
      // Nothing to do.
    } else {
      // Wait until the main thread is in the s/w breakpoint.
      if (got_sw_breakpoint_) {
        FXL_CHECK(thread->id() == exception_handling_thread_id_);
        Process* process = thread->process();
        Thread* mthread = process->FindThreadById(main_thread_id_);
        FXL_CHECK(mthread);
        FXL_CHECK(debugger_utils::GetThreadOsState(mthread->handle()) ==
                  ZX_THREAD_STATE_BLOCKED_EXCEPTION)
          << debugger_utils::GetThreadOsState(mthread->handle());
        mthread->TryNext();
      }
    }
  }

 private:
  zx_koid_t main_thread_id_ = ZX_KOID_INVALID;
  zx_koid_t exception_handling_thread_id_ = ZX_KOID_INVALID;
  bool got_sw_breakpoint_ = false;
  bool got_unexpected_exception_ = false;
};

TEST_F(SuspendResumeThreadTest, SuspendResumeTest) {
  std::vector<std::string> argv{
    helper_program,
    "trigger-sw-bkpt",
  };
  ASSERT_TRUE(SetupInferior(argv));

  zx::channel our_channel, their_channel;
  auto status = zx::channel::create(0, &our_channel, &their_channel);
  ASSERT_EQ(status, ZX_OK);
  EXPECT_TRUE(RunHelperProgram(std::move(their_channel)));

  // The inferior is waiting for us to close our side of the channel.
  our_channel.reset();

  EXPECT_TRUE(Run());
  EXPECT_TRUE(TestSuccessfulExit());
  EXPECT_TRUE(got_sw_breakpoint());
  EXPECT_FALSE(got_unexpected_exception());
}

}  // namespace
}  // namespace inferior_control
