// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "utils.h"

#include <lib/zx/channel.h>
#include <string.h>
#include <zircon/assert.h>
#include <zircon/device/intel-hda.h>
#include <zircon/process.h>

#include <utility>

#include <ddk/binding.h>
#include <ddk/driver.h>
#include <dispatcher-pool/dispatcher-channel.h>
#include <dispatcher-pool/dispatcher-execution-domain.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <intel-hda/utils/intel-hda-registers.h>

#include "debug-logging.h"

namespace audio {
namespace intel_hda {

fbl::RefPtr<fzl::VmarManager> CreateDriverVmars() {
  // Create a compact VMAR to map all of our registers into.
  //
  // TODO(johngro): See ZX-1822 for details.
  //
  // Sizing right now is a bit of a guessing game.  A compact VMAR is not
  // going to perfectly tightly pack everything; it will still insert random
  // gaps in an attempt to get some minimum level of ASLR.  For now, I'm using
  // hardcoded guidance from teisenbe@ about how to size for the worst case.
  // If/when there is a better way of doing this, I need to come back and
  // switch to that.
  //
  // Formula being used here should be...
  // 2 * (total_region_size + (512k * (total_allocations - 1)))
  constexpr size_t MAX_SIZE_PER_CONTROLLER = sizeof(hda_all_registers_t) + MAPPED_CORB_RIRB_SIZE +
                                             (MAX_STREAMS_PER_CONTROLLER * MAPPED_BDL_SIZE) +
                                             sizeof(adsp_registers_t) + MAPPED_BDL_SIZE;

  // One alloc for the main registers, one for code loader BDL.
  constexpr size_t MAX_ALLOCS_PER_DSP = 2;
  // One alloc for the main registers, one for the CORB/RIRB, two for DSP,
  // and one for each possible stream BDL.
  constexpr size_t MAX_ALLOCS_PER_CONTROLLER = 2 + MAX_ALLOCS_PER_DSP + MAX_STREAMS_PER_CONTROLLER;
  constexpr size_t VMAR_SIZE =
      2 * (MAX_SIZE_PER_CONTROLLER + ((MAX_ALLOCS_PER_CONTROLLER - 1) * (512u << 10)));

  GLOBAL_LOG(DEBUG, "Allocating 0x%zx byte VMAR for registers.\n", VMAR_SIZE);

  return fzl::VmarManager::Create(VMAR_SIZE);
}

zx_status_t CreateAndActivateChannel(const fbl::RefPtr<dispatcher::ExecutionDomain>& domain,
                                     dispatcher::Channel::ProcessHandler phandler,
                                     dispatcher::Channel::ChannelClosedHandler chandler,
                                     fbl::RefPtr<dispatcher::Channel>* local_endpoint_out,
                                     zx::channel* remote_endpoint_out) {
  if (remote_endpoint_out == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto channel = dispatcher::Channel::Create();
  if (channel == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t res =
      channel->Activate(remote_endpoint_out, domain, std::move(phandler), std::move(chandler));
  if ((res == ZX_OK) && (local_endpoint_out != nullptr)) {
    *local_endpoint_out = std::move(channel);
  }

  return res;
}

}  // namespace intel_hda
}  // namespace audio
