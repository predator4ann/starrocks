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

#include <fmt/format.h>

#include <ctime>

#include "column/column_helper.h"
#include "common/config.h"
#include "gen_cpp/cdc.pb.h"
#include "gutil/strings/substitute.h"
#include "storage/chunk_helper.h"
#include "storage/lake/kafka_producer.h"
#include "storage/lake/tablet.h"
#include "storage/lake/tablet_metadata.h"
#include "storage/primary_key_encoder.h"
#include "types/logical_type.h"
#include "util/defer_op.h"
#include "util/starrocks_metrics.h"
#include "util/stopwatch.hpp"

namespace starrocks::lake {

Status CdcDataCollector::collect_delete_data(uint32_t del_id, const RowsetUpdateState& state,
                                             const RowsetUpdateStateParams& params, int64_t txn_id, int64_t version,
                                             CdcTransactionData* collector) {
    MonotonicStopWatch collect_timer;
    collect_timer.start();

    // Track delete operation count
    StarRocksMetrics::instance()->cdc_collect_delete_total.increment(1);

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

    // Decode PK data and serialize with type preservation
    std::vector<uint32_t> pk_column_ids;
    for (size_t i = 0; i < params.tablet_schema->num_key_columns(); i++) {
        pk_column_ids.push_back(static_cast<uint32_t>(i));
    }
    Schema pkey_schema = ChunkHelper::convert_schema(params.tablet_schema, pk_column_ids);
    auto pk_chunk = ChunkHelper::new_chunk(pkey_schema, num_deletes);

    // Decode all PK rows into the chunk
    for (size_t row_idx = 0; row_idx < num_deletes; ++row_idx) {
        RETURN_IF_ERROR(PrimaryKeyEncoder::decode(pkey_schema, *deletes, row_idx, 1, pk_chunk.get()));
    }

    // Record data read/decode time
    int64_t data_read_us = collect_timer.elapsed_time() / 1000;
    StarRocksMetrics::instance()->cdc_data_read_duration_us.increment(data_read_us);

    // Serialize and send delete operations with type preservation (same as Update)
    MonotonicStopWatch serialize_timer;
    serialize_timer.start();
    std::string serialized_data =
            serialize_chunk_to_cdc_data(*collector, pk_chunk.get(), params.tablet_schema, pk_column_ids, "d");
    int64_t serialize_us = serialize_timer.elapsed_time() / 1000;
    StarRocksMetrics::instance()->cdc_serialize_duration_us.increment(serialize_us);

    if (!serialized_data.empty()) {
        size_t byte_size = serialized_data.size();

        auto* kafka_producer = KafkaProducerPool::instance()->pick(collector->tablet_id());
        if (!kafka_producer) {
            return Status::InternalError(
                    fmt::format("CDC enabled but failed to get KafkaProducer instance for tablet_id={}, txn_id={}. "
                                "Transaction aborted to prevent CDC data loss.",
                                collector->tablet_id(), collector->txn_id()));
        }
        if (!kafka_producer->is_ready()) {
            Status init_st = kafka_producer->init();
            if (!init_st.ok()) {
                return Status::InternalError(fmt::format(
                        "CDC enabled but failed to initialize KafkaProducer for tablet_id={}, txn_id={}: {}. "
                        "Transaction aborted to prevent CDC data loss.",
                        collector->tablet_id(), collector->txn_id(), init_st.to_string()));
            }
        }

        std::string topic = kafka_producer->get_topic_name();
        std::string key = std::to_string(collector->tablet_id());

        MonotonicStopWatch kafka_timer;
        kafka_timer.start();
        Status st = kafka_producer->send_sync(topic, key, serialized_data, config::cdc_kafka_timeout_ms);
        int64_t kafka_us = kafka_timer.elapsed_time() / 1000;
        StarRocksMetrics::instance()->cdc_kafka_send_duration_us.increment(kafka_us);

        if (st.ok()) {
            // Track successful metrics
            StarRocksMetrics::instance()->cdc_kafka_send_success_total.increment(1);
            StarRocksMetrics::instance()->cdc_messages_sent_total.increment(1);
            StarRocksMetrics::instance()->cdc_rows_sent_total.increment(num_deletes);
            StarRocksMetrics::instance()->cdc_bytes_sent_total.increment(byte_size);
            StarRocksMetrics::instance()->cdc_delete_operations_total.increment(1);

            VLOG(2) << strings::Substitute(
                    "CDC delete sent: tablet=$0, txn=$1, rows=$2, bytes=$3, "
                    "data_read_us=$4, serialize_us=$5, kafka_us=$6",
                    collector->tablet_id(), collector->txn_id(), num_deletes, byte_size, data_read_us, serialize_us,
                    kafka_us);
        } else {
            StarRocksMetrics::instance()->cdc_kafka_send_failed_total.increment(1);
            LOG(ERROR) << "CDC Kafka send failed for delete: " << st.to_string()
                       << ", tablet_id=" << collector->tablet_id() << ", txn_id=" << collector->txn_id()
                       << ". Transaction will be aborted to ensure CDC data consistency.";
            RETURN_IF_ERROR(st); // Abort transaction immediately when Kafka send fails
        }
    }

    LOG(INFO) << strings::Substitute("CDC: Collected delete data for tablet=$0, txn_id=$1, del_id=$2, total_deletes=$3",
                                     params.tablet->id(), txn_id, del_id, num_deletes);

    return Status::OK();
}

Status CdcDataCollector::collect_update_data(TabletManager* tablet_mgr, const FileInfo& src,
                                             const TabletSchemaCSPtr& tablet_schema,
                                             const std::vector<uint32_t>& updated_column_ids, uint32_t segment_id,
                                             int64_t txn_id, int64_t version, CdcTransactionData* collector) {
    if (updated_column_ids.empty()) {
        return Status::OK();
    }

    // Track update operation count
    StarRocksMetrics::instance()->cdc_collect_update_total.increment(1);

    MonotonicStopWatch collect_timer;
    collect_timer.start();

    LOG(INFO) << strings::Substitute("CDC: Loading segment data for updated columns, segment_id=$0, updated_columns=$1",
                                     segment_id, updated_column_ids.size());

    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(src.path));

    size_t footer_size_hint = 16 * 1024;
    LakeIOOptions lake_io_opts{.fill_data_cache = false, .buffer_size = -1};
    ASSIGN_OR_RETURN(auto segment,
                     tablet_mgr->load_segment(src, segment_id, &footer_size_hint, lake_io_opts, true, tablet_schema));

    uint32_t num_rows = segment->num_rows();
    if (num_rows == 0) {
        return Status::OK();
    }

    Schema updated_schema = ChunkHelper::convert_schema(tablet_schema, updated_column_ids);

    // Read in paged batches to reduce peak memory
    const int64_t page_size = std::max<int64_t>(1, config::cdc_kafka_max_rows_per_message);
    SegmentReadOptions seg_options;
    OlapReaderStatistics stats;
    seg_options.fs = fs;
    seg_options.stats = &stats;
    seg_options.temporary_data = true;

    ASSIGN_OR_RETURN(auto itr, segment->new_iterator(updated_schema, seg_options));
    auto page_chunk = ChunkHelper::new_chunk(updated_schema, page_size);
    size_t total_rows_read = 0;

    // Record data read/decode time (segment loading and iterator creation)
    int64_t data_read_us = collect_timer.elapsed_time() / 1000;
    StarRocksMetrics::instance()->cdc_data_read_duration_us.increment(data_read_us);

    int64_t page_number = 0;
    while (total_rows_read < num_rows) {
        page_chunk->reset();
        seg_options.chunk_size = std::min<int64_t>(page_size, num_rows - total_rows_read);
        // Iterator respects requested chunk size via options; get_next fills up to chunk_size
        RETURN_IF_ERROR(itr->get_next(page_chunk.get()));
        if (page_chunk->num_rows() == 0) break;
        total_rows_read += page_chunk->num_rows();
        page_number++;

        size_t row_count = page_chunk->num_rows();

        MonotonicStopWatch serialize_timer;
        serialize_timer.start();
        std::string serialized_data =
                serialize_chunk_to_cdc_data(*collector, page_chunk.get(), tablet_schema, updated_column_ids, "u");
        int64_t serialize_us = serialize_timer.elapsed_time() / 1000;
        StarRocksMetrics::instance()->cdc_serialize_duration_us.increment(serialize_us);

        if (!serialized_data.empty()) {
            size_t byte_size = serialized_data.size();

            auto* kafka_producer = KafkaProducerPool::instance()->pick(collector->tablet_id());
            if (kafka_producer && kafka_producer->is_ready()) {
                std::string topic = kafka_producer->get_topic_name();
                std::string key = std::to_string(collector->tablet_id());

                MonotonicStopWatch kafka_timer;
                kafka_timer.start();
                Status st = kafka_producer->send_sync(topic, key, serialized_data, config::cdc_kafka_timeout_ms);
                int64_t kafka_us = kafka_timer.elapsed_time() / 1000;
                StarRocksMetrics::instance()->cdc_kafka_send_duration_us.increment(kafka_us);

                if (st.ok()) {
                    // Track successful metrics
                    StarRocksMetrics::instance()->cdc_kafka_send_success_total.increment(1);
                    StarRocksMetrics::instance()->cdc_messages_sent_total.increment(1);
                    StarRocksMetrics::instance()->cdc_rows_sent_total.increment(row_count);
                    StarRocksMetrics::instance()->cdc_bytes_sent_total.increment(byte_size);
                    StarRocksMetrics::instance()->cdc_update_operations_total.increment(1);

                    VLOG(2) << strings::Substitute(
                            "CDC update sent: tablet=$0, txn=$1, page=$2, rows=$3, "
                            "bytes=$4, serialize_us=$5, kafka_us=$6",
                            collector->tablet_id(), collector->txn_id(), page_number, row_count, byte_size,
                            serialize_us, kafka_us);
                } else {
                    StarRocksMetrics::instance()->cdc_kafka_send_failed_total.increment(1);
                    LOG(ERROR) << "CDC Kafka send failed for update: " << st.to_string()
                               << ", tablet_id=" << collector->tablet_id() << ", txn_id=" << collector->txn_id()
                               << ". Transaction will be aborted to ensure CDC data consistency.";
                    itr->close();
                    RETURN_IF_ERROR(st);
                }
            } else {
                // Kafka producer not ready
                itr->close();
                return Status::InternalError(
                        fmt::format("CDC enabled but Kafka producer is not ready for tablet_id={}, txn_id={}. "
                                    "Transaction aborted to prevent CDC data loss.",
                                    collector->tablet_id(), collector->txn_id()));
            }
        }
    }
    itr->close();

    LOG(INFO) << strings::Substitute(
            "CDC: Successfully collected updated column data, segment_id=$0, rows=$1, columns=$2", segment_id, num_rows,
            updated_schema.num_fields());

    return Status::OK();
}

std::string CdcDataCollector::serialize_chunk_to_cdc_data(const CdcTransactionData& collector, const Chunk* chunk,
                                                          const TabletSchemaCSPtr& tablet_schema,
                                                          const std::vector<uint32_t>& column_ids,
                                                          const std::string& op_type) {
    if (chunk == nullptr || chunk->num_rows() == 0) {
        return "";
    }

    std::string format = config::cdc_output_format;
    bool use_protobuf = (format == "protobuf");

    if (use_protobuf) {
        // Protobuf serialization with type preservation
        starrocks::CdcTransactionDataPB pb_data;
        pb_data.set_tablet_id(collector.tablet_id());
        pb_data.set_txn_id(collector.txn_id());
        pb_data.set_version(collector.version());
        pb_data.set_timestamp(std::time(nullptr));

        auto* pb_operation = pb_data.add_operations();
        pb_operation->set_operation_type(op_type);

        for (size_t row_idx = 0; row_idx < chunk->num_rows(); ++row_idx) {
            auto* pb_row = pb_operation->add_rows();
            for (size_t col_idx = 0; col_idx < chunk->num_columns(); ++col_idx) {
                const auto& column = chunk->get_column_by_index(col_idx);
                const auto& tablet_column = tablet_schema->column(column_ids[col_idx]);
                auto& pb_col_value = (*pb_row->mutable_columns())[std::string(tablet_column.name())];
                column_value_to_protobuf(column.get(), row_idx, tablet_column, &pb_col_value);
            }
        }

        std::string serialized;
        if (!pb_data.SerializeToString(&serialized)) {
            LOG(ERROR) << "Failed to serialize CDC data to protobuf for tablet_id=" << collector.tablet_id();
            return "";
        }
        return serialized;
    } else {
        rapidjson::Document doc;
        doc.SetObject();
        auto& allocator = doc.GetAllocator();

        doc.AddMember("tid", collector.tablet_id(), allocator);
        doc.AddMember("txn", collector.txn_id(), allocator);
        doc.AddMember("ver", collector.version(), allocator);
        doc.AddMember("ts", std::time(nullptr), allocator);

        rapidjson::Value ops_array(rapidjson::kArrayType);
        rapidjson::Value op_obj(rapidjson::kObjectType);
        op_obj.AddMember("op", rapidjson::Value(op_type.c_str(), allocator), allocator);

        rapidjson::Value rows_array(rapidjson::kArrayType);
        for (size_t row_idx = 0; row_idx < chunk->num_rows(); ++row_idx) {
            rapidjson::Value row_obj(rapidjson::kObjectType);
            for (size_t col_idx = 0; col_idx < chunk->num_columns(); ++col_idx) {
                const auto& column = chunk->get_column_by_index(col_idx);
                const auto& tablet_column = tablet_schema->column(column_ids[col_idx]);

                rapidjson::Value json_value;
                column_value_to_json(column.get(), row_idx, tablet_column, &json_value, allocator);
                std::string col_name(tablet_column.name());
                row_obj.AddMember(rapidjson::Value(col_name.c_str(), allocator), json_value, allocator);
            }
            rows_array.PushBack(row_obj, allocator);
        }

        op_obj.AddMember("data", rows_array, allocator);
        ops_array.PushBack(op_obj, allocator);
        doc.AddMember("ops", ops_array, allocator);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);
        return buffer.GetString();
    }
}

void CdcDataCollector::column_value_to_json(const Column* column, size_t row_idx, const TabletColumn& tablet_column,
                                            rapidjson::Value* json_value,
                                            rapidjson::Document::AllocatorType& allocator) {
    if (column == nullptr || row_idx >= column->size() || json_value == nullptr) {
        json_value->SetNull();
        return;
    }

    auto datum = column->get(row_idx);
    if (datum.is_null()) {
        json_value->SetNull();
        return;
    }

    // Convert based on logical type to use native JSON types
    LogicalType type = tablet_column.type();

    switch (type) {
    case TYPE_BOOLEAN:
        json_value->SetBool(datum.get_int8() != 0);
        break;
    case TYPE_TINYINT:
    case TYPE_SMALLINT:
    case TYPE_INT:
        json_value->SetInt(datum.get_int32());
        break;
    case TYPE_BIGINT:
        json_value->SetInt64(datum.get_int64());
        break;
    case TYPE_FLOAT:
        json_value->SetFloat(datum.get_float());
        break;
    case TYPE_DOUBLE:
        json_value->SetDouble(datum.get_double());
        break;
    case TYPE_VARCHAR:
    case TYPE_CHAR:
    case TYPE_DATE:
    case TYPE_DATETIME:
    case TYPE_TIME:
    case TYPE_DECIMAL:
    case TYPE_DECIMALV2:
    case TYPE_DECIMAL32:
    case TYPE_DECIMAL64:
    case TYPE_DECIMAL128:
    case TYPE_JSON:
    case TYPE_VARBINARY:
    case TYPE_HLL:
    case TYPE_OBJECT:
    case TYPE_PERCENTILE:
    default:
        // For complex types, use string representation
        json_value->SetString(column->debug_item(row_idx).c_str(), allocator);
        break;
    }
}

void CdcDataCollector::column_value_to_protobuf(const Column* column, size_t row_idx, const TabletColumn& tablet_column,
                                                starrocks::CdcColumnValuePB* pb_value) {
    if (column == nullptr || row_idx >= column->size() || pb_value == nullptr) {
        pb_value->set_is_null(true);
        return;
    }

    auto datum = column->get(row_idx);
    if (datum.is_null()) {
        pb_value->set_is_null(true);
        return;
    }

    // Convert based on logical type to preserve type information
    LogicalType type = tablet_column.type();

    switch (type) {
    case TYPE_BOOLEAN:
        pb_value->set_bool_value(datum.get_int8() != 0);
        break;
    case TYPE_TINYINT:
    case TYPE_SMALLINT:
    case TYPE_INT:
        pb_value->set_int32_value(datum.get_int32());
        break;
    case TYPE_BIGINT:
        pb_value->set_int64_value(datum.get_int64());
        break;
    case TYPE_FLOAT:
        pb_value->set_float_value(datum.get_float());
        break;
    case TYPE_DOUBLE:
        pb_value->set_double_value(datum.get_double());
        break;
    case TYPE_VARCHAR:
    case TYPE_CHAR:
    case TYPE_DATE:
    case TYPE_DATETIME:
    case TYPE_TIME:
    case TYPE_DECIMAL:
    case TYPE_DECIMALV2:
    case TYPE_DECIMAL32:
    case TYPE_DECIMAL64:
    case TYPE_DECIMAL128:
    case TYPE_JSON:
        // For complex types, still use string representation
        pb_value->set_string_value(column->debug_item(row_idx));
        break;
    case TYPE_VARBINARY:
    case TYPE_HLL:
    case TYPE_OBJECT:
    case TYPE_PERCENTILE:
        // Binary types
        pb_value->set_bytes_value(column->debug_item(row_idx));
        break;
    default:
        // Fallback to string for unknown types
        pb_value->set_string_value(column->debug_item(row_idx));
        break;
    }
}

} // namespace starrocks::lake
