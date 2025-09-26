// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "storage/lake/cdc_data_collector.h"

#include <gtest/gtest.h>
#include "storage/lake/tablet_metadata.h"
#include "storage/tablet_schema.h"

namespace starrocks::lake {

class CdcIntegrationTest : public ::testing::Test {
public:
    void SetUp() override {
        _cdc_collector = std::make_unique<CdcDataCollector>();
    }

protected:
    std::unique_ptr<CdcDataCollector> _cdc_collector;
};

TEST_F(CdcIntegrationTest, TestCdcCollectorBasics) {
    CdcTransactionCollector txn_collector(12345, 67890, 1);
    
    EXPECT_EQ(txn_collector.tablet_id(), 12345);
    EXPECT_EQ(txn_collector.txn_id(), 67890);
    EXPECT_EQ(txn_collector.version(), 1);
    EXPECT_FALSE(txn_collector.has_data());

    CdcOperationData update_data("update");
    CdcRowData row;
    row.columns["id"] = "1";
    row.columns["name"] = "test";
    update_data.emplace_row(std::move(row));
    
    txn_collector.add_update_operation(std::move(update_data));
    EXPECT_TRUE(txn_collector.has_data());
    EXPECT_EQ(txn_collector.get_operations().size(), 1);
    EXPECT_EQ(txn_collector.get_operations()[0].operation_type, "update");
    EXPECT_EQ(txn_collector.get_operations()[0].size(), 1);
}

TEST_F(CdcIntegrationTest, TestCdcRowData) {
    CdcRowData row;
    row.reserve(3);
    
    row.columns["pk_col"] = "123";
    row.columns["name"] = "test_name";
    row.columns["value"] = "456.78";
    
    EXPECT_EQ(row.columns.size(), 3);
    EXPECT_EQ(row.columns["pk_col"], "123");
    EXPECT_EQ(row.columns["name"], "test_name");
    EXPECT_EQ(row.columns["value"], "456.78");
}

TEST_F(CdcIntegrationTest, TestCdcOperationData) {
    CdcOperationData operation("update");
    operation.reserve(2);

    CdcRowData row1;
    row1.columns["id"] = "1";
    row1.columns["name"] = "row1";
    operation.emplace_row(std::move(row1));

    CdcRowData row2;
    row2.columns["id"] = "2"; 
    row2.columns["name"] = "row2";
    operation.emplace_row(std::move(row2));
    
    EXPECT_EQ(operation.operation_type, "update");
    EXPECT_EQ(operation.size(), 2);
    EXPECT_FALSE(operation.empty());

    EXPECT_EQ(operation.rows[0].columns["id"], "1");
    EXPECT_EQ(operation.rows[1].columns["name"], "row2");
}

} // namespace starrocks::lake
