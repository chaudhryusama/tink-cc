// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////////

#include "tink/internal/parameters_parser.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tink/internal/serialization.h"
#include "tink/parameters.h"
#include "tink/util/test_matchers.h"

namespace crypto {
namespace tink {
namespace internal {
namespace {

using ::crypto::tink::test::IsOk;
using ::testing::Eq;
using ::testing::IsFalse;

class ExampleParameters : public Parameters {
 public:
  bool HasIdRequirement() const override { return false; }

  bool operator==(const Parameters& other) const override { return true; }
};

class ExampleSerialization : public Serialization {
 public:
  absl::string_view ObjectIdentifier() const override {
    return "example_type_url";
  }
};

util::StatusOr<ExampleParameters> Parse(ExampleSerialization serialization) {
  return ExampleParameters();
}

TEST(ParametersParserTest, Create) {
  ParametersParser<ExampleSerialization, ExampleParameters> parser(
      "example_type_url", Parse);

  EXPECT_THAT(parser.ObjectIdentifier(), Eq("example_type_url"));
  EXPECT_THAT(parser.TypeIndex(),
              Eq(std::type_index(typeid(ExampleParameters))));
}

TEST(ParametersParserTest, ParseParameters) {
  ParametersParser<ExampleSerialization, ExampleParameters> parser(
      "example_type_url", Parse);

  util::StatusOr<ExampleParameters> params =
      parser.ParseParameters(ExampleSerialization());
  ASSERT_THAT(params, IsOk());
  EXPECT_THAT(params->HasIdRequirement(), IsFalse());
}

}  // namespace
}  // namespace internal
}  // namespace tink
}  // namespace crypto