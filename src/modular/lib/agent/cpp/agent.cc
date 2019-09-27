// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/lib/agent/cpp/agent.h"

namespace modular {

Agent::Agent(std::shared_ptr<sys::OutgoingDirectory> publish_dir,
             fit::callback<void()> on_terminate)
    : publish_dir_(std::move(publish_dir)), on_terminate_(std::move(on_terminate)) {
  publish_dir_->AddPublicService<fuchsia::modular::Agent>(agent_bindings_.GetHandler(this));
  publish_dir_->AddPublicService<fuchsia::modular::Lifecycle>(lifecycle_bindings_.GetHandler(this));
}

Agent::~Agent() = default;

// |fuchsia::modular::Agent|
void Agent::Connect(
    std::string requestor_id,
    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> outgoing_services_request) {
  agent_service_provider_bindings_.AddBinding(this, std::move(outgoing_services_request));
}

// |fuchsia::modular::Agent|
void Agent::RunTask(std::string task_id, fit::function<void()> done) { done(); }

// |fuchsia::sys::ServiceProvider|
void Agent::ConnectToService(std::string service_name, zx::channel request) {
  auto it = service_name_to_handler_.find(service_name);
  if (it != service_name_to_handler_.end()) {
    it->second(std::move(request));
  }
}

// |fuchsia::modular::Lifecycle|
void Agent::Terminate() {
  // Unpublish Agent interfaces.
  publish_dir_->RemovePublicService<fuchsia::modular::Agent>();
  publish_dir_->RemovePublicService<fuchsia::modular::Lifecycle>();

  // Stop processing further requests.
  agent_bindings_.CloseAll();
  lifecycle_bindings_.CloseAll();
  agent_service_provider_bindings_.CloseAll();

  on_terminate_();
}

}  // namespace modular
