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

#include "column/binary_column.h"
#include "column/column_helper.h"
#include "column/const_column.h"
#include "column/fixed_length_column.h"
#include "column/nullable_column.h"
#include "common/compiler_util.h"
#include "common/config.h"
#include "gen_cpp/cdc.pb.h"
#include "gutil/strings/substitute.h"
#include "runtime/decimalv2_value.h"
#include "runtime/decimalv3.h"
#include "storage/chunk_helper.h"
#include "storage/lake/kafka_producer.h"
#include "storage/lake/tablet.h"
#include "storage/lake/tablet_metadata.h"
#include "storage/primary_key_encoder.h"
#include "types/date_value.h"
#include "types/large_int_value.h"
#include "types/logical_type.h"
#include "types/timestamp_value.h"
#include "util/defer_op.h"
#include "util/starrocks_metrics.h"
#include "util/stopwatch.hpp"

namespace starrocks::lake {

// Helper function to get raw string representation of column value without debug formatting
std::string CdcDataCollector::column_value_to_raw_string(const Column* column, size_t row_idx,
                                                         const TabletColumn& tablet_column) {
    if (column == nullptr || row_idx >= column->size()) {
        return "";
    }

    auto datum = column->get(row_idx);
    if (datum.is_null()) {
        return "";
    }

    LogicalType type = tablet_column.type();

    switch (type) {
    case TYPE_VARCHAR:
    case TYPE_CHAR: {
        // For string types, get the slice directly without quotes
        const auto& slice = datum.get_slice();
        return std::string(slice.data, slice.size);
    }
    case TYPE_DATE: {
        // Date type: use to_string()
        return datum.get_date().to_string();
    }
    case TYPE_DATE_V1: {
        // Date V1 type
        return datum.get_date().to_string();
    }
    case TYPE_DATETIME:
    case TYPE_DATETIME_V1: {
        // Datetime type: use to_string()
        return datum.get_timestamp().to_string();
    }
    case TYPE_TIME: {
        // Time is stored as double, need special handling
        double time_val = datum.get_double();
        int hour = static_cast<int>(time_val / 3600);
        int minute = static_cast<int>((time_val - hour * 3600) / 60);
        double second = time_val - hour * 3600 - minute * 60;
        return fmt::format("{:02d}:{:02d}:{:02.0f}", hour, minute, second);
    }
    case TYPE_LARGEINT: {
        // int128 to string
        return LargeIntValue::to_string(datum.get_int128());
    }
    case TYPE_DECIMAL:
    case TYPE_DECIMALV2: {
        // DecimalV2 to string
        return datum.get_decimal().to_string();
    }
    case TYPE_DECIMAL32: {
        // Decimal32 to string
        return DecimalV3Cast::to_string<int32_t>(datum.get_int32(), tablet_column.precision(), tablet_column.scale());
    }
    case TYPE_DECIMAL64: {
        // Decimal64 to string
        return DecimalV3Cast::to_string<int64_t>(datum.get_int64(), tablet_column.precision(), tablet_column.scale());
    }
    case TYPE_DECIMAL128: {
        // Decimal128 to string
        return DecimalV3Cast::to_string<int128_t>(datum.get_int128(), tablet_column.precision(), tablet_column.scale());
    }
    case TYPE_JSON: {
        // JSON: try to get string representation
        const auto* json_val = datum.get_json();
        if (json_val != nullptr) {
            auto json_str = json_val->to_string();
            if (json_str.ok()) {
                return *json_str;
            }
        }
        return "{}";
    }
    case TYPE_STRUCT:
    case TYPE_ARRAY:
    case TYPE_MAP: {
        // For complex types, fall back to debug_item as they need recursive formatting
        // But ideally should implement proper serialization
        return column->debug_item(row_idx);
    }
    case TYPE_VARBINARY:
    case TYPE_BINARY: {
        // Binary data: get slice
        const auto& slice = datum.get_slice();
        return std::string(slice.data, slice.size);
    }
    case TYPE_HLL:
    case TYPE_OBJECT:
    case TYPE_PERCENTILE: {
        // Object types: use their to_string() if available
        // Fall back to debug_item
        return column->debug_item(row_idx);
    }
    default:
        // For other types that are already handled as numbers, shouldn't reach here
        // Fall back to debug_item
        return column->debug_item(row_idx);
    }
}

Status CdcDataCollector::collect_delete_data(uint32_t del_id, const RowsetUpdateState& state,
                                             const RowsetUpdateStateParams& params, int64_t txn_id, int64_t version,
                                             CdcTransactionData* collector, const std::string& topic) {
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
            std::string err = fmt::format(
                    "CDC enabled but failed to get KafkaProducer instance for tablet_id={}, txn_id={}. "
                    "Transaction aborted to prevent CDC data loss.",
                    collector->tablet_id(), collector->txn_id());
            return Status::InternalError(err);
        }
        if (!kafka_producer->is_ready()) {
            Status init_st = kafka_producer->init();
            if (!init_st.ok()) {
                std::string err = fmt::format(
                        "CDC enabled but failed to initialize KafkaProducer for tablet_id={}, txn_id={}: {}. "
                        "Transaction aborted to prevent CDC data loss.",
                        collector->tablet_id(), collector->txn_id(), init_st.to_string());
                return Status::InternalError(err);
            }
        }

        std::string key = std::to_string(collector->tablet_id());
        MonotonicStopWatch kafka_timer;
        kafka_timer.start();
        Status st = kafka_producer->send_sync(topic, key, serialized_data, config::cdc_kafka_timeout_ms);
        int64_t kafka_us = kafka_timer.elapsed_time() / 1000;
        StarRocksMetrics::instance()->cdc_kafka_send_duration_us.increment(kafka_us);

        if (st.ok()) {
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
            return st;
        }
    }

    LOG(INFO) << strings::Substitute("CDC: Collected delete data for tablet=$0, txn_id=$1, del_id=$2, total_deletes=$3",
                                     params.tablet->id(), txn_id, del_id, num_deletes);

    return Status::OK();
}

Status CdcDataCollector::collect_update_data(TabletManager* tablet_mgr, const FileInfo& src,
                                             const TabletSchemaCSPtr& tablet_schema,
                                             const std::vector<uint32_t>& updated_column_ids, uint32_t segment_id,
                                             int64_t txn_id, int64_t version, CdcTransactionData* collector,
                                             const std::string& topic) {
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
    seg_options.chunk_size = static_cast<int>(std::min<int64_t>(page_size, num_rows));
    ASSIGN_OR_RETURN(auto itr, segment->new_iterator(updated_schema, seg_options));
    auto page_chunk = ChunkHelper::new_chunk(updated_schema, page_size);
    size_t total_rows_read = 0;

    // Record data read/decode time (segment loading and iterator creation)
    int64_t data_read_us = collect_timer.elapsed_time() / 1000;
    StarRocksMetrics::instance()->cdc_data_read_duration_us.increment(data_read_us);

    int64_t page_number = 0;
    while (total_rows_read < num_rows) {
        page_chunk->reset();
        //seg_options.chunk_size = std::min<int64_t>(page_size, num_rows - total_rows_read);
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
            if (!kafka_producer || !kafka_producer->is_ready()) {
                std::string err = fmt::format(
                        "CDC enabled but Kafka producer is not ready for tablet_id={}, txn_id={}. "
                        "Transaction aborted to prevent CDC data loss.",
                        collector->tablet_id(), collector->txn_id());
                itr->close();
                return Status::InternalError(err);
            }

            std::string key = std::to_string(collector->tablet_id());
            MonotonicStopWatch kafka_timer;
            kafka_timer.start();
            Status st = kafka_producer->send_sync(topic, key, serialized_data, config::cdc_kafka_timeout_ms);
            int64_t kafka_us = kafka_timer.elapsed_time() / 1000;
            StarRocksMetrics::instance()->cdc_kafka_send_duration_us.increment(kafka_us);

            if (st.ok()) {
                StarRocksMetrics::instance()->cdc_kafka_send_success_total.increment(1);
                StarRocksMetrics::instance()->cdc_messages_sent_total.increment(1);
                StarRocksMetrics::instance()->cdc_rows_sent_total.increment(row_count);
                StarRocksMetrics::instance()->cdc_bytes_sent_total.increment(byte_size);
                StarRocksMetrics::instance()->cdc_update_operations_total.increment(1);

                VLOG(2) << strings::Substitute(
                        "CDC update sent: tablet=$0, txn=$1, page=$2, rows=$3, "
                        "bytes=$4, serialize_us=$5, kafka_us=$6",
                        collector->tablet_id(), collector->txn_id(), page_number, row_count, byte_size, serialize_us,
                        kafka_us);
            } else {
                StarRocksMetrics::instance()->cdc_kafka_send_failed_total.increment(1);
                LOG(ERROR) << "CDC Kafka send failed for update: " << st.to_string()
                           << ", tablet_id=" << collector->tablet_id() << ", txn_id=" << collector->txn_id()
                           << ". Transaction will be aborted to ensure CDC data consistency.";
                itr->close();
                return st;
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

template <typename Handler>
void CdcDataCollector::convert_column_value_impl(const Column* column, size_t row_idx,
                                                 const TabletColumn& tablet_column, Handler&& handler,
                                                 const char* func_name) {
    LogicalType type = tablet_column.type();

    try {
        // Unwrap wrapper columns (NullableColumn, ConstColumn) to get actual data column
        const Column* data_column = column;

        // Handle ConstColumn
        if (column->is_constant()) {
            auto* const_col = down_cast<const ConstColumn*>(column);
            data_column = const_col->data_column().get();
            if (UNLIKELY(data_column == nullptr)) {
                LOG(ERROR) << "CDC: ConstColumn has null data_column, column_name=" << tablet_column.name()
                           << ", type=" << type << ", row_idx=" << row_idx;
                handler.handle_null();
                return;
            }
        }

        // Handle NullableColumn
        if (data_column->is_nullable()) {
            auto* nullable_col = down_cast<const NullableColumn*>(data_column);
            data_column = nullable_col->data_column().get();
            if (UNLIKELY(data_column == nullptr)) {
                LOG(ERROR) << "CDC: NullableColumn has null data_column, column_name=" << tablet_column.name()
                           << ", type=" << type << ", row_idx=" << row_idx;
                handler.handle_null();
                return;
            }
        }

// Helper macro for simple numeric types - direct array access without extra checks
#define HANDLE_NUMERIC_TYPE(TYPE_ENUM, CPP_TYPE, HANDLER_METHOD)                     \
    case TYPE_ENUM: {                                                                \
        auto* data_col = down_cast<const FixedLengthColumn<CPP_TYPE>*>(data_column); \
        handler.HANDLER_METHOD(data_col->get_data()[row_idx]);                       \
        return;                                                                      \
    }

        // Branch prediction: most common types (INT, BIGINT, VARCHAR, DATE, DATETIME) are handled first
        switch (type) {
            HANDLE_NUMERIC_TYPE(TYPE_INT, int32_t, handle_int32)
            HANDLE_NUMERIC_TYPE(TYPE_BIGINT, int64_t, handle_int64)

        case TYPE_VARCHAR:
        case TYPE_CHAR: {
            auto* binary_col = down_cast<const BinaryColumn*>(data_column);
            Slice slice = binary_col->get_slice(row_idx);
            handler.handle_string(std::string(slice.data, slice.size));
            return;
        }
        case TYPE_DATE:
        case TYPE_DATE_V1: {
            auto* data_col = down_cast<const FixedLengthColumn<DateValue>*>(data_column);
            const DateValue& date_val = data_col->get_data()[row_idx];
            handler.handle_string(date_val.to_string());
            return;
        }
        case TYPE_DATETIME:
        case TYPE_DATETIME_V1: {
            auto* data_col = down_cast<const FixedLengthColumn<TimestampValue>*>(data_column);
            const TimestampValue& ts_val = data_col->get_data()[row_idx];
            handler.handle_string(ts_val.to_string());
            return;
        }

        case TYPE_BOOLEAN: {
            auto* data_col = down_cast<const FixedLengthColumn<uint8_t>*>(data_column);
            handler.handle_bool(data_col->get_data()[row_idx] != 0);
            return;
        }

            HANDLE_NUMERIC_TYPE(TYPE_TINYINT, int8_t, handle_int8)
            HANDLE_NUMERIC_TYPE(TYPE_UNSIGNED_TINYINT, uint8_t, handle_uint8)
            HANDLE_NUMERIC_TYPE(TYPE_SMALLINT, int16_t, handle_int16)
            HANDLE_NUMERIC_TYPE(TYPE_UNSIGNED_SMALLINT, uint16_t, handle_uint16)
            HANDLE_NUMERIC_TYPE(TYPE_UNSIGNED_INT, uint32_t, handle_uint32)
            HANDLE_NUMERIC_TYPE(TYPE_UNSIGNED_BIGINT, uint64_t, handle_uint64)
            HANDLE_NUMERIC_TYPE(TYPE_FLOAT, float, handle_float)
            HANDLE_NUMERIC_TYPE(TYPE_DOUBLE, double, handle_double)
            HANDLE_NUMERIC_TYPE(TYPE_DISCRETE_DOUBLE, double, handle_double)

#undef HANDLE_NUMERIC_TYPE
        case TYPE_LARGEINT:
        case TYPE_TIME:
        case TYPE_DECIMAL:
        case TYPE_DECIMALV2:
        case TYPE_DECIMAL32:
        case TYPE_DECIMAL64:
        case TYPE_DECIMAL128:
        case TYPE_JSON:
        case TYPE_STRUCT:
        case TYPE_ARRAY:
        case TYPE_MAP:
            handler.handle_string(column_value_to_raw_string(column, row_idx, tablet_column));
            return;
        case TYPE_BINARY:
        case TYPE_VARBINARY: {
            auto* binary_col = down_cast<const BinaryColumn*>(data_column);
            Slice slice = binary_col->get_slice(row_idx);
            handler.handle_bytes(std::string(slice.data, slice.size));
            return;
        }
        case TYPE_HLL:
        case TYPE_OBJECT:
        case TYPE_PERCENTILE:
            handler.handle_bytes(column_value_to_raw_string(column, row_idx, tablet_column));
            return;
        case TYPE_NULL:
        case TYPE_NONE:
            handler.handle_null();
            return;
        case TYPE_UNKNOWN:
        case TYPE_FUNCTION:
        default:
            if (UNLIKELY(true)) {
                LOG(WARNING) << "CDC: Unsupported column type in " << func_name << ", type=" << type
                             << ", column_name=" << tablet_column.name() << ", using raw string as fallback";
            }
            handler.handle_string(column_value_to_raw_string(column, row_idx, tablet_column));
            return;
        }
    } catch (const std::bad_cast& e) {
        // Catches down_cast failures - indicates schema mismatch between TabletColumn and actual Column
        LOG(ERROR) << "CDC: Type mismatch caught in " << func_name << ", type=" << type
                   << ", column_name=" << tablet_column.name() << ", row_idx=" << row_idx << ", error=" << e.what()
                   << ", is_nullable=" << column->is_nullable()
                   << ". This indicates a schema inconsistency issue. Using fallback.";
        handler.handle_string(column_value_to_raw_string(column, row_idx, tablet_column));
    } catch (const std::exception& e) {
        // Catches other unexpected exceptions
        LOG(ERROR) << "CDC: Unexpected exception caught in " << func_name << ", type=" << type
                   << ", column_name=" << tablet_column.name() << ", row_idx=" << row_idx << ", error=" << e.what()
                   << ", is_nullable=" << column->is_nullable() << ". Using fallback.";
        handler.handle_string(column_value_to_raw_string(column, row_idx, tablet_column));
    }
}

struct JsonValueHandler {
    rapidjson::Value* json_value;
    rapidjson::Document::AllocatorType& allocator;

    void handle_null() const { json_value->SetNull(); }
    void handle_bool(bool val) const { json_value->SetBool(val); }
    void handle_int8(int8_t val) const { json_value->SetInt(val); }
    void handle_uint8(uint8_t val) const { json_value->SetUint(val); }
    void handle_int16(int16_t val) const { json_value->SetInt(val); }
    void handle_uint16(uint16_t val) const { json_value->SetUint(val); }
    void handle_int32(int32_t val) const { json_value->SetInt(val); }
    void handle_uint32(uint32_t val) const { json_value->SetUint(val); }
    void handle_int64(int64_t val) const { json_value->SetInt64(val); }
    void handle_uint64(uint64_t val) const { json_value->SetUint64(val); }
    void handle_float(float val) const { json_value->SetFloat(val); }
    void handle_double(double val) const { json_value->SetDouble(val); }
    void handle_string(const std::string& val) const { json_value->SetString(val.c_str(), allocator); }
    void handle_bytes(const std::string& val) const { json_value->SetString(val.c_str(), allocator); }
};

struct ProtobufValueHandler {
    starrocks::CdcColumnValuePB* pb_value;

    void handle_null() const { pb_value->set_is_null(true); }
    void handle_bool(bool val) const { pb_value->set_bool_value(val); }
    void handle_int8(int8_t val) const { pb_value->set_int32_value(val); }
    void handle_uint8(uint8_t val) const { pb_value->set_int32_value(val); }
    void handle_int16(int16_t val) const { pb_value->set_int32_value(val); }
    void handle_uint16(uint16_t val) const { pb_value->set_int32_value(val); }
    void handle_int32(int32_t val) const { pb_value->set_int32_value(val); }
    void handle_uint32(uint32_t val) const { pb_value->set_int64_value(val); }
    void handle_int64(int64_t val) const { pb_value->set_int64_value(val); }
    void handle_uint64(uint64_t val) const { pb_value->set_string_value(std::to_string(val)); }
    void handle_float(float val) const { pb_value->set_float_value(val); }
    void handle_double(double val) const { pb_value->set_double_value(val); }
    void handle_string(const std::string& val) const { pb_value->set_string_value(val); }
    void handle_bytes(const std::string& val) const { pb_value->set_bytes_value(val); }
};

void CdcDataCollector::column_value_to_json(const Column* column, size_t row_idx, const TabletColumn& tablet_column,
                                            rapidjson::Value* json_value,
                                            rapidjson::Document::AllocatorType& allocator) {
    if (UNLIKELY(column == nullptr || row_idx >= column->size() || json_value == nullptr)) {
        json_value->SetNull();
        return;
    }

    if (column->is_null(row_idx)) {
        json_value->SetNull();
        return;
    }

    JsonValueHandler handler{json_value, allocator};
    convert_column_value_impl(column, row_idx, tablet_column, handler, "JSON serialization");
}

void CdcDataCollector::column_value_to_protobuf(const Column* column, size_t row_idx, const TabletColumn& tablet_column,
                                                starrocks::CdcColumnValuePB* pb_value) {
    if (UNLIKELY(column == nullptr || row_idx >= column->size() || pb_value == nullptr)) {
        pb_value->set_is_null(true);
        return;
    }

    if (column->is_null(row_idx)) {
        pb_value->set_is_null(true);
        return;
    }

    ProtobufValueHandler handler{pb_value};
    convert_column_value_impl(column, row_idx, tablet_column, handler, "Protobuf serialization");
}

} // namespace starrocks::lake
