// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/testing/inspect.h"

#include <lib/async-testing/test_loop.h>
#include <lib/async_promise/executor.h>
#include <lib/callback/capture.h>
#include <lib/callback/set_when_called.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fit/function.h>
#include <lib/inspect_deprecated/deprecated/expose.h>
#include <lib/inspect_deprecated/hierarchy.h>
#include <lib/inspect_deprecated/inspect.h>
#include <lib/inspect_deprecated/reader.h>
#include <lib/inspect_deprecated/testing/inspect.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fuchsia/ledger/cpp/fidl.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "peridot/lib/convert/convert.h"
#include "src/ledger/bin/inspect/inspect.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/ledger/bin/testing/inspect.h"
#include "src/ledger/bin/testing/loop_controller.h"
#include "src/ledger/bin/testing/loop_controller_test_loop.h"

namespace ledger {
namespace {

using ::inspect_deprecated::testing::ChildrenMatch;
using ::inspect_deprecated::testing::NameMatches;
using ::inspect_deprecated::testing::NodeMatches;
using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

template <typename P>
testing::AssertionResult OpenChild(P parent, const std::string& child_name,
                                   fidl::InterfacePtr<fuchsia::inspect::Inspect>* child,
                                   LoopController* loop_controller) {
  bool success;
  std::unique_ptr<CallbackWaiter> waiter = loop_controller->NewWaiter();
  (*parent)->OpenChild(child_name, child->NewRequest(),
                       callback::Capture(waiter->GetCallback(), &success));
  if (!waiter->RunUntilCalled()) {
    return ::testing::AssertionFailure() << "RunUntilCalled not successful!";
  }
  if (!success) {
    return ::testing::AssertionFailure() << "OpenChild not successful!";
  }
  return ::testing::AssertionSuccess();
}

}  // namespace

testing::AssertionResult OpenChild(inspect_deprecated::Node* parent, const std::string& child_name,
                                   fidl::InterfacePtr<fuchsia::inspect::Inspect>* child,
                                   LoopController* loop_controller) {
  std::shared_ptr<component::Object> parent_object = parent->object_dir().object();
  return OpenChild(&parent_object, child_name, child, loop_controller);
}

testing::AssertionResult OpenChild(inspect_deprecated::Node* parent, const std::string& child_name,
                                   fidl::InterfacePtr<fuchsia::inspect::Inspect>* child,
                                   async::TestLoop* test_loop) {
  std::shared_ptr<component::Object> parent_object = parent->object_dir().object();
  LoopControllerTestLoop loop_controller(test_loop);
  return OpenChild(&parent_object, child_name, child, &loop_controller);
}

testing::AssertionResult OpenChild(fidl::InterfacePtr<fuchsia::inspect::Inspect>* parent,
                                   const std::string& child_name,
                                   fidl::InterfacePtr<fuchsia::inspect::Inspect>* child,
                                   async::TestLoop* test_loop) {
  LoopControllerTestLoop loop_controller(test_loop);
  return OpenChild(parent, child_name, child, &loop_controller);
}

testing::AssertionResult Inspect(fuchsia::inspect::InspectPtr* top_level,
                                 LoopController* loop_controller,
                                 inspect_deprecated::ObjectHierarchy* hierarchy) {
  fidl::InterfaceHandle<fuchsia::inspect::Inspect> handle = top_level->Unbind();
  inspect_deprecated::ObjectReader object_reader =
      inspect_deprecated::ObjectReader(std::move(handle));
  async::Executor executor(loop_controller->dispatcher());
  fit::result<inspect_deprecated::ObjectHierarchy> hierarchy_result;
  std::unique_ptr<CallbackWaiter> waiter = loop_controller->NewWaiter();
  auto hierarchy_promise =
      inspect_deprecated::ReadFromFidl(object_reader)
          .then([&](fit::result<inspect_deprecated::ObjectHierarchy>& then_hierarchy_result) {
            hierarchy_result = std::move(then_hierarchy_result);
            waiter->GetCallback()();
          });
  executor.schedule_task(std::move(hierarchy_promise));
  if (!waiter->RunUntilCalled()) {
    return ::testing::AssertionFailure() << "RunUntilCalled not successful!";
  }
  if (!hierarchy_result.is_ok()) {
    return ::testing::AssertionFailure() << "Hierarchy result not okay!";
  }
  *hierarchy = hierarchy_result.take_value();
  top_level->Bind(object_reader.TakeChannel());
  return ::testing::AssertionSuccess();
}

testing::AssertionResult Inspect(inspect_deprecated::Node* top_level_node,
                                 async::TestLoop* test_loop,
                                 inspect_deprecated::ObjectHierarchy* hierarchy) {
  LoopControllerTestLoop loop_controller(test_loop);
  fidl::InterfacePtr<fuchsia::inspect::Inspect> inspect_ptr;
  testing::AssertionResult open_child_result =
      OpenChild(top_level_node, kSystemUnderTestAttachmentPointPathComponent.ToString(),
                &inspect_ptr, &loop_controller);
  if (!open_child_result) {
    return open_child_result;
  }
  return Inspect(&inspect_ptr, &loop_controller, hierarchy);
}

::testing::Matcher<const inspect_deprecated::ObjectHierarchy&> PageMatches(
    const convert::ExtendedStringView& page_id,
    const std::set<const std::optional<const storage::CommitId>>& heads) {
  std::vector<testing::Matcher<const inspect_deprecated::ObjectHierarchy&>> head_matchers;
  head_matchers.reserve(heads.size());
  for (const std::optional<const storage::CommitId>& head : heads) {
    if (head) {
      head_matchers.push_back(NodeMatches(NameMatches(CommitIdToDisplayName(head.value()))));
    } else {
      head_matchers.push_back(_);
    }
  }
  return AllOf(NodeMatches(NameMatches(PageIdToDisplayName(page_id.ToString()))),
               ChildrenMatch(ElementsAre(
                   NodeMatches(NameMatches(kCommitsInspectPathComponent.ToString())),
                   AllOf(NodeMatches(NameMatches(kHeadsInspectPathComponent.ToString())),
                         ChildrenMatch(UnorderedElementsAreArray(head_matchers))))));
}

::testing::Matcher<const inspect_deprecated::ObjectHierarchy&> LedgerMatches(
    const convert::ExtendedStringView& ledger_name,
    const std::vector<testing::Matcher<const inspect_deprecated::ObjectHierarchy&>>&
        page_matchers) {
  return AllOf(NodeMatches(NameMatches(ledger_name.ToString())),
               ChildrenMatch(ElementsAre(
                   AllOf(NodeMatches(NameMatches(kPagesInspectPathComponent.ToString())),
                         ChildrenMatch(ElementsAreArray(page_matchers))))));
}

testing::Matcher<const inspect_deprecated::ObjectHierarchy&> RepositoryMatches(
    std::optional<convert::ExtendedStringView> repository_name,
    const std::vector<testing::Matcher<const inspect_deprecated::ObjectHierarchy&>>&
        ledger_matchers) {
  auto children_match = ChildrenMatch(
      ElementsAre(AllOf(NodeMatches(NameMatches(kLedgersInspectPathComponent.ToString())),
                        ChildrenMatch(ElementsAreArray(ledger_matchers)))));
  if (repository_name) {
    return AllOf(NodeMatches(NameMatches(repository_name.value().ToString())), children_match);
  }
  return children_match;
}

testing::Matcher<const inspect_deprecated::ObjectHierarchy&> RepositoriesAggregateMatches(
    const std::vector<testing::Matcher<const inspect_deprecated::ObjectHierarchy&>>&
        repository_matchers) {
  return AllOf(NodeMatches(NameMatches(kRepositoriesInspectPathComponent.ToString())),
               ChildrenMatch(ElementsAreArray(repository_matchers)));
}

}  // namespace ledger
