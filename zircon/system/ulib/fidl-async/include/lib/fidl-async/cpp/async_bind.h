// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_ASYNC_CPP_ASYNC_BIND_H_
#define LIB_FIDL_ASYNC_CPP_ASYNC_BIND_H_

#include <lib/async/cpp/task.h>
#include <lib/async/cpp/wait.h>
#include <lib/fidl/epitaph.h>
#include <lib/fidl/llcpp/transaction.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>
#include <zircon/fidl.h>

namespace fidl {

template <typename Interface>
using OnChannelCloseFn = fit::callback<void(Interface*)>;

namespace internal {

// Thread safety token.
//
// This token acts like a "no-op mutex", allowing compiler thread safety annotations
// to be placed on code or data that should only be accessed by a particular thread.
// Any code that acquires the token makes the claim that it is running on the (single)
// correct thread, and hence it is safe to access the annotated data and execute the annotated code.
struct __TA_CAPABILITY("role") Token {};
class __TA_SCOPED_CAPABILITY ScopedToken {
 public:
  explicit ScopedToken(const Token& token) __TA_ACQUIRE(token) {}
  ~ScopedToken() __TA_RELEASE() {}
};

class AsyncTransaction;
class AsyncBinding;

using TypeErasedDispatchFn = bool (*)(void*, fidl_msg_t*, ::fidl::Transaction*);

using TypeErasedOnChannelCloseFn = fit::callback<void(void*)>;

zx_status_t AsyncTypeErasedBind(async_dispatcher_t* dispatcher, zx::channel channel, void* impl,
                                TypeErasedDispatchFn dispatch_fn,
                                TypeErasedOnChannelCloseFn on_channel_closing_fn,
                                TypeErasedOnChannelCloseFn on_channel_closed_fn);

// This class abstracts the binding of a channel, a single threaded dispatcher and an implementation
// of the llcpp bindings.  The static CreateSelfManagedBinding method creates a binding that stays
// alive until either a peer close is recevied in the channel from the remote end, or all
// transactions generated from it are destructed and an error occurred (either Close() is called
// from a tranasction or an internal error like not being able to write to the channel occur).
class AsyncBinding {
 public:
  static std::shared_ptr<AsyncBinding> CreateSelfManagedBinding(
      async_dispatcher_t* dispatcher, zx::channel channel, void* impl,
      TypeErasedDispatchFn dispatch_fn, TypeErasedOnChannelCloseFn on_channel_closing_fn,
      TypeErasedOnChannelCloseFn on_channel_closed_fn);
  ~AsyncBinding() __TA_REQUIRES(domain_token());

  void MessageHandler(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                      const zx_packet_signal_t* signal) __TA_REQUIRES(domain_token());
  zx_status_t BeginWait() __TA_EXCLUDES(domain_token()) { return callback_.Begin(dispatcher_); }

 protected:
  explicit AsyncBinding(async_dispatcher_t* dispatcher, zx::channel channel, void* impl,
                        TypeErasedDispatchFn dispatch_fn,
                        TypeErasedOnChannelCloseFn on_channel_closing_fn,
                        TypeErasedOnChannelCloseFn on_channel_closed_fn);

 private:
  friend fidl::internal::AsyncTransaction;

  zx::unowned_channel channel() const { return zx::unowned_channel(channel_); }
  const Token& domain_token() const __TA_RETURN_CAPABILITY(domain_token_) { return domain_token_; }
  void OnChannelClosing() __TA_REQUIRES(domain_token());
  void Close(zx_status_t epitaph) __TA_EXCLUDES(domain_token());
  void Release(std::shared_ptr<AsyncBinding> reference) __TA_EXCLUDES(domain_token());

  Token domain_token_;
  async_dispatcher_t* dispatcher_;
  zx::channel channel_;
  void* interface_;
  TypeErasedDispatchFn dispatch_fn_;
  TypeErasedOnChannelCloseFn on_channel_closing_fn_ __TA_GUARDED(domain_token()) = {};
  TypeErasedOnChannelCloseFn on_channel_closed_fn_ __TA_GUARDED(domain_token()) = {};
  zx_status_t epitaph_ __TA_GUARDED(domain_token()) = ZX_OK;
  async::WaitMethod<AsyncBinding, &AsyncBinding::MessageHandler> callback_{this};
  std::shared_ptr<AsyncBinding> keep_alive_ = {};
};

}  // namespace internal

// Binds an implementation of a low-level C++ server interface to |channel| using a
// single-threaded |dispatcher|.
// This implementation allows for multiple in-flight asynchronous transactions.
//
// This function adds an |async::WaitMethod| to the given single-threaded |dispatcher| that waits
// asynchronously for new messages to arrive on |channel|. When a message arrives, the dispatch
// function corresponding to the interface is called on the |dispatcher| thread.
//
// Typically, the dispatch function is generated by the low-level C++ backend
// for FIDL interfaces. These dispatch functions decode the |fidl_msg_t| and
// call into the implementation of the FIDL interface, via its C++ vtable.
//
// When an error occurs in the server implementation as part of handling the message, it may call
// |Close(error)| on the completer to indicate the error condition to the dispatcher.
// The dispatcher will send an epitaph with this error code before closing the channel once all
// transactions are destroyed.  If the client end of the channel gets closed, the binding will be
// destroyed once all transactions are destroyed.
//
// Returns whether |fidl::AsyncBind| was able to begin waiting on the given |channel|.
// Upon any error, |channel| is closed and the binding is terminated.
//
// The |dispatcher| takes ownership of the channel. The |dispatcher| is automatically destroyed
// only after all transactions are destroyed.
//
// TODO(38454): Add ability to unbind without errors/Close()-via-transaction, hide the
// binding implementation, and make the closing callback more like the hlcpp binding.h error_handler
// (suggestions by dustingreen@).
// TODO(38455): Specify reason in close related callbacks (suggestion by yifeit@).
// TODO(38456): Add support for multithreaded dispatchers.
//
template <typename Interface>
zx_status_t AsyncBind(async_dispatcher_t* dispatcher, zx::channel channel, Interface* impl) {
  return internal::AsyncTypeErasedBind(dispatcher, std::move(channel), impl,
                                       &Interface::_Outer::TypeErasedDispatch, nullptr, nullptr);
}

// As above, but will invoke |on_channel_closing_fn| on |impl| when either end of the |channel|
// is being closed.  The server end gets closed once all transactions are destroyed and either
// someone called Close() or there was an error for instance not able to write to the |channel|.
// If the client end gets closed, the binding will be destroyed once all transactions are
// destroyed.  on_channel_closing_fn will be called once someone calls Close(), there was an error,
// or the client end gets closed, this notification allows in-flight transactions to be cancelled
// earlier if desired.  |on_channel_closed_fn| is called when the channel is getting closed as part
// of the binding destruction.
template <typename Interface>
zx_status_t AsyncBind(async_dispatcher_t* dispatcher, zx::channel channel, Interface* impl,
                      OnChannelCloseFn<Interface> on_channel_closing_fn,
                      OnChannelCloseFn<Interface> on_channel_closed_fn) {
  return internal::AsyncTypeErasedBind(
      dispatcher, std::move(channel), impl, &Interface::_Outer::TypeErasedDispatch,
      [fn = std::move(on_channel_closing_fn)](void* impl) mutable {
        fn(static_cast<Interface*>(impl));
      },
      [fn = std::move(on_channel_closed_fn)](void* impl) mutable {
        fn(static_cast<Interface*>(impl));
      });
}

// As above, but will destroy |impl| as the binding is destroyed and the |channel| closed.
// The server end gets closed once all transactions are destroyed and either someone called
// Close() or there was an error for instance not able to write to the |channel|.
// If the client end gets closed, the binding will be destroyed once all transactions are destroyed
// and then |impl| will be destroyed.
template <typename Interface>
zx_status_t AsyncBind(async_dispatcher_t* dispatcher, zx::channel channel,
                      std::unique_ptr<Interface> impl) {
  OnChannelCloseFn<Interface> on_closing = [](Interface* impl) {};
  OnChannelCloseFn<Interface> on_closed = [](Interface* impl) { delete impl; };
  return AsyncBind(dispatcher, std::move(channel), impl.release(), std::move(on_closing),
                   std::move(on_closed));
}

}  // namespace fidl

#endif  // LIB_FIDL_ASYNC_CPP_ASYNC_BIND_H_
