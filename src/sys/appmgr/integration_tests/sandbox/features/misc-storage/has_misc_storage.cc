// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/io/llcpp/fidl.h>

#include "src/sys/appmgr/integration_tests/sandbox/namespace_test.h"

namespace fio = ::llcpp::fuchsia::io;

TEST_F(NamespaceTest, HasMiscData) {
  ExpectExists("/misc");
  ExpectPathSupportsStrictRights("/misc", fio::OPEN_RIGHT_READABLE | fio::OPEN_RIGHT_WRITABLE);
}
