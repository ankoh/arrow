// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "arrow/array.h"
#include "arrow/array/builder_binary.h"
#include "arrow/compute/cast.h"
#include "arrow/compute/exec/options.h"
#include "arrow/compute/exec/util.h"
#include "arrow/engine/substrait/extension_set.h"
#include "arrow/engine/substrait/plan_internal.h"
#include "arrow/engine/substrait/serde.h"
#include "arrow/engine/substrait/test_plan_builder.h"
#include "arrow/engine/substrait/type_internal.h"
#include "arrow/record_batch.h"
#include "arrow/table.h"
#include "arrow/testing/future_util.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/type.h"

namespace arrow {

namespace engine {
struct FunctionTestCase {
  Id function_id;
  std::vector<std::string> arguments;
  std::vector<std::shared_ptr<DataType>> data_types;
  // For a test case that should fail just use the empty string
  std::string expected_output;
  std::shared_ptr<DataType> expected_output_type;
};

Result<std::shared_ptr<Array>> GetArray(const std::string& value,
                                        const std::shared_ptr<DataType>& data_type) {
  StringBuilder str_builder;
  if (value.empty()) {
    ARROW_EXPECT_OK(str_builder.AppendNull());
  } else {
    ARROW_EXPECT_OK(str_builder.Append(value));
  }
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Array> value_str, str_builder.Finish());
  ARROW_ASSIGN_OR_RAISE(Datum value_datum, compute::Cast(value_str, data_type));
  return value_datum.make_array();
}

Result<std::shared_ptr<Table>> GetInputTable(
    const std::vector<std::string>& arguments,
    const std::vector<std::shared_ptr<DataType>>& data_types) {
  std::vector<std::shared_ptr<Array>> columns;
  std::vector<std::shared_ptr<Field>> fields;
  EXPECT_EQ(arguments.size(), data_types.size());
  for (std::size_t i = 0; i < arguments.size(); i++) {
    if (data_types[i]) {
      ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Array> arg_array,
                            GetArray(arguments[i], data_types[i]));
      columns.push_back(std::move(arg_array));
      fields.push_back(field("arg_" + std::to_string(i), data_types[i]));
    }
  }
  std::shared_ptr<RecordBatch> batch =
      RecordBatch::Make(schema(std::move(fields)), 1, columns);
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Table> table, Table::FromRecordBatches({batch}));
  return table;
}

Result<std::shared_ptr<Table>> GetOutputTable(
    const std::string& output_value, const std::shared_ptr<DataType>& output_type) {
  std::vector<std::shared_ptr<Array>> columns(1);
  std::vector<std::shared_ptr<Field>> fields(1);
  ARROW_ASSIGN_OR_RAISE(columns[0], GetArray(output_value, output_type));
  fields[0] = field("output", output_type);
  std::shared_ptr<RecordBatch> batch =
      RecordBatch::Make(schema(std::move(fields)), 1, columns);
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Table> table, Table::FromRecordBatches({batch}));
  return table;
}

Result<std::shared_ptr<compute::ExecPlan>> PlanFromTestCase(
    const FunctionTestCase& test_case, std::shared_ptr<Table>* output_table) {
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Table> input_table,
                        GetInputTable(test_case.arguments, test_case.data_types));
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Buffer> substrait,
                        internal::CreateScanProjectSubstrait(
                            test_case.function_id, input_table, test_case.arguments,
                            test_case.data_types, *test_case.expected_output_type));
  std::shared_ptr<compute::SinkNodeConsumer> consumer =
      std::make_shared<compute::TableSinkNodeConsumer>(output_table,
                                                       default_memory_pool());

  // Mock table provider that ignores the table name and returns input_table
  NamedTableProvider table_provider = [input_table](const std::vector<std::string>&) {
    std::shared_ptr<compute::ExecNodeOptions> options =
        std::make_shared<compute::TableSourceNodeOptions>(input_table);
    return compute::Declaration("table_source", {}, options, "mock_source");
  };

  ConversionOptions conversion_options;
  conversion_options.named_table_provider = std::move(table_provider);

  ARROW_ASSIGN_OR_RAISE(
      std::shared_ptr<compute::ExecPlan> plan,
      DeserializePlan(*substrait, std::move(consumer), default_extension_id_registry(),
                      /*ext_set_out=*/nullptr, conversion_options));
  return plan;
}

void CheckValidTestCases(const std::vector<FunctionTestCase>& valid_cases) {
  for (const FunctionTestCase& test_case : valid_cases) {
    std::shared_ptr<Table> output_table;
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<compute::ExecPlan> plan,
                         PlanFromTestCase(test_case, &output_table));
    ASSERT_OK(plan->StartProducing());
    ASSERT_FINISHES_OK(plan->finished());

    // Could also modify the Substrait plan with an emit to drop the leading columns
    int result_column = output_table->num_columns() - 1;  // last column holds result
    ASSERT_OK_AND_ASSIGN(output_table, output_table->SelectColumns({result_column}));

    ASSERT_OK_AND_ASSIGN(
        std::shared_ptr<Table> expected_output,
        GetOutputTable(test_case.expected_output, test_case.expected_output_type));
    AssertTablesEqual(*expected_output, *output_table, /*same_chunk_layout=*/false);
  }
}

void CheckErrorTestCases(const std::vector<FunctionTestCase>& error_cases) {
  for (const FunctionTestCase& test_case : error_cases) {
    std::shared_ptr<Table> output_table;
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<compute::ExecPlan> plan,
                         PlanFromTestCase(test_case, &output_table));
    ASSERT_RAISES(Invalid, plan->StartProducing());
  }
}

// These are not meant to be an exhaustive test of Substrait
// conformance.  Instead, we should test just enough to ensure
// we are mapping to the correct function
TEST(FunctionMapping, ValidCases) {
  const std::vector<FunctionTestCase> valid_test_cases = {
      {{kSubstraitArithmeticFunctionsUri, "add"},
       {"SILENT", "127", "10"},
       {nullptr, int8(), int8()},
       "-119",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "subtract"},
       {"SILENT", "-119", "10"},
       {nullptr, int8(), int8()},
       "127",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "multiply"},
       {"SILENT", "10", "13"},
       {nullptr, int8(), int8()},
       "-126",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "divide"},
       {"SILENT", "-128", "-1"},
       {nullptr, int8(), int8()},
       "0",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "sign"}, {"-1"}, {int8()}, "-1", int8()},
      {{kSubstraitArithmeticFunctionsUri, "power"},
       {"SILENT", "2", "2"},
       {nullptr, int8(), int8()},
       "4",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "sqrt"},
       {"SILENT", "4"},
       {nullptr, int8()},
       "2",
       float64()},
      {{kSubstraitArithmeticFunctionsUri, "exp"},
       {"1"},
       {float64()},
       "2.718281828459045",
       float64()},
      {{kSubstraitArithmeticFunctionsUri, "abs"},
       {"SILENT", "-1"},
       {nullptr, int8()},
       "1",
       int8()},
      {{kSubstraitBooleanFunctionsUri, "or"},
       {"1", ""},
       {boolean(), boolean()},
       "1",
       boolean()},
      {{kSubstraitBooleanFunctionsUri, "and"},
       {"1", ""},
       {boolean(), boolean()},
       "",
       boolean()},
      {{kSubstraitBooleanFunctionsUri, "xor"},
       {"1", "1"},
       {boolean(), boolean()},
       "0",
       boolean()},
      {{kSubstraitBooleanFunctionsUri, "not"}, {"1"}, {boolean()}, "0", boolean()},
      {{kSubstraitComparisonFunctionsUri, "equal"},
       {"57", "57"},
       {int8(), int8()},
       "1",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "is_null"}, {"abc"}, {utf8()}, "0", boolean()},
      {{kSubstraitComparisonFunctionsUri, "is_not_null"},
       {"57"},
       {int8()},
       "1",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "not_equal"},
       {"57", "57"},
       {int8(), int8()},
       "0",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "lt"},
       {"57", "80"},
       {int8(), int8()},
       "1",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "lt"},
       {"57", "57"},
       {int8(), int8()},
       "0",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "gt"},
       {"57", "30"},
       {int8(), int8()},
       "1",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "gt"},
       {"57", "57"},
       {int8(), int8()},
       "0",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "lte"},
       {"57", "57"},
       {int8(), int8()},
       "1",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "lte"},
       {"50", "57"},
       {int8(), int8()},
       "1",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "gte"},
       {"57", "57"},
       {int8(), int8()},
       "1",
       boolean()},
      {{kSubstraitComparisonFunctionsUri, "gte"},
       {"60", "57"},
       {int8(), int8()},
       "1",
       boolean()},
      {{kSubstraitDatetimeFunctionsUri, "extract"},
       {"YEAR", "2022-07-15T14:33:14"},
       {nullptr, timestamp(TimeUnit::MICRO)},
       "2022",
       int64()},
      {{kSubstraitDatetimeFunctionsUri, "extract"},
       {"MONTH", "2022-07-15T14:33:14"},
       {nullptr, timestamp(TimeUnit::MICRO)},
       "7",
       int64()},
      {{kSubstraitDatetimeFunctionsUri, "extract"},
       {"DAY", "2022-07-15T14:33:14"},
       {nullptr, timestamp(TimeUnit::MICRO)},
       "15",
       int64()},
      {{kSubstraitDatetimeFunctionsUri, "extract"},
       {"SECOND", "2022-07-15T14:33:14"},
       {nullptr, timestamp(TimeUnit::MICRO)},
       "14",
       int64()},
      {{kSubstraitDatetimeFunctionsUri, "extract"},
       {"YEAR", "2022-07-15T14:33:14Z"},
       {nullptr, timestamp(TimeUnit::MICRO, "UTC")},
       "2022",
       int64()},
      {{kSubstraitDatetimeFunctionsUri, "extract"},
       {"MONTH", "2022-07-15T14:33:14Z"},
       {nullptr, timestamp(TimeUnit::MICRO, "UTC")},
       "7",
       int64()},
      {{kSubstraitDatetimeFunctionsUri, "extract"},
       {"DAY", "2022-07-15T14:33:14Z"},
       {nullptr, timestamp(TimeUnit::MICRO, "UTC")},
       "15",
       int64()},
      {{kSubstraitDatetimeFunctionsUri, "extract"},
       {"SECOND", "2022-07-15T14:33:14Z"},
       {nullptr, timestamp(TimeUnit::MICRO, "UTC")},
       "14",
       int64()},
      {{kSubstraitDatetimeFunctionsUri, "lt"},
       {"2022-07-15T14:33:14", "2022-07-15T14:33:20"},
       {timestamp(TimeUnit::MICRO), timestamp(TimeUnit::MICRO)},
       "1",
       boolean()},
      {{kSubstraitDatetimeFunctionsUri, "lte"},
       {"2022-07-15T14:33:14", "2022-07-15T14:33:14"},
       {timestamp(TimeUnit::MICRO), timestamp(TimeUnit::MICRO)},
       "1",
       boolean()},
      {{kSubstraitDatetimeFunctionsUri, "gt"},
       {"2022-07-15T14:33:30", "2022-07-15T14:33:14"},
       {timestamp(TimeUnit::MICRO), timestamp(TimeUnit::MICRO)},
       "1",
       boolean()},
      {{kSubstraitDatetimeFunctionsUri, "gte"},
       {"2022-07-15T14:33:14", "2022-07-15T14:33:14"},
       {timestamp(TimeUnit::MICRO), timestamp(TimeUnit::MICRO)},
       "1",
       boolean()},
      {{kSubstraitStringFunctionsUri, "concat"},
       {"abc", "def"},
       {utf8(), utf8()},
       "abcdef",
       utf8()},
      {{kSubstraitLogarithmicFunctionsUri, "ln"}, {"1"}, {int8()}, "0", float64()},
      {{kSubstraitLogarithmicFunctionsUri, "log10"}, {"10"}, {int8()}, "1", float64()},
      {{kSubstraitLogarithmicFunctionsUri, "log2"}, {"2"}, {int8()}, "1", float64()},
      {{kSubstraitLogarithmicFunctionsUri, "log1p"},
       {"1"},
       {int8()},
       "0.6931471805599453",
       float64()},
      {{kSubstraitLogarithmicFunctionsUri, "logb"},
       {"10", "10"},
       {int8(), int8()},
       "1",
       float64()},
      {{kSubstraitRoundingFunctionsUri, "floor"}, {"3.1"}, {float64()}, "3", float64()},
      {{kSubstraitRoundingFunctionsUri, "ceil"}, {"3.1"}, {float64()}, "4", float64()}};
  CheckValidTestCases(valid_test_cases);
}

TEST(FunctionMapping, ErrorCases) {
  const std::vector<FunctionTestCase> error_test_cases = {
      {{kSubstraitArithmeticFunctionsUri, "add"},
       {"ERROR", "127", "10"},
       {nullptr, int8(), int8()},
       "",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "subtract"},
       {"ERROR", "-119", "10"},
       {nullptr, int8(), int8()},
       "",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "multiply"},
       {"ERROR", "10", "13"},
       {nullptr, int8(), int8()},
       "",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "divide"},
       {"ERROR", "-128", "-1"},
       {nullptr, int8(), int8()},
       "",
       int8()}};
  CheckErrorTestCases(error_test_cases);
}

// For each aggregate test case we take in three values.  We compute the
// aggregate both on the entire set (all three values) and on groups.  The
// first two rows will be in the first group and the last row will be in the
// second group.  It's important to test both for coverage since the arrow
// function used actually changes when group ids are present
struct AggregateTestCase {
  // The substrait function id
  Id function_id;
  // The three values, as a JSON string
  std::string arguments;
  // The data type of the three values
  std::shared_ptr<DataType> data_type;
  // The result of the aggregate on all three
  std::string combined_output;
  // The result of the aggregate on each group (i.e. the first two rows
  // and the last row).  Should be a json-encoded array of size 2
  std::string group_outputs;
  // The data type of the outputs
  std::shared_ptr<DataType> output_type;
};

std::shared_ptr<Table> GetInputTableForAggregateCase(const AggregateTestCase& test_case) {
  std::vector<std::shared_ptr<Array>> columns(2);
  std::vector<std::shared_ptr<Field>> fields(2);
  columns[0] = ArrayFromJSON(int8(), "[1, 1, 2]");
  columns[1] = ArrayFromJSON(test_case.data_type, test_case.arguments);
  fields[0] = field("key", int8());
  fields[1] = field("value", test_case.data_type);
  std::shared_ptr<RecordBatch> batch =
      RecordBatch::Make(schema(std::move(fields)), /*num_rows=*/3, std::move(columns));
  EXPECT_OK_AND_ASSIGN(std::shared_ptr<Table> table, Table::FromRecordBatches({batch}));
  return table;
}

std::shared_ptr<Table> GetOutputTableForAggregateCase(
    const std::shared_ptr<DataType>& output_type, const std::string& json_data) {
  std::shared_ptr<Array> out_arr = ArrayFromJSON(output_type, json_data);
  std::shared_ptr<RecordBatch> batch =
      RecordBatch::Make(schema({field("", output_type)}), 1, {out_arr});
  EXPECT_OK_AND_ASSIGN(std::shared_ptr<Table> table, Table::FromRecordBatches({batch}));
  return table;
}

std::shared_ptr<compute::ExecPlan> PlanFromAggregateCase(
    const AggregateTestCase& test_case, std::shared_ptr<Table>* output_table,
    bool with_keys) {
  std::shared_ptr<Table> input_table = GetInputTableForAggregateCase(test_case);
  std::vector<int> key_idxs = {};
  if (with_keys) {
    key_idxs = {0};
  }
  EXPECT_OK_AND_ASSIGN(
      std::shared_ptr<Buffer> substrait,
      internal::CreateScanAggSubstrait(test_case.function_id, input_table, key_idxs,
                                       /*arg_idx=*/1, *test_case.output_type));
  std::shared_ptr<compute::SinkNodeConsumer> consumer =
      std::make_shared<compute::TableSinkNodeConsumer>(output_table,
                                                       default_memory_pool());

  // Mock table provider that ignores the table name and returns input_table
  NamedTableProvider table_provider = [input_table](const std::vector<std::string>&) {
    std::shared_ptr<compute::ExecNodeOptions> options =
        std::make_shared<compute::TableSourceNodeOptions>(input_table);
    return compute::Declaration("table_source", {}, options, "mock_source");
  };

  ConversionOptions conversion_options;
  conversion_options.named_table_provider = std::move(table_provider);

  EXPECT_OK_AND_ASSIGN(
      std::shared_ptr<compute::ExecPlan> plan,
      DeserializePlan(*substrait, std::move(consumer), default_extension_id_registry(),
                      /*ext_set_out=*/nullptr, conversion_options));
  return plan;
}

void CheckWholeAggregateCase(const AggregateTestCase& test_case) {
  std::shared_ptr<Table> output_table;
  std::shared_ptr<compute::ExecPlan> plan =
      PlanFromAggregateCase(test_case, &output_table, /*with_keys=*/false);

  ASSERT_OK(plan->StartProducing());
  ASSERT_FINISHES_OK(plan->finished());

  ASSERT_OK_AND_ASSIGN(output_table,
                       output_table->SelectColumns({output_table->num_columns() - 1}));

  std::shared_ptr<Table> expected_output =
      GetOutputTableForAggregateCase(test_case.output_type, test_case.combined_output);
  AssertTablesEqual(*expected_output, *output_table, /*same_chunk_layout=*/false);
}

void CheckGroupedAggregateCase(const AggregateTestCase& test_case) {
  std::shared_ptr<Table> output_table;
  std::shared_ptr<compute::ExecPlan> plan =
      PlanFromAggregateCase(test_case, &output_table, /*with_keys=*/true);

  ASSERT_OK(plan->StartProducing());
  ASSERT_FINISHES_OK(plan->finished());

  // The aggregate node's output is unpredictable so we sort by the key column
  ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Array> sort_indices,
      compute::SortIndices(output_table, compute::SortOptions({compute::SortKey(
                                             output_table->num_columns() - 1,
                                             compute::SortOrder::Ascending)})));
  ASSERT_OK_AND_ASSIGN(Datum sorted_table_datum,
                       compute::Take(output_table, sort_indices));
  output_table = sorted_table_datum.table();
  // TODO(ARROW-17245) We should be selecting N-1 here but Acero
  // currently emits things in reverse order
  ASSERT_OK_AND_ASSIGN(output_table, output_table->SelectColumns({0}));

  std::shared_ptr<Table> expected_output =
      GetOutputTableForAggregateCase(test_case.output_type, test_case.group_outputs);

  AssertTablesEqual(*expected_output, *output_table, /*same_chunk_layout=*/false);
}

void CheckAggregateCases(const std::vector<AggregateTestCase>& test_cases) {
  for (const AggregateTestCase& test_case : test_cases) {
    CheckWholeAggregateCase(test_case);
    CheckGroupedAggregateCase(test_case);
  }
}

TEST(FunctionMapping, AggregateCases) {
  const std::vector<AggregateTestCase> test_cases = {
      {{kSubstraitArithmeticFunctionsUri, "sum"},
       "[1, 2, 3]",
       int8(),
       "[6]",
       "[3, 3]",
       int64()},
      {{kSubstraitArithmeticFunctionsUri, "min"},
       "[1, 2, 3]",
       int8(),
       "[1]",
       "[1, 3]",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "max"},
       "[1, 2, 3]",
       int8(),
       "[3]",
       "[2, 3]",
       int8()},
      {{kSubstraitArithmeticFunctionsUri, "avg"},
       "[1, 2, 3]",
       float64(),
       "[2]",
       "[1.5, 3]",
       float64()},
      {{kSubstraitAggregateGenericFunctionsUri, "count"},
       {"[1, 2, 30]"},
       {int8()},
       "[3]",
       "[2, 1]",
       int64()}};
  CheckAggregateCases(test_cases);
}

}  // namespace engine
}  // namespace arrow
