// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include <queue>

#include "exec/pipeline/context_with_dependency.h"
#include "exprs/agg/aggregate_factory.h"
#include "exprs/expr.h"
#include "gen_cpp/Types_types.h"
#include "runtime/descriptors.h"
#include "runtime/types.h"
#include "util/runtime_profile.h"

namespace starrocks {

class ManagedFunctionStates;
using ManagedFunctionStatesPtr = std::unique_ptr<ManagedFunctionStates>;

struct FunctionTypes {
    TypeDescriptor result_type;
    bool has_nullable_child;
    bool is_nullable; // window function result whether is nullable
};

struct FrameRange {
    int64_t start;
    int64_t end;
};

class SegmentStatistics {
private:
    // We will not perform loop search until processing enough segments
    // segment canbe partition or peer group
    static constexpr int64_t MIN_SEGMENT_NUM = 16;

    // Overhead of binary search is O(N/S logN), where S denote the average size of segment
    // Overhead of loop search is O(N)
    // The default chunk_size is 4096, then logN turns out to be log(4096) = 12
    // Considering the error of estimation, we set the threshold to 8
    static constexpr int64_t AVERAGE_SIZE_THRESHOLD = 8;

public:
    void update(int64_t segment_size) {
        _count++;
        _cumulative_size += segment_size;
        _average_size = _cumulative_size / _count;
    }

    void reset() {
        _count = 0;
        _cumulative_size = 0;
        _average_size = 0;
    }

    bool is_high_cardinality() { return _count > MIN_SEGMENT_NUM && _average_size < AVERAGE_SIZE_THRESHOLD; }

    int64_t _count = 0;
    int64_t _cumulative_size = 0;
    int64_t _average_size = 0;
};

class Analytor;
using AnalytorPtr = std::shared_ptr<Analytor>;
using Analytors = std::vector<AnalytorPtr>;

// Component used to do analytic processing
// it contains common data struct and algorithm of analysis
class Analytor final : public pipeline::ContextWithDependency {
    friend class ManagedFunctionStates;

public:
    ~Analytor() {
        if (_state != nullptr) {
            close(_state);
        }
    }
    Analytor(const TPlanNode& tnode, const RowDescriptor& child_row_desc, const TupleDescriptor* result_tuple_desc);

    Status prepare(RuntimeState* state, ObjectPool* pool, RuntimeProfile* runtime_profile);
    Status open(RuntimeState* state);
    void close(RuntimeState* state) override;

    bool is_sink_complete() { return _is_sink_complete.load(std::memory_order_acquire); }
    void sink_complete() { _is_sink_complete.store(true, std::memory_order_release); }
    bool is_chunk_buffer_empty();
    vectorized::ChunkPtr poll_chunk_buffer();
    void offer_chunk_to_buffer(const vectorized::ChunkPtr& chunk);

    bool reached_limit() const { return _limit != -1 && _num_rows_returned >= _limit; }
    int64_t first_total_position_of_current_chunk() const {
        return _input_chunk_first_row_positions[_output_chunk_index];
    }
    size_t current_chunk_size() const;
    void update_input_rows(int64_t increment) { _input_rows += increment; }
    bool has_output() const { return _output_chunk_index < _input_chunks.size(); }
    bool is_current_chunk_finished_eval() { return _window_result_position >= current_chunk_size(); }
    int64_t window_result_position() const { return _window_result_position; }
    void set_window_result_position(int64_t window_result_position) {
        _window_result_position = window_result_position;
    }
    void update_window_result_position(int64_t increment) { _window_result_position += increment; }
    bool& input_eos() { return _input_eos; }

    int64_t current_row_position() const { return _current_row_position; }
    void update_current_row_position(int64_t increment) { _current_row_position += increment; }
    int64_t partition_start() const { return _partition_start; }
    int64_t partition_end() const { return _partition_end; }
    const std::pair<bool, int64_t>& found_partition_end() const { return _found_partition_end; }
    int64_t peer_group_start() const { return _peer_group_start; }
    int64_t peer_group_end() const { return _peer_group_end; }
    const std::pair<bool, int64_t>& found_peer_group_end() const { return _found_peer_group_end; }

    const std::vector<starrocks_udf::FunctionContext*>& agg_fn_ctxs() { return _agg_fn_ctxs; }
    const std::vector<std::vector<ExprContext*>>& agg_expr_ctxs() { return _agg_expr_ctxs; }
    const std::vector<std::vector<vectorized::ColumnPtr>>& agg_intput_columns() { return _agg_intput_columns; }

    const std::vector<ExprContext*>& partition_ctxs() { return _partition_ctxs; }
    const vectorized::Columns& partition_columns() { return _partition_columns; }

    const std::vector<ExprContext*>& order_ctxs() { return _order_ctxs; }
    const vectorized::Columns& order_columns() { return _order_columns; }

    RuntimeProfile::Counter* compute_timer() { return _compute_timer; }

    FrameRange get_sliding_frame_range();

    Status add_chunk(const vectorized::ChunkPtr& chunk);

    void update_window_batch(int64_t peer_group_start, int64_t peer_group_end, int64_t frame_start, int64_t frame_end);
    void reset_window_state();
    void get_window_function_result(size_t frame_start, size_t frame_end);

    Status output_result_chunk(vectorized::ChunkPtr* chunk);
    void create_agg_result_columns(int64_t chunk_size);

    bool is_new_partition();
    int64_t get_total_position(int64_t local_position);
    void find_partition_end();
    void find_peer_group_end();
    void reset_state_for_cur_partition();
    void reset_state_for_next_partition();

    void remove_unused_buffer_values(RuntimeState* state);

    bool need_partition_materializing() const { return _need_partition_materializing; }

#ifdef NDEBUG
    static constexpr int32_t BUFFER_CHUNK_NUMBER = 1000;
#else
    static constexpr int32_t BUFFER_CHUNK_NUMBER = 1;
#endif

private:
    RuntimeState* _state = nullptr;
    bool _is_closed = false;
    // TPlanNode is only valid in the PREPARE and INIT phase
    const TPlanNode& _tnode;
    const RowDescriptor& _child_row_desc;
    const TupleDescriptor* _result_tuple_desc;
    ObjectPool* _pool;
    std::unique_ptr<MemPool> _mem_pool;
    // The open phase still relies on the TFunction object for some initialization operations
    std::vector<TFunction> _fns;

    // only used in pipeline engine
    std::atomic<bool> _is_sink_complete = false;
    // only used in pipeline engine
    std::queue<vectorized::ChunkPtr> _buffer;
    std::mutex _buffer_mutex;

    RuntimeProfile* _runtime_profile;
    RuntimeProfile::Counter* _rows_returned_counter = nullptr;
    RuntimeProfile::Counter* _column_resize_timer = nullptr;
    RuntimeProfile::Counter* _compute_timer = nullptr;
    RuntimeProfile::Counter* _partition_search_timer = nullptr;
    RuntimeProfile::Counter* _peer_group_search_timer = nullptr;

    int64_t _num_rows_returned = 0;
    int64_t _limit; // -1: no limit
    bool _has_lead_lag_function = false;
    bool _is_range_with_start = false;

    vectorized::Columns _result_window_columns;
    std::vector<vectorized::ChunkPtr> _input_chunks;
    std::vector<int64_t> _input_chunk_first_row_positions;
    int64_t _input_rows = 0;
    int64_t _removed_from_buffer_rows = 0;
    int64_t _removed_chunk_index = 0;
    int64_t _output_chunk_index = 0;
    int64_t _window_result_position = 0;
    bool _input_eos = false;

    int64_t _current_row_position = 0;

    // Record the start pos of current partition.
    int64_t _partition_start = 0;
    // Record the end pos of current partition.
    // If the end position has not been found during the iteration, _partition_end = _partition_start.
    int64_t _partition_end = 0;
    // If first = true, then second record the first position of the latest Chunk that is not equal to the PartitionKey.
    // If first = false, then second points to the last position + 1.
    std::pair<bool, int64_t> _found_partition_end = {false, 0};
    SegmentStatistics _partition_statistics;
    std::queue<int64_t> _candidate_partition_ends;

    // A peer group is all of the rows that are peers within the specified ordering.
    // Record the start pos of current peer group.
    int64_t _peer_group_start = 0;
    // Record the end pos of current peer group.
    // If the end position has not been found during the iteration, _peer_group_end = _peer_group_start.
    int64_t _peer_group_end = 0;
    // If first = true, then second record the first position of the latest Chunk that is not equal to the peer group.
    // If first = false, then second points to the last position + 1.
    std::pair<bool, int64_t> _found_peer_group_end = {false, 0};
    SegmentStatistics _peer_group_statistics;
    std::queue<int64_t> _candidate_peer_group_ends;

    // Offset from the current row for ROWS windows with start or end bounds specified
    // with offsets. Is positive if the offset is FOLLOWING, negative if PRECEDING, and 0
    // if type is CURRENT ROW or UNBOUNDED PRECEDING/FOLLOWING.
    int64_t _rows_start_offset = 0;
    int64_t _rows_end_offset = 0;

    // The offset of the n-th window function in a row of window functions.
    std::vector<size_t> _agg_states_offsets;
    // The total size of the row for the window function state.
    size_t _agg_states_total_size = 0;
    // The max align size for all window aggregate state
    size_t _max_agg_state_align_size = 1;
    std::vector<starrocks_udf::FunctionContext*> _agg_fn_ctxs;
    std::vector<const vectorized::AggregateFunction*> _agg_functions;
    std::vector<ManagedFunctionStatesPtr> _managed_fn_states;
    std::vector<std::vector<ExprContext*>> _agg_expr_ctxs;
    std::vector<std::vector<vectorized::ColumnPtr>> _agg_intput_columns;
    std::vector<FunctionTypes> _agg_fn_types;

    std::vector<ExprContext*> _partition_ctxs;
    vectorized::Columns _partition_columns;

    std::vector<ExprContext*> _order_ctxs;
    vectorized::Columns _order_columns;

    // Tuple id of the buffered tuple (identical to the input child tuple, which is
    // assumed to come from a single SortNode). NULL if both partition_exprs and
    // order_by_exprs are empty.
    TTupleId _buffered_tuple_id = 0;

    bool _has_udaf = false;

    // Some window functions, eg. NTILE, need the boundary of partition to calculate its value.
    // For these functions, we must wait util the partition finished.
    bool _need_partition_materializing = false;

private:
    void _append_column(size_t chunk_size, vectorized::Column* dst_column, vectorized::ColumnPtr& src_column);
    void _update_window_batch_normal(int64_t peer_group_start, int64_t peer_group_end, int64_t frame_start,
                                     int64_t frame_end);
    // lead and lag function is special, the frame_start and frame_end
    // maybe less than zero.
    void _update_window_batch_lead_lag(int64_t peer_group_start, int64_t peer_group_end, int64_t frame_start,
                                       int64_t frame_end);

    int64_t _find_first_not_equal(vectorized::Column* column, int64_t target, int64_t start, int64_t end);
    void _find_candidate_partition_ends();
    void _find_candidate_peer_group_ends();
};

// Helper class that properly invokes destructor when state goes out of scope.
class ManagedFunctionStates {
public:
    ManagedFunctionStates(std::vector<starrocks_udf::FunctionContext*>* ctxs,
                          vectorized::AggDataPtr __restrict agg_states, Analytor* agg_node)
            : _ctxs(ctxs), _agg_states(agg_states), _agg_node(agg_node) {
        for (int i = 0; i < _agg_node->_agg_functions.size(); i++) {
            _agg_node->_agg_functions[i]->create((*_ctxs)[i], _agg_states + _agg_node->_agg_states_offsets[i]);
        }
    }

    ~ManagedFunctionStates() {
        for (int i = 0; i < _agg_node->_agg_functions.size(); i++) {
            _agg_node->_agg_functions[i]->destroy((*_ctxs)[i], _agg_states + _agg_node->_agg_states_offsets[i]);
        }
    }

    uint8_t* mutable_data() { return _agg_states; }
    const uint8_t* data() const { return _agg_states; }

private:
    std::vector<starrocks_udf::FunctionContext*>* _ctxs;
    vectorized::AggDataPtr _agg_states;
    Analytor* _agg_node;
};

class AnalytorFactory;
using AnalytorFactoryPtr = std::shared_ptr<AnalytorFactory>;
class AnalytorFactory {
public:
    AnalytorFactory(size_t dop, const TPlanNode& tnode, const RowDescriptor& child_row_desc,
                    const TupleDescriptor* result_tuple_desc)
            : _analytors(dop), _tnode(tnode), _child_row_desc(child_row_desc), _result_tuple_desc(result_tuple_desc) {}
    AnalytorPtr create(int i);

private:
    Analytors _analytors;
    const TPlanNode& _tnode;
    const RowDescriptor& _child_row_desc;
    const TupleDescriptor* _result_tuple_desc;
};
} // namespace starrocks
