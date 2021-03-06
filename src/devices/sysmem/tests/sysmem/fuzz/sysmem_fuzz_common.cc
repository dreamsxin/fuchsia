// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "sysmem_fuzz_common.h"

#include "log_rtn.h"

FakeDdkSysmem::~FakeDdkSysmem() {
  if (initialized_) {
    sysmem_.DdkAsyncRemove();
    initialized_ = false;
  }
}
bool FakeDdkSysmem::Init() {
  if (initialized_) {
    fprintf(stderr, "FakeDdkSysmem already initialized.\n");
    fflush(stderr);
    return false;
  }
  fbl::Array<fake_ddk::ProtocolEntry> protocols(new fake_ddk::ProtocolEntry[2], 2);
  protocols[0] = {ZX_PROTOCOL_PBUS, *reinterpret_cast<const fake_ddk::Protocol*>(pbus_.proto())};
  protocols[1] = {ZX_PROTOCOL_PDEV, *reinterpret_cast<const fake_ddk::Protocol*>(pdev_.proto())};
  ddk_.SetProtocols(std::move(protocols));
  if (ZX_OK == sysmem_.Bind()) {
    initialized_ = true;
  }
  return initialized_;
}

zx_status_t connect_to_sysmem_driver(zx_handle_t fake_ddk_client,
                                     zx::channel* allocator_client_param) {
  zx::channel allocator_client;
  zx::channel allocator_server;
  zx_status_t status = zx::channel::create(0, &allocator_client, &allocator_server);
  LOGRTN(status, "Failed allocator channel create.\n");

  status = fuchsia_sysmem_DriverConnectorConnect(fake_ddk_client, allocator_server.release());
  LOGRTN(status, "Failed sysmem driver connect.\n");

  *allocator_client_param = std::move(allocator_client);
  return ZX_OK;
}
