// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zxtest/zxtest.h>

#include "assert_strstr.h"
#include "error_test.h"
#include "test_library.h"

namespace {

TEST(ErrorsTests, GoodError) {
  TestLibrary library(R"FIDL(
library example;

protocol Example {
    Method() -> (string foo) error int32;
};

)FIDL");

  ASSERT_TRUE(library.Compile());

  auto methods = &library.LookupProtocol("Example")->methods;
  ASSERT_EQ(methods->size(), 1);
  auto method = &methods->at(0);
  auto response = method->maybe_response;
  ASSERT_NOT_NULL(response);
  ASSERT_EQ(response->members.size(), 1);
  auto response_member = &response->members.at(0);
  ASSERT_EQ(response_member->type_ctor->type->kind, fidl::flat::Type::Kind::kIdentifier);
  auto result_identifier =
      static_cast<const fidl::flat::IdentifierType*>(response_member->type_ctor->type);
  const fidl::flat::Union* result_union =
      library.LookupUnion(std::string(result_identifier->name.decl_name()));
  ASSERT_NOT_NULL(result_union);
  ASSERT_NOT_NULL(result_union->attributes);
  ASSERT_TRUE(result_union->attributes->HasAttribute("Result"));
  ASSERT_EQ(result_union->members.size(), 2);

  const auto& success = result_union->members.at(0);
  ASSERT_NOT_NULL(success.maybe_used);
  ASSERT_STR_EQ("response", std::string(success.maybe_used->name.data()).c_str());

  const fidl::flat::Union::Member& error = result_union->members.at(1);
  ASSERT_NOT_NULL(error.maybe_used);
  ASSERT_STR_EQ("err", std::string(error.maybe_used->name.data()).c_str());

  ASSERT_NOT_NULL(error.maybe_used->type_ctor->type);
  ASSERT_EQ(error.maybe_used->type_ctor->type->kind, fidl::flat::Type::Kind::kPrimitive);
  auto primitive_type =
      static_cast<const fidl::flat::PrimitiveType*>(error.maybe_used->type_ctor->type);
  ASSERT_EQ(primitive_type->subtype, fidl::types::PrimitiveSubtype::kInt32);
}

TEST(ErrorsTests, GoodErrorUnsigned) {
  TestLibrary library(R"FIDL(
library example;

protocol Example {
    Method() -> (string foo) error uint32;
};

)FIDL");

  ASSERT_TRUE(library.Compile());
}

TEST(ErrorsTests, GoodErrorEnum) {
  TestLibrary library(R"FIDL(
library example;

enum ErrorType : int32 {
    GOOD = 1;
    BAD = 2;
    UGLY = 3;
};

protocol Example {
    Method() -> (string foo) error ErrorType;
};

)FIDL");

  ASSERT_TRUE(library.Compile());
}

TEST(ErrorsTests, GoodErrorEnumAfter) {
  TestLibrary library(R"FIDL(
library example;

protocol Example {
    Method() -> (string foo) error ErrorType;
};

enum ErrorType : int32 {
    GOOD = 1;
    BAD = 2;
    UGLY = 3;
};

)FIDL");

  ASSERT_TRUE(library.Compile());
}

TEST(ErrorsTests, BadErrorUnknownIdentifier) {
  TestLibrary library(R"FIDL(
library example;

protocol Example {
    Method() -> (string foo) error ErrorType;
};
)FIDL");

  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnknownType);
  ASSERT_STR_STR(errors[0]->msg.c_str(), "ErrorType");
}

TEST(ErrorsTests, BadErrorWrongPrimitive) {
  TestLibrary library(R"FIDL(
library example;

protocol Example {
    Method() -> (string foo) error float32;
};
)FIDL");

  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrInvalidErrorType);
}

TEST(ErrorsTests, BadErrorMissingType) {
  TestLibrary library(R"FIDL(
library example;
protocol Example {
    Method() -> (int32 flub) error;
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
}

TEST(ErrorsTests, BadErrorNotAType) {
  TestLibrary library(R"FIDL(
library example;
protocol Example {
    Method() -> (int32 flub) error "hello";
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
}

TEST(ErrorsTests, BadErrorNoResponse) {
  TestLibrary library(R"FIDL(
library example;
protocol Example {
    Method() -> error int32;
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
}

TEST(ErrorsTests, BadErrorUnexpectedEndOfFile) {
  TestLibrary library(R"FIDL(
library example;
table ForgotTheSemicolon {}
)FIDL");

  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_GE(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
}

TEST(ErrorsTests, BadErrorEmptyFile) {
  TestLibrary library("");

  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_GE(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedIdentifier);
}
}  // namespace
