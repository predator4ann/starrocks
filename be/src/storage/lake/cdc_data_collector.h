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

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "common/status.h"
#include "storage/lake/rowset_update_state.h"
#include "storage/lake/tablet.h"
#include "storage/tablet_schema.h"

namespace starrocks {
class Chunk;
class TabletSchema;
class CdcColumnValuePB;
struct FileInfo;
using TabletSchemaCSPtr = std::shared_ptr<const TabletSchema>;

} // namespace starrocks

namespace starrocks::lake {

class CdcDataCollector;

// CDC transaction data container
class CdcTransactionData {
public:
    explicit CdcTransactionData(int64_t tablet_id, int64_t txn_id, int64_t version)
            : _tablet_id(tablet_id), _txn_id(txn_id), _version(version) {}

    int64_t tablet_id() const { return _tablet_id; }
    int64_t txn_id() const { return _txn_id; }
    int64_t version() const { return _version; }

private:
    int64_t _tablet_id;
    int64_t _txn_id;
    int64_t _version;
};

class CdcDataCollector {
public:
    CdcDataCollector() = default;
    ~CdcDataCollector() = default;

    // Collect delete data
    Status collect_delete_data(uint32_t del_id, const RowsetUpdateState& state, const RowsetUpdateStateParams& params,
                               int64_t txn_id, int64_t version, CdcTransactionData* collector);

    // Collect updated column data by loading segment file (for rewrite scenarios)
    Status collect_update_data(TabletManager* tablet_mgr, const FileInfo& src, const TabletSchemaCSPtr& tablet_schema,
                               const std::vector<uint32_t>& updated_column_ids, uint32_t segment_id, int64_t txn_id,
                               int64_t version, CdcTransactionData* collector);

private:
    // Unified function: serialize Chunk directly to CDC data (protobuf or JSON based on config)
    // Supports both Update and Delete operations with full type preservation
    // Returns serialized string, empty on error
    std::string serialize_chunk_to_cdc_data(const CdcTransactionData& collector, const Chunk* chunk,
                                            const TabletSchemaCSPtr& tablet_schema,
                                            const std::vector<uint32_t>& column_ids, const std::string& op_type);

    // Helper function to get raw string representation of column value (without debug formatting)
    static std::string column_value_to_raw_string(const Column* column, size_t row_idx,
                                                  const TabletColumn& tablet_column);

    // Helper template to convert column value with unified logic
    template <typename Handler>
    void convert_column_value_impl(const Column* column, size_t row_idx, const TabletColumn& tablet_column,
                                   Handler&& handler, const char* func_name);

    // Convert Column value to typed JSON value (preserves native types: int, float, bool, string)
    void column_value_to_json(const Column* column, size_t row_idx, const TabletColumn& tablet_column,
                              rapidjson::Value* json_value, rapidjson::Document::AllocatorType& allocator);

    // Convert column value to protobuf message (with full type preservation)
    void column_value_to_protobuf(const Column* column, size_t row_idx, const TabletColumn& tablet_column,
                                  starrocks::CdcColumnValuePB* pb_value);
};

} // namespace starrocks::lake
