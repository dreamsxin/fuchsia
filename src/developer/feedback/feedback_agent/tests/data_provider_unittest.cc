// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_agent/data_provider.h"

#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/math/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/fit/result.h>
#include <lib/fostr/fidl/fuchsia/math/formatting.h>
#include <lib/fsl/vmo/file.h>
#include <lib/fsl/vmo/sized_vmo.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fsl/vmo/vector.h>
#include <lib/gtest/real_loop_fixture.h>
#include <lib/sys/cpp/testing/service_directory_provider.h>
#include <lib/sys/cpp/testing/test_with_environment.h>
#include <lib/syslog/cpp/logger.h>
#include <lib/syslog/logger.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "src/developer/feedback/feedback_agent/config.h"
#include "src/developer/feedback/feedback_agent/constants.h"
#include "src/developer/feedback/feedback_agent/tests/stub_channel_provider.h"
#include "src/developer/feedback/feedback_agent/tests/stub_logger.h"
#include "src/developer/feedback/feedback_agent/tests/stub_scenic.h"
#include "src/developer/feedback/testing/gmatchers.h"
#include "src/developer/feedback/testing/gpretty_printers.h"
#include "src/developer/feedback/utils/archive.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/fxl/strings/substitute.h"
#include "src/lib/fxl/test/test_settings.h"
#include "third_party/googletest/googlemock/include/gmock/gmock.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/schema.h"

namespace feedback {
namespace {

using fuchsia::feedback::Attachment;
using fuchsia::feedback::Data;
using fuchsia::feedback::ImageEncoding;
using fuchsia::feedback::Screenshot;

const std::set<std::string> kDefaultAnnotations = {
    kAnnotationBuildBoard,   kAnnotationBuildLatestCommitDate,
    kAnnotationBuildProduct, kAnnotationBuildVersion,
    kAnnotationChannel,      kAnnotationDeviceBoardName,
    kAnnotationDeviceUptime,
};
const std::set<std::string> kDefaultAttachments = {
    kAttachmentBuildSnapshot,
    kAttachmentInspect,
    kAttachmentLogKernel,
    kAttachmentLogSystem,
};
const Config kDefaultConfig = Config{kDefaultAnnotations, kDefaultAttachments};

constexpr bool kSuccess = true;
constexpr bool kFailure = false;

// Returns a Screenshot with the right dimensions, no image.
std::unique_ptr<Screenshot> MakeUniqueScreenshot(const size_t image_dim_in_px) {
  std::unique_ptr<Screenshot> screenshot = std::make_unique<Screenshot>();
  screenshot->dimensions_in_px.height = image_dim_in_px;
  screenshot->dimensions_in_px.width = image_dim_in_px;
  return screenshot;
}

// Represents arguments for DataProvider::GetScreenshotCallback.
struct GetScreenshotResponse {
  std::unique_ptr<Screenshot> screenshot;

  // This should be kept in sync with DoGetScreenshotResponseMatch() as we only want to display what
  // we actually compare, for now the presence of a screenshot and its dimensions if present.
  operator std::string() const {
    if (!screenshot) {
      return "no screenshot";
    }
    const fuchsia::math::Size& dimensions_in_px = screenshot->dimensions_in_px;
    return fxl::StringPrintf("a %d x %d screenshot", dimensions_in_px.width,
                             dimensions_in_px.height);
  }

  // This is used by gTest to pretty-prints failed expectations instead of the default byte string.
  friend std::ostream& operator<<(std::ostream& os, const GetScreenshotResponse& response) {
    return os << std::string(response);
  }
};

// Compares two GetScreenshotResponse objects.
//
// This should be kept in sync with std::string() as we only want to display what we actually
// compare, for now the presence of a screenshot and its dimensions.
template <typename ResultListenerT>
bool DoGetScreenshotResponseMatch(const GetScreenshotResponse& actual,
                                  const GetScreenshotResponse& expected,
                                  ResultListenerT* result_listener) {
  if (actual.screenshot == nullptr && expected.screenshot == nullptr) {
    return true;
  }
  if (actual.screenshot == nullptr && expected.screenshot != nullptr) {
    *result_listener << "Got no screenshot, expected one";
    return false;
  }
  if (expected.screenshot == nullptr && actual.screenshot != nullptr) {
    *result_listener << "Expected no screenshot, got one";
    return false;
  }
  // actual.screenshot and expected.screenshot are now valid.

  if (!fidl::Equals(actual.screenshot->dimensions_in_px, expected.screenshot->dimensions_in_px)) {
    *result_listener << "Expected screenshot dimensions " << expected.screenshot->dimensions_in_px
                     << ", got " << actual.screenshot->dimensions_in_px;
    return false;
  }

  // We do not compare the VMOs.

  return true;
}

// Returns true if gMock |arg| matches |expected|, assuming two GetScreenshotResponse objects.
MATCHER_P(MatchesGetScreenshotResponse, expected, "matches " + std::string(expected.get())) {
  return DoGetScreenshotResponseMatch(arg, expected, result_listener);
}

// Unit-tests the implementation of the fuchsia.feedback.DataProvider FIDL interface.
//
// This does not test the environment service. It directly instantiates the class, without
// connecting through FIDL.
class DataProviderImplTest : public sys::testing::TestWithEnvironment {
 public:
  void SetUp() override { SetUpDataProvider(kDefaultConfig); }

  void TearDown() override {
    if (!controller_) {
      return;
    }
    controller_->Kill();
    bool done = false;
    controller_.events().OnTerminated = [&done](int64_t code,
                                                fuchsia::sys::TerminationReason reason) {
      FXL_CHECK(reason == fuchsia::sys::TerminationReason::EXITED);
      done = true;
    };
    RunLoopUntil([&done] { return done; });
  }

 protected:
  void SetUpDataProvider(const Config& config) {
    data_provider_.reset(new DataProviderImpl(
        dispatcher(), service_directory_provider_.service_directory(), config));
  }

  void SetUpScenic(std::unique_ptr<StubScenic> stub_scenic) {
    stub_scenic_ = std::move(stub_scenic);
    if (stub_scenic_) {
      FXL_CHECK(service_directory_provider_.AddService(stub_scenic_->GetHandler()) == ZX_OK);
    }
  }

  void SetUpLogger(const std::vector<fuchsia::logger::LogMessage>& messages) {
    stub_logger_.reset(new StubLogger());
    stub_logger_->set_messages(messages);
    FXL_CHECK(service_directory_provider_.AddService(stub_logger_->GetHandler()) == ZX_OK);
  }

  void SetUpChannelProvider(std::string channel) {
    stub_channel_provider_.reset(new StubChannelProvider());
    stub_channel_provider_->set_channel(channel);
    FXL_CHECK(service_directory_provider_.AddService(stub_channel_provider_->GetHandler()) ==
              ZX_OK);
  }

  // Injects a test app that exposes some Inspect data in the test environment.
  //
  // Useful to guarantee there is a component within the environment that exposes Inspect data as we
  // are excluding system_objects paths from the Inspect discovery and the test component itself
  // only has a system_objects Inspect node.
  void InjectInspectTestApp() {
    fuchsia::sys::LaunchInfo launch_info;
    launch_info.url =
        "fuchsia-pkg://fuchsia.com/feedback_agent_tests#meta/"
        "inspect_test_app.cmx";
    environment_ = CreateNewEnclosingEnvironment("inspect_test_app_environment", CreateServices());
    environment_->CreateComponent(std::move(launch_info), controller_.NewRequest());
    bool ready = false;
    controller_.events().OnDirectoryReady = [&ready] { ready = true; };
    RunLoopUntil([&ready] { return ready; });
  }

  GetScreenshotResponse GetScreenshot() {
    GetScreenshotResponse out_response;
    bool has_out_response = false;
    data_provider_->GetScreenshot(ImageEncoding::PNG, [&out_response, &has_out_response](
                                                          std::unique_ptr<Screenshot> screenshot) {
      out_response.screenshot = std::move(screenshot);
      has_out_response = true;
    });
    RunLoopUntil([&has_out_response] { return has_out_response; });
    return out_response;
  }

  fit::result<Data, zx_status_t> GetData() {
    fit::result<Data, zx_status_t> out_result;
    bool has_out_result = false;
    data_provider_->GetData([&out_result, &has_out_result](fit::result<Data, zx_status_t> result) {
      out_result = std::move(result);
      has_out_result = true;
    });
    RunLoopUntil([&has_out_result] { return has_out_result; });
    return out_result;
  }

  void UnpackAttachmentBundle(const Data& data, std::vector<Attachment>* unpacked_attachments) {
    ASSERT_TRUE(data.has_attachment_bundle());
    const auto& attachment_bundle = data.attachment_bundle();
    EXPECT_STREQ(attachment_bundle.key.c_str(), kAttachmentBundle);
    ASSERT_TRUE(Unpack(attachment_bundle.value, unpacked_attachments));
    EXPECT_EQ(unpacked_attachments->size(), data.attachments().size());
  }

  uint64_t total_num_scenic_bindings() { return stub_scenic_->total_num_bindings(); }
  size_t current_num_scenic_bindings() { return stub_scenic_->current_num_bindings(); }
  const std::vector<TakeScreenshotResponse>& get_scenic_responses() const {
    return stub_scenic_->take_screenshot_responses();
  }

  std::unique_ptr<DataProviderImpl> data_provider_;

 private:
  ::sys::testing::ServiceDirectoryProvider service_directory_provider_;
  std::unique_ptr<::sys::testing::EnclosingEnvironment> environment_;
  fuchsia::sys::ComponentControllerPtr controller_;

  std::unique_ptr<StubScenic> stub_scenic_;
  std::unique_ptr<StubLogger> stub_logger_;
  std::unique_ptr<StubChannelProvider> stub_channel_provider_;
};

TEST_F(DataProviderImplTest, GetScreenshot_SucceedOnScenicReturningSuccess) {
  const size_t image_dim_in_px = 100;
  std::vector<TakeScreenshotResponse> scenic_responses;
  scenic_responses.emplace_back(CreateCheckerboardScreenshot(image_dim_in_px), kSuccess);
  std::unique_ptr<StubScenic> stub_scenic = std::make_unique<StubScenic>();
  stub_scenic->set_take_screenshot_responses(std::move(scenic_responses));
  SetUpScenic(std::move(stub_scenic));

  GetScreenshotResponse feedback_response = GetScreenshot();

  EXPECT_TRUE(get_scenic_responses().empty());

  ASSERT_NE(feedback_response.screenshot, nullptr);
  EXPECT_EQ(static_cast<size_t>(feedback_response.screenshot->dimensions_in_px.height),
            image_dim_in_px);
  EXPECT_EQ(static_cast<size_t>(feedback_response.screenshot->dimensions_in_px.width),
            image_dim_in_px);
  EXPECT_TRUE(feedback_response.screenshot->image.vmo.is_valid());

  fsl::SizedVmo expected_sized_vmo;
  ASSERT_TRUE(fsl::VmoFromFilename("/pkg/data/checkerboard_100.png", &expected_sized_vmo));
  std::vector<uint8_t> expected_pixels;
  ASSERT_TRUE(fsl::VectorFromVmo(expected_sized_vmo, &expected_pixels));
  std::vector<uint8_t> actual_pixels;
  ASSERT_TRUE(fsl::VectorFromVmo(feedback_response.screenshot->image, &actual_pixels));
  EXPECT_EQ(actual_pixels, expected_pixels);
}

TEST_F(DataProviderImplTest, GetScreenshot_FailOnScenicNotAvailable) {
  SetUpScenic(nullptr);

  GetScreenshotResponse feedback_response = GetScreenshot();

  EXPECT_EQ(feedback_response.screenshot, nullptr);
}

TEST_F(DataProviderImplTest, GetScreenshot_FailOnScenicReturningFailure) {
  std::vector<TakeScreenshotResponse> scenic_responses;
  scenic_responses.emplace_back(CreateEmptyScreenshot(), kFailure);
  std::unique_ptr<StubScenic> stub_scenic = std::make_unique<StubScenic>();
  stub_scenic->set_take_screenshot_responses(std::move(scenic_responses));
  SetUpScenic(std::move(stub_scenic));

  GetScreenshotResponse feedback_response = GetScreenshot();

  EXPECT_TRUE(get_scenic_responses().empty());

  EXPECT_EQ(feedback_response.screenshot, nullptr);
}

TEST_F(DataProviderImplTest, GetScreenshot_FailOnScenicReturningNonBGRA8Screenshot) {
  std::vector<TakeScreenshotResponse> scenic_responses;
  scenic_responses.emplace_back(CreateNonBGRA8Screenshot(), kSuccess);
  std::unique_ptr<StubScenic> stub_scenic = std::make_unique<StubScenic>();
  stub_scenic->set_take_screenshot_responses(std::move(scenic_responses));
  SetUpScenic(std::move(stub_scenic));

  GetScreenshotResponse feedback_response = GetScreenshot();

  EXPECT_TRUE(get_scenic_responses().empty());

  EXPECT_EQ(feedback_response.screenshot, nullptr);
}

TEST_F(DataProviderImplTest, GetScreenshot_ParallelRequests) {
  // We simulate three calls to DataProviderImpl::GetScreenshot(): one for which the stub Scenic
  // will return a checkerboard 10x10, one for a 20x20 and one failure.
  const size_t num_calls = 3u;
  const size_t image_dim_in_px_0 = 10u;
  const size_t image_dim_in_px_1 = 20u;
  std::vector<TakeScreenshotResponse> scenic_responses;
  scenic_responses.emplace_back(CreateCheckerboardScreenshot(image_dim_in_px_0), kSuccess);
  scenic_responses.emplace_back(CreateCheckerboardScreenshot(image_dim_in_px_1), kSuccess);
  scenic_responses.emplace_back(CreateEmptyScreenshot(), kFailure);
  ASSERT_EQ(scenic_responses.size(), num_calls);
  std::unique_ptr<StubScenic> stub_scenic = std::make_unique<StubScenic>();
  stub_scenic->set_take_screenshot_responses(std::move(scenic_responses));
  SetUpScenic(std::move(stub_scenic));

  std::vector<GetScreenshotResponse> feedback_responses;
  for (size_t i = 0; i < num_calls; i++) {
    data_provider_->GetScreenshot(ImageEncoding::PNG,
                                  [&feedback_responses](std::unique_ptr<Screenshot> screenshot) {
                                    feedback_responses.push_back({std::move(screenshot)});
                                  });
  }
  RunLoopUntil([&feedback_responses] { return feedback_responses.size() == num_calls; });

  EXPECT_TRUE(get_scenic_responses().empty());

  // We cannot assume that the order of the DataProviderImpl::GetScreenshot() calls match the order
  // of the Scenic::TakeScreenshot() callbacks because of the async message loop. Thus we need to
  // match them as sets.
  //
  // We set the expectations in advance and then pass a reference to the gMock matcher using
  // testing::ByRef() because the underlying VMO is not copyable.
  const GetScreenshotResponse expected_0 = {MakeUniqueScreenshot(image_dim_in_px_0)};
  const GetScreenshotResponse expected_1 = {MakeUniqueScreenshot(image_dim_in_px_1)};
  const GetScreenshotResponse expected_2 = {nullptr};
  EXPECT_THAT(feedback_responses, testing::UnorderedElementsAreArray({
                                      MatchesGetScreenshotResponse(testing::ByRef(expected_0)),
                                      MatchesGetScreenshotResponse(testing::ByRef(expected_1)),
                                      MatchesGetScreenshotResponse(testing::ByRef(expected_2)),
                                  }));

  // Additionally, we check that in the non-empty responses, the VMO is valid.
  for (const auto& response : feedback_responses) {
    if (response.screenshot == nullptr) {
      continue;
    }
    EXPECT_TRUE(response.screenshot->image.vmo.is_valid());
    EXPECT_GE(response.screenshot->image.size, 0u);
  }
}

TEST_F(DataProviderImplTest, GetScreenshot_OneScenicConnectionPerGetScreenshotCall) {
  // We use a stub that always returns false as we are not interested in the responses.
  SetUpScenic(std::make_unique<StubScenicAlwaysReturnsFalse>());

  const size_t num_calls = 5u;
  std::vector<GetScreenshotResponse> feedback_responses;
  for (size_t i = 0; i < num_calls; i++) {
    data_provider_->GetScreenshot(ImageEncoding::PNG,
                                  [&feedback_responses](std::unique_ptr<Screenshot> screenshot) {
                                    feedback_responses.push_back({std::move(screenshot)});
                                  });
  }
  RunLoopUntil([&feedback_responses] { return feedback_responses.size() == num_calls; });

  EXPECT_EQ(total_num_scenic_bindings(), num_calls);
  // The unbinding is asynchronous so we need to run the loop until all the outstanding connections
  // are actually close in the stub.
  RunLoopUntil([this] { return current_num_scenic_bindings() == 0u; });
}

TEST_F(DataProviderImplTest, GetData_SmokeTest) {
  fit::result<Data, zx_status_t> result = GetData();

  ASSERT_TRUE(result.is_ok());

  // There is not much we can assert here as no missing annotation nor attachment is fatal and we
  // cannot expect annotations or attachments to be present.

  const Data& data = result.value();

  // If there are annotations, there should be at least one attachment.
  if (data.has_annotations()) {
    ASSERT_TRUE(data.has_attachments());
  }

  // If there are attachments, there should be an attachment bundle with the same number of
  // attachments once unpacked.
  if (data.has_attachments()) {
    std::vector<Attachment> unpacked_attachments;
    UnpackAttachmentBundle(data, &unpacked_attachments);
  }
}

TEST_F(DataProviderImplTest, GetData_AnnotationsAsAttachment) {
  fit::result<Data, zx_status_t> result = GetData();

  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();

  // There should be an "annotations.json" attachment.
  ASSERT_TRUE(data.has_attachments());
  bool found_annotations_attachment = false;
  std::string annotations_json;
  for (const auto& attachment : data.attachments()) {
    if (attachment.key != kAttachmentAnnotations) {
      continue;
    }
    found_annotations_attachment = true;

    ASSERT_TRUE(fsl::StringFromVmo(attachment.value, &annotations_json));
    ASSERT_FALSE(annotations_json.empty());

    // JSON verification.
    // We check that the output is a valid JSON and that it matches the schema.
    rapidjson::Document json;
    ASSERT_FALSE(json.Parse(annotations_json.c_str()).HasParseError());
    rapidjson::Document schema_json;
    ASSERT_FALSE(schema_json
                     .Parse(fxl::Substitute(R"({
  "type": "object",
  "properties": {
    "$0": {
      "type": "string"
    },
    "$1": {
      "type": "string"
    },
    "$2": {
      "type": "string"
    },
    "$3": {
      "type": "string"
    },
    "$4": {
      "type": "string"
    },
    "$5": {
      "type": "string"
    },
    "$6": {
      "type": "string"
    }
  },
  "additionalProperties": false
})",
                                            kAnnotationBuildBoard, kAnnotationBuildLatestCommitDate,
                                            kAnnotationBuildProduct, kAnnotationBuildVersion,
                                            kAnnotationChannel, kAnnotationDeviceBoardName,
                                            kAnnotationDeviceUptime))
                     .HasParseError());
    rapidjson::SchemaDocument schema(schema_json);
    rapidjson::SchemaValidator validator(schema);
    EXPECT_TRUE(json.Accept(validator));
  }
  EXPECT_TRUE(found_annotations_attachment);

  // That same "annotations.json" attachment should be present in the attachment bundle.
  std::vector<Attachment> unpacked_attachments;
  UnpackAttachmentBundle(data, &unpacked_attachments);
  EXPECT_THAT(unpacked_attachments,
              testing::Contains(MatchesAttachment(kAttachmentAnnotations, annotations_json)));
}

TEST_F(DataProviderImplTest, GetData_SysLog) {
  // CollectSystemLogs() has its own set of unit tests so we only cover one log message here to
  // check that we are attaching the logs.
  SetUpLogger({
      BuildLogMessage(FX_LOG_INFO, "log message",
                      /*timestamp_offset=*/zx::duration(0), {"foo"}),
  });
  const std::string expected_syslog = "[15604.000][07559][07687][foo] INFO: log message\n";

  fit::result<Data, zx_status_t> result = GetData();

  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();

  // There should be a "log.system.txt" attachment.
  ASSERT_TRUE(data.has_attachments());
  EXPECT_THAT(data.attachments(),
              testing::Contains(MatchesAttachment(kAttachmentLogSystem, expected_syslog)));

  // That same "log.system.txt" attachment should be present in the attachment bundle.
  std::vector<Attachment> unpacked_attachments;
  UnpackAttachmentBundle(data, &unpacked_attachments);
  EXPECT_THAT(unpacked_attachments,
              testing::Contains(MatchesAttachment(kAttachmentLogSystem, expected_syslog)));
}

constexpr char kInspectJsonSchema[] = R"({
  "type": "array",
  "items": {
    "type": "object",
    "properties": {
      "path": {
        "type": "string"
      },
      "contents": {
        "type": "object"
      }
    },
    "required": [
      "path",
      "contents"
    ],
    "additionalProperties": false
  },
  "uniqueItems": true
})";

TEST_F(DataProviderImplTest, GetData_Inspect) {
  InjectInspectTestApp();

  fit::result<Data, zx_status_t> result = GetData();

  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();

  // There should be an "inspect.json" attachment.
  ASSERT_TRUE(data.has_attachments());
  bool found_inspect_attachment = false;
  std::string inspect_json;
  for (const auto& attachment : data.attachments()) {
    if (attachment.key != kAttachmentInspect) {
      continue;
    }
    found_inspect_attachment = true;

    ASSERT_TRUE(fsl::StringFromVmo(attachment.value, &inspect_json));
    ASSERT_FALSE(inspect_json.empty());

    // JSON verification.
    // We check that the output is a valid JSON and that it matches the schema.
    rapidjson::Document json;
    ASSERT_FALSE(json.Parse(inspect_json.c_str()).HasParseError());
    rapidjson::Document schema_json;
    ASSERT_FALSE(schema_json.Parse(kInspectJsonSchema).HasParseError());
    rapidjson::SchemaDocument schema(schema_json);
    rapidjson::SchemaValidator validator(schema);
    EXPECT_TRUE(json.Accept(validator));

    // We then check that we get the expected Inspect data for the injected test app.
    bool has_entry_for_test_app = false;
    for (const auto& obj : json.GetArray()) {
      const std::string path = obj["path"].GetString();
      if (path.find("inspect_test_app.cmx") != std::string::npos) {
        has_entry_for_test_app = true;
        const auto contents = obj["contents"].GetObject();
        ASSERT_TRUE(contents.HasMember("root"));
        const auto root = contents["root"].GetObject();
        ASSERT_TRUE(root.HasMember("obj1"));
        ASSERT_TRUE(root.HasMember("obj2"));
        const auto obj1 = root["obj1"].GetObject();
        const auto obj2 = root["obj2"].GetObject();
        ASSERT_TRUE(obj1.HasMember("version"));
        ASSERT_TRUE(obj2.HasMember("version"));
        EXPECT_STREQ(obj1["version"].GetString(), "1.0");
        EXPECT_STREQ(obj2["version"].GetString(), "1.0");
        ASSERT_TRUE(obj1.HasMember("value"));
        ASSERT_TRUE(obj2.HasMember("value"));
        EXPECT_EQ(obj1["value"].GetUint(), 100u);
        EXPECT_EQ(obj2["value"].GetUint(), 200u);
      }
    }
    EXPECT_TRUE(has_entry_for_test_app);
  }
  EXPECT_TRUE(found_inspect_attachment);

  // That same "inspect.json" attachment should be present in the attachment bundle.
  std::vector<Attachment> unpacked_attachments;
  UnpackAttachmentBundle(data, &unpacked_attachments);
  EXPECT_THAT(unpacked_attachments,
              testing::Contains(MatchesAttachment(kAttachmentInspect, inspect_json)));
}

TEST_F(DataProviderImplTest, GetData_Channel) {
  SetUpChannelProvider("my-channel");

  fit::result<Data, zx_status_t> result = GetData();

  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();
  ASSERT_TRUE(data.has_annotations());
  EXPECT_THAT(data.annotations(),
              testing::Contains(MatchesAnnotation(kAnnotationChannel, "my-channel")));
}

TEST_F(DataProviderImplTest, GetData_Uptime) {
  fit::result<Data, zx_status_t> result = GetData();

  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();
  ASSERT_TRUE(data.has_annotations());
  EXPECT_THAT(data.annotations(), testing::Contains(MatchesKey(kAnnotationDeviceUptime)));
}

TEST_F(DataProviderImplTest, GetData_EmptyAnnotationAllowlist) {
  SetUpDataProvider(Config{/*annotation_allowlist=*/{}, kDefaultAttachments});

  fit::result<Data, zx_status_t> result = GetData();
  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();
  EXPECT_FALSE(data.has_annotations());
}

TEST_F(DataProviderImplTest, GetData_EmptyAttachmentAllowlist) {
  SetUpDataProvider(Config{kDefaultAnnotations, /*attachment_allowlist=*/{}});

  fit::result<Data, zx_status_t> result = GetData();
  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();
  EXPECT_TRUE(data.has_attachments());
  ASSERT_EQ(data.attachments().size(), 1u);
  EXPECT_STREQ(data.attachments()[0].key.c_str(), kAttachmentAnnotations);
  std::vector<Attachment> unpacked_attachments;
  UnpackAttachmentBundle(data, &unpacked_attachments);
  EXPECT_THAT(unpacked_attachments, testing::Contains(MatchesKey(kAttachmentAnnotations)));
}

TEST_F(DataProviderImplTest, GetData_EmptyAllowlists) {
  SetUpDataProvider(Config{/*annotation_allowlist=*/{}, /*attachment_allowlist=*/{}});

  fit::result<Data, zx_status_t> result = GetData();
  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();
  EXPECT_FALSE(data.has_annotations());
  EXPECT_FALSE(data.has_attachments());
  EXPECT_FALSE(data.has_attachment_bundle());
}

TEST_F(DataProviderImplTest, GetData_UnknownAllowlistedAnnotation) {
  SetUpDataProvider(Config{/*annotation_allowlist=*/{"unknown.annotation"}, kDefaultAttachments});

  fit::result<Data, zx_status_t> result = GetData();
  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();
  EXPECT_FALSE(data.has_annotations());
}

TEST_F(DataProviderImplTest, GetData_UnknownAllowlistedAttachment) {
  SetUpDataProvider(Config{kDefaultAnnotations,
                           /*attachment_allowlist=*/{"unknown.attachment"}});

  fit::result<Data, zx_status_t> result = GetData();
  ASSERT_TRUE(result.is_ok());

  const Data& data = result.value();
  EXPECT_TRUE(data.has_attachments());
  ASSERT_EQ(data.attachments().size(), 1u);
  EXPECT_STREQ(data.attachments()[0].key.c_str(), kAttachmentAnnotations);
  std::vector<Attachment> unpacked_attachments;
  UnpackAttachmentBundle(data, &unpacked_attachments);
  EXPECT_THAT(unpacked_attachments, testing::Contains(MatchesKey(kAttachmentAnnotations)));
}

}  // namespace
}  // namespace feedback

int main(int argc, char** argv) {
  if (!fxl::SetTestSettings(argc, argv)) {
    return EXIT_FAILURE;
  }

  testing::InitGoogleTest(&argc, argv);
  syslog::InitLogger({"feedback", "test"});
  return RUN_ALL_TESTS();
}
