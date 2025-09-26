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

#include <ctime>
#include "column/column_helper.h"
#include "common/config.h"
#include "gutil/strings/substitute.h"
#include "storage/lake/kafka_producer.h"
#include "storage/lake/tablet.h"
#include "storage/lake/tablet_metadata.h"
#include "storage/primary_key_encoder.h"
#include "storage/chunk_helper.h"
#include "util/defer_op.h"
#include "types/logical_type.h"

namespace starrocks::lake {

void CdcTransactionData::add_update_operation(CdcOperationData&& update_data) {
    if (!update_data.empty()) {
        update_data.operation_type = "u";
        _operations.emplace_back(std::move(update_data));
    }
}

void CdcTransactionData::add_delete_operation(CdcOperationData&& delete_data) {
    if (!delete_data.empty()) {
        delete_data.operation_type = "d";
        _operations.emplace_back(std::move(delete_data));
    }
}

bool CdcTransactionData::has_data() const {
    return !_operations.empty();
}

void CdcTransactionData::clear() {
    _operations.clear();
}

Status CdcDataCollector::collect_delete_data(uint32_t del_id, const RowsetUpdateState& state,
                                            const RowsetUpdateStateParams& params, int64_t txn_id, int64_t version,
                                            CdcTransactionData* collector) {
    LOG(INFO) << strings::Substitute("CDC: Starting to collect delete data for tablet=$0, txn_id=$1, del_id=$2", 
                                    params.tablet->id(), txn_id, del_id);
    
    const auto& deletes = state.deletes(del_id);
    if (deletes == nullptr) {
        return Status::OK();
    }
    
    size_t num_deletes = deletes->size();
    if (num_deletes == 0) {
        return Status::OK();
    }
    
    CdcOperationData delete_data("d");
    delete_data.reserve(num_deletes);
    
    for (size_t row_idx = 0; row_idx < num_deletes; ++row_idx) {
        CdcRowData cdc_row;
        cdc_row.reserve(params.tablet_schema->num_key_columns());
        
        // Delete operations only need primary key information
        RETURN_IF_ERROR(build_cdc_row_data_from_pk(deletes.get(), params.tablet_schema, 
                                                  row_idx, &cdc_row));
        
        delete_data.emplace_row(std::move(cdc_row));
    }
    
    // If streaming send is enabled, send this operation page immediately.
    if (config::cdc_streaming_send) {
        std::vector<CdcOperationData> page_ops;
        page_ops.emplace_back(std::move(delete_data));
        // Use page_number=1 and total_pages_hint=1 for delete-only page
        RETURN_IF_ERROR(send_operations_page(*collector, page_ops, /*page_number*/1, /*total_pages_hint*/1));
    } else {
        collector->add_delete_operation(std::move(delete_data));
    }
    
    LOG(INFO) << strings::Substitute("CDC: Collected delete data for tablet=$0, txn_id=$1, del_id=$2, total_deletes=$3", 
                                    params.tablet->id(), txn_id, del_id, num_deletes);
    
    return Status::OK();
}

Status CdcDataCollector::commit_transaction_data(const CdcTransactionData& collector) {
    if (!collector.has_data()) {
        return Status::OK();
    }
    
    auto* kafka_producer = KafkaProducerPool::instance()->pick(collector.tablet_id());
    if (!kafka_producer) {
        return Status::InternalError("Failed to get KafkaProducer instance");
    }
    if (!kafka_producer->is_ready()) {
        RETURN_IF_ERROR(kafka_producer->init());
    }
    // If streaming+transactions mode, the transaction is managed in process_unified_cdc.
    // Otherwise, perform a per-collector transactional send here.
    if (config::cdc_kafka_enable_transactions && config::cdc_streaming_send) {
        // Streaming mode should have already started and will commit at the end.
        RETURN_IF_ERROR(send_to_kafka(collector));
    } else {
        if (config::cdc_kafka_enable_transactions) {
            RETURN_IF_ERROR(kafka_producer->begin_transaction());
        }
        Status send_st = send_to_kafka(collector);
        if (!send_st.ok()) {
            if (config::cdc_kafka_enable_transactions) {
                (void)kafka_producer->abort_transaction();
            }
            return send_st;
        }
        if (config::cdc_kafka_enable_transactions) {
            RETURN_IF_ERROR(kafka_producer->commit_transaction());
        }
    }
    
    return Status::OK();
}


Status CdcDataCollector::build_cdc_row_data_from_pk(const Column* pk_column, const TabletSchemaCSPtr& tablet_schema,
                                                   size_t row_idx, CdcRowData* cdc_row) {
    if (row_idx >= pk_column->size()) {
        return Status::InvalidArgument("Row index out of range");
    }
    
    // For composite primary keys, we need to decode the encoded primary key
    if (tablet_schema->num_key_columns() == 1) {
        // Single primary key - direct access
        const auto& tablet_column = tablet_schema->column(0);
        std::string column_value = column_value_to_string(pk_column, row_idx, tablet_column);
        cdc_row->columns[std::string(tablet_column.name())] = std::move(column_value);
    } else {
        // Multiple primary keys - decode the composite key using PrimaryKeyEncoder
        RETURN_IF_ERROR(decode_composite_primary_key(pk_column, tablet_schema, row_idx, cdc_row));
    }
    
    return Status::OK();
}

Status CdcDataCollector::decode_composite_primary_key(const Column* pk_column, const TabletSchemaCSPtr& tablet_schema,
                                                     size_t row_idx, CdcRowData* cdc_row) {
    LOG(INFO) << strings::Substitute("CDC: Decoding composite primary key with $0 columns for row_idx=$1", 
                                    tablet_schema->num_key_columns(), row_idx);
    
    std::vector<uint32_t> pk_columns;
    for (size_t i = 0; i < tablet_schema->num_key_columns(); i++) {
        pk_columns.push_back((uint32_t)i);
    }
    Schema pkey_schema = ChunkHelper::convert_schema(tablet_schema, pk_columns);
    
    auto decoded_chunk = ChunkHelper::new_chunk(pkey_schema, 1);
    
    RETURN_IF_ERROR(PrimaryKeyEncoder::decode(pkey_schema, *pk_column, row_idx, 1, decoded_chunk.get()));
    
    for (size_t col_idx = 0; col_idx < decoded_chunk->num_columns(); ++col_idx) {
        const auto& column = decoded_chunk->get_column_by_index(col_idx);
        const auto& tablet_column = tablet_schema->column(col_idx);
        
        std::string column_value = column_value_to_string(column.get(), 0, tablet_column);
        cdc_row->columns[std::string(tablet_column.name())] = std::move(column_value);
    }
    
    LOG(INFO) << strings::Substitute("CDC: Successfully decoded composite primary key with $0 columns", 
                                    decoded_chunk->num_columns());
    
    return Status::OK();
}




Status CdcDataCollector::collect_update_data(const FileInfo& src, 
                                             const TabletSchemaCSPtr& tablet_schema,
                                             const std::vector<uint32_t>& updated_column_ids,
                                             uint32_t segment_id, int64_t txn_id, int64_t version,
                                             CdcTransactionData* collector) {
    if (updated_column_ids.empty()) {
        return Status::OK();
    }
    
    LOG(INFO) << strings::Substitute("CDC: Loading segment data for updated columns, segment_id=$0, updated_columns=$1", 
                                    segment_id, updated_column_ids.size());
    
    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(src.path));
    
    size_t footer_size_hint = 16 * 1024;
    LakeIOOptions lake_io_opts{.fill_data_cache = false, .buffer_size = -1};
    ASSIGN_OR_RETURN(auto segment, Segment::open(fs, src, segment_id, tablet_schema, &footer_size_hint, nullptr, lake_io_opts));
    
    uint32_t num_rows = segment->num_rows();
    if (num_rows == 0) {
        return Status::OK();
    }
    
    Schema updated_schema = ChunkHelper::convert_schema(tablet_schema, updated_column_ids);
    
    // Read in paged batches to reduce peak memory and enable streaming send
    const int64_t page_size = std::max<int64_t>(1, config::cdc_kafka_max_rows_per_message);
    const int64_t total_pages_hint = (num_rows + page_size - 1) / page_size;
    SegmentReadOptions seg_options;
    OlapReaderStatistics stats;
    seg_options.fs = fs;
    seg_options.stats = &stats;
    seg_options.temporary_data = true;
    
    ASSIGN_OR_RETURN(auto itr, segment->new_iterator(updated_schema, seg_options));
    auto page_chunk = ChunkHelper::new_chunk(updated_schema, page_size);
    size_t total_rows_read = 0;
    
    int64_t page_number = 0;
    while (total_rows_read < num_rows) {
        page_chunk->reset();
        seg_options.chunk_size = std::min<int64_t>(page_size, num_rows - total_rows_read);
        // Iterator respects requested chunk size via options; get_next fills up to chunk_size
        RETURN_IF_ERROR(itr->get_next(page_chunk.get()));
        if (page_chunk->num_rows() == 0) break;
        total_rows_read += page_chunk->num_rows();
        page_number++;

        // Build operation rows for this page
        CdcOperationData update_data("u");
        update_data.reserve(page_chunk->num_rows());
        for (size_t row_idx = 0; row_idx < page_chunk->num_rows(); ++row_idx) {
            CdcRowData cdc_row;
            cdc_row.reserve(page_chunk->num_columns());
            for (size_t col_idx = 0; col_idx < page_chunk->num_columns(); ++col_idx) {
                const auto& column = page_chunk->get_column_by_index(col_idx);
                const auto& tablet_column = tablet_schema->column(updated_column_ids[col_idx]);
                std::string column_value = column_value_to_string(column.get(), row_idx, tablet_column);
                cdc_row.columns[std::string(tablet_column.name())] = std::move(column_value);
            }
            update_data.emplace_row(std::move(cdc_row));
        }

        if (config::cdc_streaming_send) {
            std::vector<CdcOperationData> page_ops;
            page_ops.emplace_back(std::move(update_data));
            RETURN_IF_ERROR(send_operations_page(*collector, page_ops, page_number, total_pages_hint));
        } else {
            collector->add_update_operation(std::move(update_data));
        }
    }
    itr->close();
    
    LOG(INFO) << strings::Substitute("CDC: Successfully collected updated column data, segment_id=$0, rows=$1, columns=$2", 
                                    segment_id, num_rows, updated_schema.num_fields());
    
    return Status::OK();
}


std::string CdcDataCollector::column_value_to_string(const Column* column, size_t row_idx, 
                                                    const TabletColumn& tablet_column) {
    if (column == nullptr || row_idx >= column->size()) {
        return "";
    }
    
    try {
        auto datum = column->get(row_idx);
        
        if (datum.is_null()) {
            return "NULL";
        }
        
        return column->debug_item(row_idx);
        
    } catch (const std::exception& e) {
        LOG(WARNING) << strings::Substitute("CDC: Failed to convert column value to string for row_idx=$0, column_type=$1, error=$2", 
                                          row_idx, tablet_column.type(), e.what());
        return "<conversion_error>";
    }
}

Status CdcDataCollector::send_to_kafka(const CdcTransactionData& collector) {
    if (!collector.has_data()) {
        VLOG(2) << "No CDC data to send for tablet_id=" << collector.tablet_id();
        return Status::OK();
    }
    
    auto* kafka_producer = KafkaProducerPool::instance()->pick(collector.tablet_id());
    if (!kafka_producer) {
        return Status::InternalError("Failed to get KafkaProducer instance");
    }
    
    if (!kafka_producer->is_ready()) {
        RETURN_IF_ERROR(kafka_producer->init());
    }
    
    std::string topic = kafka_producer->get_topic_name();
    
    const auto& operations = collector.get_operations();
    if (operations.empty()) {
        return Status::OK();
    }
    
    size_t total_rows = 0;
    for (const auto& operation : operations) {
        total_rows += operation.rows.size();
    }
    
    size_t max_rows_per_message = static_cast<size_t>(config::cdc_kafka_max_rows_per_message);
    size_t total_pages = (total_rows + max_rows_per_message - 1) / max_rows_per_message;
    
    if (total_pages == 0) {
        return Status::OK();
    }
    
    LOG(INFO) << strings::Substitute("CDC streaming: tablet_id=$0, txn_id=$1, total_rows=$2, total_pages=$3",
                                    collector.tablet_id(), collector.txn_id(), total_rows, total_pages);
    
    size_t current_page = 0;
    size_t current_row_count = 0;
    size_t operation_idx = 0;
    size_t row_idx_in_operation = 0;
    
    while (current_page < total_pages && operation_idx < operations.size()) {
        // Build operations for this page, respecting max_rows_per_message limit
        std::vector<CdcOperationData> page_ops;
        size_t rows_in_current_message = 0;
        
        // Fill current message with operations/rows up to the limit
        while (operation_idx < operations.size() && rows_in_current_message < max_rows_per_message) {
            const auto& operation = operations[operation_idx];
            
            if (operation.empty()) {
                operation_idx++;
                row_idx_in_operation = 0;
                continue;
            }
            
            // Create a new operation for this page
            CdcOperationData page_operation(operation.operation_type);
            
            // Add rows from current operation, obeying per-message limit strictly
            size_t rows_allowed = max_rows_per_message - rows_in_current_message;
            size_t added = 0;
            while (row_idx_in_operation < operation.rows.size() && added < rows_allowed) {
                page_operation.emplace_row(CdcRowData(operation.rows[row_idx_in_operation].columns));
                row_idx_in_operation++;
                rows_in_current_message++;
                added++;
            }
            
            if (!page_operation.empty()) {
                page_ops.emplace_back(std::move(page_operation));
            }
            
            // If we've processed all rows in this operation, move to next operation
            if (row_idx_in_operation >= operation.rows.size()) {
                operation_idx++;
                row_idx_in_operation = 0;
            }

            // If page is full, stop adding more operations/rows now
            if (rows_in_current_message >= max_rows_per_message) {
                break;
            }
        }
        
        std::string json_data = serialize_cdc_data_to_json(collector, page_ops, 
                                                          current_page + 1, total_pages, 
                                                          total_rows, rows_in_current_message);
        
        // Generate message key using tablet_id
        std::string key = std::to_string(collector.tablet_id());
        
        // Send message synchronously
        Status send_status = kafka_producer->send_sync(topic, key, json_data, config::cdc_kafka_timeout_ms);
        
        if (send_status.ok()) {
            LOG(INFO) << strings::Substitute("CDC page sent to Kafka: tablet_id=$0, txn_id=$1, "
                                            "page=$2/$3, rows=$4, data_size=$5", 
                                            collector.tablet_id(), collector.txn_id(), 
                                            current_page + 1, total_pages, rows_in_current_message, json_data.size());
            VLOG(2) << "CDC JSON page [" << (current_page + 1) << "/" << total_pages << "]: " << json_data;
        } else {
            LOG(ERROR) << strings::Substitute("Failed to send CDC page to Kafka: tablet_id=$0, txn_id=$1, "
                                             "page=$2/$3, error=$4", 
                                             collector.tablet_id(), collector.txn_id(),
                                             current_page + 1, total_pages, send_status.to_string());
            
            // Return error to ensure transaction consistency
            return send_status;
        }
        
        current_page++;
        current_row_count += rows_in_current_message;
    }
    
    if (total_pages > 1) {
        LOG(INFO) << strings::Substitute("CDC transaction completed: tablet_id=$0, txn_id=$1, "
                                        "total_pages=$2, total_rows=$3",
                                        collector.tablet_id(), collector.txn_id(), total_pages, total_rows);
    }
    
    return Status::OK();
}

Status CdcDataCollector::send_operations_page(const CdcTransactionData& collector,
                                             const std::vector<CdcOperationData>& page_operations,
                                             int64_t page_number,
                                             int64_t total_pages_hint) {
    auto* kafka_producer = KafkaProducerPool::instance()->pick(collector.tablet_id());
    if (!kafka_producer) return Status::InternalError("Failed to get KafkaProducer instance");
    if (!kafka_producer->is_ready()) RETURN_IF_ERROR(kafka_producer->init());
    std::string topic = kafka_producer->get_topic_name();

    // Calculate total rows in page
    size_t rows_in_current_message = 0;
    for (const auto& operation : page_operations) {
        rows_in_current_message += operation.rows.size();
    }
    
    // For streaming operations, we don't have accurate total_rows, so use actual rows count
    // and treat each operation as independent (no pagination info)
    std::string json_data = serialize_cdc_data_to_json(collector, page_operations, 
                                                      1, 1,  // No pagination for streaming operations
                                                      0, rows_in_current_message);

    std::string key = std::to_string(collector.tablet_id());
    Status st = kafka_producer->send_sync(topic, key, json_data, config::cdc_kafka_timeout_ms);
    return st;
}

std::string CdcDataCollector::serialize_cdc_data_to_json(const CdcTransactionData& collector,
                                                        const std::vector<CdcOperationData>& operations,
                                                        int64_t page_number,
                                                        int64_t total_pages,
                                                        size_t total_rows,
                                                        size_t rows_in_page) {
    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();
    
    doc.AddMember("tid", collector.tablet_id(), allocator);
    doc.AddMember("txn", collector.txn_id(), allocator);
    doc.AddMember("ver", collector.version(), allocator);
    doc.AddMember("ts", std::time(nullptr), allocator);
    
    // Pagination info - only add if there's actual pagination
    if (total_pages > 1) {
        rapidjson::Value pg(rapidjson::kObjectType);
        pg.AddMember("num", static_cast<int64_t>(page_number), allocator);
        pg.AddMember("total", static_cast<int64_t>(total_pages), allocator);
        pg.AddMember("rows", static_cast<int64_t>(total_rows), allocator);
        pg.AddMember("cur", static_cast<int64_t>(rows_in_page), allocator);  // current message rows
        doc.AddMember("pg", pg, allocator);
    }
    
    // Operations array with shortened field names
    rapidjson::Value ops_array(rapidjson::kArrayType);
    size_t actual_rows_in_page = 0;
    
    for (const auto& operation : operations) {
        if (operation.empty()) continue;
        
        rapidjson::Value op_obj(rapidjson::kObjectType);
        rapidjson::Value op_type(operation.operation_type.c_str(), allocator);
        op_obj.AddMember("op", op_type, allocator);
        
        rapidjson::Value rows_array(rapidjson::kArrayType);
        for (const auto& row : operation.rows) {
            rapidjson::Value row_obj(rapidjson::kObjectType);
            for (const auto& [column_name, column_value] : row.columns) {
                rapidjson::Value col_name(column_name.c_str(), allocator);
                rapidjson::Value col_value(column_value.c_str(), allocator);
                row_obj.AddMember(col_name, col_value, allocator);
            }
            rows_array.PushBack(row_obj, allocator);
            actual_rows_in_page++;
        }
        op_obj.AddMember("data", rows_array, allocator);
        ops_array.PushBack(op_obj, allocator);
    }
    
    doc.AddMember("ops", ops_array, allocator);
    
    if (rows_in_page > 0 && rows_in_page != actual_rows_in_page) {
        doc.AddMember("cnt", static_cast<int64_t>(rows_in_page), allocator);
    }
    
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return buffer.GetString();
}

} // namespace starrocks::lake
