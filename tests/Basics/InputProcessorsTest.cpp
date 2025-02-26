////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "Basics/InputProcessors.h"

#include "gtest/gtest.h"

using namespace arangodb;

TEST(InputProcessorsTest, testEmpty) {
  {
    std::string empty;
    InputProcessorJSONL proc(empty);
    ASSERT_FALSE(proc.valid());
  }

  {
    std::string empty("\n");
    InputProcessorJSONL proc(empty);
    ASSERT_FALSE(proc.valid());
  }

  {
    std::string empty("\n\n\n");
    InputProcessorJSONL proc(empty);
    ASSERT_FALSE(proc.valid());
  }
}

#ifndef _WIN32
TEST(InputProcessorsTest, testNonEmpty) {
  char const* data =
#include "InputProcessorsData.json"
      ;

  int rowsFound = 0;
  InputProcessorJSONL proc(data);
  while (proc.valid()) {
    auto slice = proc.value();
    ASSERT_TRUE(slice.isObject());
    ++rowsFound;
  }

  ASSERT_EQ(202, rowsFound);
}
#endif
