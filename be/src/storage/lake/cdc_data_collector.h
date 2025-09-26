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

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "common/status.h"
#include "storage/lake/rowset_update_state.h"
#include "storage/lake/tablet.h"
#include "storage/tablet_schema.h"
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace starrocks {
class Chunk;
class TabletSchema;
struct FileInfo;
using TabletSchemaCSPtr = std::shared_ptr<const TabletSchema>;

}

namespace starrocks::lake {

// Forward declarations
class CdcDataCollector;
class CdcTransactionData;

struct CdcRowData {
    std::unordered_map<std::string, std::string> columns;  // column_name -> column_value
    
    CdcRowData() = default;
    CdcRowData(std::unordered_map<std::string, std::string> cols) : columns(std::move(cols)) {}
    
    void reserve(size_t column_count) {
        columns.reserve(column_count);
    }
};

struct CdcOperationData {
    std::vector<CdcRowData> rows;
    std::string operation_type;  // "update", "delete"
    
    CdcOperationData() = default;
    CdcOperationData(std::string op_type) : operation_type(std::move(op_type)) {}
    
    void reserve(size_t row_count) {
        rows.reserve(row_count);
    }
    
    void emplace_row(CdcRowData&& row) {
        rows.emplace_back(std::move(row));
    }
    
    void append_rows(std::vector<CdcRowData>&& new_rows) {
        if (rows.empty()) {
            rows = std::move(new_rows);
        } else {
            rows.reserve(rows.size() + new_rows.size());
            for (auto& row : new_rows) {
                rows.emplace_back(std::move(row));
            }
        }
    }
    
    bool empty() const { return rows.empty(); }
    size_t size() const { return rows.size(); }
};

// CDC transaction data container - holds all CDC operations for a single transaction
class CdcTransactionData {
public:
    explicit CdcTransactionData(int64_t tablet_id, int64_t txn_id, int64_t version)
        : _tablet_id(tablet_id), _txn_id(txn_id), _version(version) {}

    // Add UPDATE operation data
    void add_update_operation(CdcOperationData&& update_data);
    
    // Add DELETE operation data
    void add_delete_operation(CdcOperationData&& delete_data);
    
    // Get all collected operations
    const std::vector<CdcOperationData>& get_operations() const { return _operations; }
    
    // Check if has any data
    bool has_data() const;
    
    // Clear all data
    void clear();

    int64_t tablet_id() const { return _tablet_id; }
    int64_t txn_id() const { return _txn_id; }
    int64_t version() const { return _version; }

private:
    int64_t _tablet_id;
    int64_t _txn_id;
    int64_t _version;
    std::vector<CdcOperationData> _operations;
};


class CdcDataCollector {
public:
    CdcDataCollector() = default;
    ~CdcDataCollector() = default;

    // Collect delete data
    Status collect_delete_data(uint32_t del_id, const RowsetUpdateState& state,
                              const RowsetUpdateStateParams& params, int64_t txn_id, int64_t version,
                              CdcTransactionData* collector);
    
    // Commit transaction-level CDC data
    Status commit_transaction_data(const CdcTransactionData& collector);
    
    // Kafka output methods
    Status send_to_kafka(const CdcTransactionData& collector);
    // Streaming send a single page of operations (e.g., one update page or batched deletes)
    Status send_operations_page(const CdcTransactionData& collector,
                                const std::vector<CdcOperationData>& page_operations,
                                int64_t page_number,
                                int64_t total_pages_hint);
    
    // Collect updated column data by loading segment file (for rewrite scenarios)
    Status collect_update_data(const FileInfo& src, 
                               const TabletSchemaCSPtr& tablet_schema,
                               const std::vector<uint32_t>& updated_column_ids,
                               uint32_t segment_id, int64_t txn_id, int64_t version,
                               CdcTransactionData* collector);
    
private:
    // Build CdcRowData from primary key column only (for DELETE operations)
    Status build_cdc_row_data_from_pk(const Column* pk_column, const TabletSchemaCSPtr& tablet_schema,
                                     size_t row_idx, CdcRowData* cdc_row);
    
    // Decode composite primary key into individual column values
    Status decode_composite_primary_key(const Column* pk_column, const TabletSchemaCSPtr& tablet_schema,
                                       size_t row_idx, CdcRowData* cdc_row);
    
    // Convert Column value to string representation
    std::string column_value_to_string(const Column* column, size_t row_idx, 
                                      const TabletColumn& tablet_column);

    // Common JSON serialization with optimized field names and pagination handling
    std::string serialize_cdc_data_to_json(const CdcTransactionData& collector,
                                          const std::vector<CdcOperationData>& operations,
                                          int64_t page_number = 1,
                                          int64_t total_pages = 1,
                                          size_t total_rows = 0,
                                          size_t rows_in_page = 0);
};

} // namespace starrocks::lake
