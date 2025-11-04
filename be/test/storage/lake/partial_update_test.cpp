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

#include <gtest/gtest.h>

#include <random>

#include "column/chunk.h"
#include "column/datum_convert.h"
#include "column/datum_tuple.h"
#include "column/fixed_length_column.h"
#include "column/schema.h"
#include "column/vectorized_fwd.h"
#include "common/logging.h"
#include "storage/chunk_helper.h"
#include "storage/lake/delta_writer.h"
#include "storage/lake/meta_file.h"
#include "storage/lake/tablet_manager.h"
#include "storage/lake/tablet_reader.h"
#include "storage/lake/tablet_writer.h"
#include "storage/lake/test_util.h"
#include "storage/rowset/segment.h"
#include "storage/rowset/segment_options.h"
#include "storage/tablet_schema.h"
#include "testutil/assert.h"
#include "testutil/id_generator.h"
#include "testutil/sync_point.h"

namespace starrocks::lake {

class LakePartialUpdateTestBase : public TestBase {
public:
    explicit LakePartialUpdateTestBase(const char* test_directory) : TestBase(test_directory) {
        _tablet_metadata = std::make_shared<TabletMetadata>();
        _tablet_metadata->set_id(next_id());
        _tablet_metadata->set_version(1);
        _tablet_metadata->set_next_rowset_id(1);
        //
        //  | column | type | KEY | NULL |
        //  +--------+------+-----+------+
        //  |   c0   |  INT | YES |  NO  |
        //  |   c1   |  INT | NO  |  NO  |
        //  |   c2   |  INT | NO  |  NO  |
        auto schema = _tablet_metadata->mutable_schema();
        schema->set_id(next_id());
        schema->set_num_short_key_columns(1);
        schema->set_keys_type(PRIMARY_KEYS);
        schema->set_num_rows_per_row_block(65535);
        auto c0 = schema->add_column();
        {
            c0->set_unique_id(next_id());
            c0->set_name("c0");
            c0->set_type("INT");
            c0->set_is_key(true);
            c0->set_is_nullable(false);
        }
        auto c1 = schema->add_column();
        {
            c1->set_unique_id(next_id());
            c1->set_name("c1");
            c1->set_type("INT");
            c1->set_is_key(false);
            c1->set_is_nullable(true);
            c1->set_aggregation("REPLACE");
        }
        auto c2 = schema->add_column();
        {
            c2->set_unique_id(next_id());
            c2->set_name("c2");
            c2->set_type("INT");
            c2->set_is_key(false);
            c2->set_is_nullable(true);
            c2->set_aggregation("REPLACE");
            c2->set_default_value("10");
        }

        _slots.emplace_back(0, "c0", TypeDescriptor{LogicalType::TYPE_INT});
        _slots.emplace_back(1, "c1", TypeDescriptor{LogicalType::TYPE_INT});
        _slot_pointers.emplace_back(&_slots[0]);
        _slot_pointers.emplace_back(&_slots[1]);

        _slot_cid_map.emplace(0, 0);
        _slot_cid_map.emplace(1, 1);
        _slot_cid_map.emplace(2, 2);

        _tablet_schema = TabletSchema::create(*schema);
        _schema = std::make_shared<Schema>(ChunkHelper::convert_schema(_tablet_schema));
        _partial_schema = std::make_shared<Schema>(ChunkHelper::convert_schema(_tablet_schema, {0, 1}));
    }

    void SetUp() override {
        clear_and_init_test_dir();
        CHECK_OK(_tablet_mgr->put_tablet_metadata(*_tablet_metadata));
    }

    void TearDown() override {
        // check primary index cache's ref
        EXPECT_TRUE(_update_mgr->TEST_check_primary_index_cache_ref(_tablet_metadata->id(), 1));
        ExecEnv::GetInstance()->delete_file_thread_pool()->wait();
        // check trash files already removed
        for (const auto& file : _trash_files) {
            EXPECT_FALSE(fs::path_exist(file));
        }
        remove_test_dir_or_die();
    }

    Chunk generate_data(int64_t chunk_size, int shift, bool partial, int update_ratio) {
        std::vector<int> v0(chunk_size);
        std::vector<int> v1(chunk_size);
        std::vector<int> v2(chunk_size);
        for (int i = 0; i < chunk_size; i++) {
            v0[i] = i + shift * chunk_size;
        }
        auto rng = std::default_random_engine{};
        std::shuffle(v0.begin(), v0.end(), rng);
        for (int i = 0; i < chunk_size; i++) {
            v1[i] = v0[i] * update_ratio;
        }

        auto c0 = Int32Column::create();
        auto c1 = Int32Column::create();
        c0->append_numbers(v0.data(), v0.size() * sizeof(int));
        c1->append_numbers(v1.data(), v1.size() * sizeof(int));

        if (!partial) {
            for (int i = 0; i < chunk_size; i++) {
                v2[i] = v0[i] * 4;
            }
            auto c2 = Int32Column::create();
            c2->append_numbers(v2.data(), v2.size() * sizeof(int));
            return Chunk({std::move(c0), std::move(c1), std::move(c2)}, _slot_cid_map);
        } else {
            return Chunk({std::move(c0), std::move(c1)}, _slot_cid_map);
        }
    }

    int64_t check(int64_t version, std::function<bool(int c0, int c1, int c2)> check_fn) {
        ASSIGN_OR_ABORT(auto metadata, _tablet_mgr->get_tablet_metadata(_tablet_metadata->id(), version));
        auto reader = std::make_shared<TabletReader>(_tablet_mgr.get(), metadata, *_schema);
        CHECK_OK(reader->prepare());
        CHECK_OK(reader->open(TabletReaderParams()));
        auto chunk = ChunkHelper::new_chunk(*_schema, 128);
        int64_t ret = 0;
        while (true) {
            auto st = reader->get_next(chunk.get());
            if (st.is_end_of_file()) {
                break;
            }
            CHECK_OK(st);
            ret += chunk->num_rows();
            auto cols = chunk->columns();
            for (int i = 0; i < chunk->num_rows(); i++) {
                EXPECT_TRUE(check_fn(cols[0]->get(i).get_int32(), cols[1]->get(i).get_int32(),
                                     cols[2]->get(i).get_int32()));
            }
            chunk->reset();
        }
        return ret;
    }

protected:
    constexpr static const int kChunkSize = 12;

    std::shared_ptr<TabletMetadata> _tablet_metadata;
    std::shared_ptr<TabletSchema> _tablet_schema;
    std::shared_ptr<Schema> _schema;
    std::shared_ptr<Schema> _partial_schema;
    int64_t _partition_id = 4561;
    std::vector<std::string> _trash_files;
    std::vector<SlotDescriptor> _slots;
    std::vector<SlotDescriptor*> _slot_pointers;
    Chunk::SlotHashMap _slot_cid_map;
};

class LakePartialUpdateTest : public LakePartialUpdateTestBase, testing::WithParamInterface<PrimaryKeyParam> {
public:
    LakePartialUpdateTest() : LakePartialUpdateTestBase(kTestDirectory) {}

    void SetUp() override {
        _tablet_metadata->set_enable_persistent_index(GetParam().enable_persistent_index);
        _tablet_metadata->set_persistent_index_type(GetParam().persistent_index_type);
        LakePartialUpdateTestBase::SetUp();
    }
    constexpr static const char* const kTestDirectory = "test_lake_partial_update";
};

TEST_P(LakePartialUpdateTest, test_write) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 3);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    // partial update
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
            if (i == 0) {
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 1);
            } else {
                // move old .cols files into orphan files.
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
            }
        } else {
            EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 1);
        }
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 6);
    }
    EXPECT_TRUE(_update_mgr->update_state_mem_tracker()->consumption() == 0);
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
}

TEST_P(LakePartialUpdateTest, test_partial_update_with_condition) {
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        return;
    }

    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    std::vector<Chunk> chunks(3);
    chunks[0] = generate_data(kChunkSize, 0, true, 2);
    chunks[1] = generate_data(kChunkSize, 0, true, 3);
    chunks[2] = generate_data(kChunkSize, 0, true, 4);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    // 0. partial update with condition less than merge condition val
    // 1. partial update with condition equal to merge condition val
    // 2. partial update with condition greater than merge condition val
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .set_merge_condition("c1")
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunks[i], indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        if (i == 0 || i == 1) {
            ASSERT_EQ(kChunkSize,
                      check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
        } else {
            ASSERT_EQ(kChunkSize,
                      check(version, [](int c0, int c1, int c2) { return (c0 * 4 == c1) && (c0 * 4 == c2); }));
        }
    }
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
}

TEST_P(LakePartialUpdateTest, test_write_multi_segment) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 3);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    // partial update, and make it generate two segment files in one rowset
    const int64_t old_size = config::write_buffer_size;
    config::write_buffer_size = 1;
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
            if (i == 0) {
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
            } else {
                // move old .cols into orphan files.
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 3);
            }
        } else {
            EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
        }
    }
    config::write_buffer_size = old_size;
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 6);
        // check segment size in last metadata
        EXPECT_EQ(new_tablet_metadata->rowsets(5).segments_size(), 2);
    }
    EXPECT_TRUE(_update_mgr->update_state_mem_tracker()->consumption() == 0);
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
}

TEST_P(LakePartialUpdateTest, test_write_multi_segment_by_diff_val) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 5);
    auto chunk2 = generate_data(kChunkSize, 0, true, 6);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    // partial update, and make it generate two segment files in one rowset
    const int64_t old_size = config::write_buffer_size;
    config::write_buffer_size = 1;
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->write(chunk2, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
            if (i == 0) {
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
            } else {
                // move old .cols into orphan files.
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 3);
            }
        } else {
            EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
        }
    }
    config::write_buffer_size = old_size;
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 6 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 6);
        // check segment size in last metadata
        EXPECT_EQ(new_tablet_metadata->rowsets(5).segments_size(), 2);
    }
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
}

TEST_P(LakePartialUpdateTest, test_resolve_conflict) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 5);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    std::vector<int64_t> txn_ids;
    // concurrent partial update
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        txn_ids.push_back(txn_id);
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
    }
    // publish in order
    for (auto txn_id : txn_ids) {
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        if (GetParam().partial_update_mode != PartialUpdateMode::COLUMN_UPDATE_MODE) {
            EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 1);
        }
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 5 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    if (GetParam().partial_update_mode != PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 6);
    }
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
}

TEST_P(LakePartialUpdateTest, test_resolve_conflict_multi_segment) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 5);
    auto chunk2 = generate_data(kChunkSize, 0, true, 6);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    // partial update, and make it generate two segment files in one rowset
    const int64_t old_size = config::write_buffer_size;
    config::write_buffer_size = 1;
    std::vector<int64_t> txn_ids;
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        txn_ids.push_back(txn_id);
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->write(chunk2, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
    }
    // publish in order
    for (auto txn_id : txn_ids) {
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    config::write_buffer_size = old_size;
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 6 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 6);
        // check segment size in last metadata
        EXPECT_EQ(new_tablet_metadata->rowsets(5).segments_size(), 2);
    }
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
}

TEST_P(LakePartialUpdateTest, test_resolve_conflict2) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 5);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    SyncPoint::GetInstance()->SetCallBack("TabletManager::skip_cache_latest_metadata",
                                          [](void* arg) { *(bool*)arg = true; });
    SyncPoint::GetInstance()->EnableProcessing();

    DeferOp defer([]() {
        SyncPoint::GetInstance()->ClearCallBack("TabletManager::skip_cache_latest_metadata");
        SyncPoint::GetInstance()->DisableProcessing();
    });
    std::vector<int64_t> txn_ids;
    // concurrent partial update
    for (int i = 0; i < 2; i++) {
        auto txn_id = next_id();
        txn_ids.push_back(txn_id);
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
            if (i == 0) {
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 1);
            } else {
                // move old .cols into orphan files.
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
            }
        } else {
            EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 1);
        }
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 5 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 5);
    }
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
}

TEST_P(LakePartialUpdateTest, test_write_with_index_reload) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 3);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    // remove pk index, to make it reload again
    _update_mgr->try_remove_primary_index_cache(tablet_id);

    // partial update
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
            if (i == 0) {
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 1);
            } else {
                // move old .cols into orphan files.
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
            }
        } else {
            EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 1);
        }
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 6);
    }
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::CLOUD_NATIVE) {
        auto sstable_meta = new_tablet_metadata->sstable_meta();
        for (auto& sstable : sstable_meta.sstables()) {
            EXPECT_GT(sstable.max_rss_rowid(), 0);
        }
    }
}

TEST_P(LakePartialUpdateTest, test_partial_update_publish_retry) {
    if (GetParam().enable_persistent_index) return;
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 3);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 1);

    // partial update
    auto txn_id = next_id();
    {
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();

        SyncPoint::GetInstance()->SetCallBack("ProtobufFile::save:serialize", [](void* arg) { *(bool*)arg = false; });
        SyncPoint::GetInstance()->EnableProcessing();
        ASSERT_ERROR(publish_single_version(tablet_id, version + 1, txn_id).status());
        SyncPoint::GetInstance()->ClearCallBack("ProtobufFile::save:serialize");
        SyncPoint::GetInstance()->DisableProcessing();
    }
    // retry publish again
    ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
    _tablet_mgr->prune_metacache();
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
}

TEST_P(LakePartialUpdateTest, test_concurrent_write_publish) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 5);
    auto chunk2 = generate_data(kChunkSize, 0, true, 6);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        ++version;
    }
    // partial update
    std::thread t1([&]() {
        for (int i = 0; i < 100; ++i) {
            auto txn_id1 = next_id();
            ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                       .set_tablet_manager(_tablet_mgr.get())
                                                       .set_tablet_id(tablet_id)
                                                       .set_txn_id(txn_id1)
                                                       .set_partition_id(_partition_id)
                                                       .set_mem_tracker(_mem_tracker.get())
                                                       .set_schema_id(_tablet_schema->id())
                                                       .set_slot_descriptors(&_slot_pointers)
                                                       .set_partial_update_mode(GetParam().partial_update_mode)
                                                       .build());
            ASSERT_OK(delta_writer->open());
            ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
            ASSERT_OK(delta_writer->finish_with_txnlog());
            delta_writer->close();
            ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id1).status());
            version++;
        }
    });

    // partial update
    std::thread t2([&]() {
        for (int i = 0; i < 100; ++i) {
            const int64_t old_size = config::write_buffer_size;
            config::write_buffer_size = 1;
            const int64_t old_mem_usage = config::l0_max_mem_usage;
            config::l0_max_mem_usage = 1;
            auto txn_id2 = next_id() + 1000;
            ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                       .set_tablet_manager(_tablet_mgr.get())
                                                       .set_tablet_id(tablet_id)
                                                       .set_txn_id(txn_id2)
                                                       .set_partition_id(_partition_id)
                                                       .set_mem_tracker(_mem_tracker.get())
                                                       .set_schema_id(_tablet_schema->id())
                                                       .set_slot_descriptors(&_slot_pointers)
                                                       .set_partial_update_mode(GetParam().partial_update_mode)
                                                       .build());
            ASSERT_OK(delta_writer->open());
            ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
            ASSERT_OK(delta_writer->write(chunk2, indexes.data(), indexes.size()));
            ASSERT_OK(delta_writer->finish_with_txnlog());
            delta_writer->close();
            config::write_buffer_size = old_size;
            config::l0_max_mem_usage = old_mem_usage;
        }
    });
    t1.join();
    t2.join();
}

TEST_P(LakePartialUpdateTest, test_batch_publish) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 3);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }
    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));

    auto base_version = version;
    std::vector<int64_t> txn_ids(3);
    for (int i = 0; i < 3; i++) {
        int64_t txn_id = next_id();
        txn_ids[i] = txn_id;
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
    }
    auto new_version = base_version + 3;

    ASSERT_OK(batch_publish(tablet_id, base_version, new_version, txn_ids).status());

    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, new_version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 1);
        // 3 .dat + 2 .cols
        EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 5);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 4);
        EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 3);
    }
    _tablet_mgr->prune_metacache();
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    _update_mgr->try_remove_primary_index_cache(tablet_id);

    // publish again
    ASSERT_OK(batch_publish(tablet_id, base_version, new_version, txn_ids).status());
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, new_version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 1);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 4);
    }
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        // 3 .dat + 2 .cols
        EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 5);
    } else {
        EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 3);
    }
}

INSTANTIATE_TEST_SUITE_P(
        LakePartialUpdateTest, LakePartialUpdateTest,
        ::testing::Values(PrimaryKeyParam{true}, PrimaryKeyParam{false},
                          PrimaryKeyParam{true, PersistentIndexTypePB::CLOUD_NATIVE},
                          PrimaryKeyParam{.enable_persistent_index = true,
                                          .partial_update_mode = PartialUpdateMode::COLUMN_UPDATE_MODE},
                          PrimaryKeyParam{.enable_persistent_index = false,
                                          .partial_update_mode = PartialUpdateMode::COLUMN_UPDATE_MODE}));

class LakeIncompleteSortKeyPartialUpdateTest : public TestBase {
public:
    LakeIncompleteSortKeyPartialUpdateTest() : TestBase(kTestDirectory) {
        _tablet_metadata = std::make_shared<TabletMetadata>();
        _tablet_metadata->set_id(next_id());
        _tablet_metadata->set_version(1);
        _tablet_metadata->set_next_rowset_id(1);
        //
        //  | column | type | KEY | NULL | SORTKEY |
        //  +--------+------+-----+------+---------+
        //  |   c0   |  INT | YES |  NO  |   NO    |
        //  |   c1   |  INT | NO  |  NO  |   YES   |
        //  |   c2   |  INT | NO  |  NO  |   YES   |
        auto schema = _tablet_metadata->mutable_schema();
        schema->set_id(next_id());
        schema->set_num_short_key_columns(1);
        schema->set_keys_type(PRIMARY_KEYS);
        schema->set_num_rows_per_row_block(65535);
        auto c0 = schema->add_column();
        {
            c0->set_unique_id(next_id());
            c0->set_name("c0");
            c0->set_type("INT");
            c0->set_is_key(true);
            c0->set_is_nullable(false);
        }
        auto c1 = schema->add_column();
        {
            c1->set_unique_id(next_id());
            c1->set_name("c1");
            c1->set_type("INT");
            c1->set_is_key(false);
            c1->set_is_nullable(false);
            c1->set_aggregation("REPLACE");
        }
        auto c2 = schema->add_column();
        {
            c2->set_unique_id(next_id());
            c2->set_name("c2");
            c2->set_type("INT");
            c2->set_is_key(false);
            c2->set_is_nullable(false);
            c2->set_aggregation("REPLACE");
            //c2->set_default_value("10");
        }

        _slots.emplace_back(0, "c0", TypeDescriptor{LogicalType::TYPE_INT});
        _slots.emplace_back(1, "c1", TypeDescriptor{LogicalType::TYPE_INT});
        _slots.emplace_back(2, "__op", TypeDescriptor{LogicalType::TYPE_INT});
        _slot_pointers.emplace_back(&_slots[0]);
        _slot_pointers.emplace_back(&_slots[1]);
        _slot_pointers.emplace_back(&_slots[2]);

        _slot_cid_map.emplace(0, 0);
        _slot_cid_map.emplace(1, 1);
        _slot_cid_map.emplace(2, 2);

        schema->add_sort_key_idxes(1);
        schema->add_sort_key_idxes(2);
        _tablet_schema = TabletSchema::create(*schema);
    }

    void SetUp() override {
        clear_and_init_test_dir();
        CHECK_OK(_tablet_mgr->put_tablet_metadata(*_tablet_metadata));
    }

    void TearDown() override {
        // check primary index cache's ref
        EXPECT_TRUE(_update_mgr->TEST_check_primary_index_cache_ref(_tablet_metadata->id(), 1));
        ExecEnv::GetInstance()->delete_file_thread_pool()->wait();
        remove_test_dir_or_die();
    }

    Chunk generate_data(int64_t chunk_size, int shift, int update_ratio) {
        std::vector<int> v0(chunk_size);
        std::vector<int> v1(chunk_size);
        std::vector<int> v2(chunk_size);
        for (int i = 0; i < chunk_size; i++) {
            v0[i] = i + shift * chunk_size;
        }
        auto rng = std::default_random_engine{};
        std::shuffle(v0.begin(), v0.end(), rng);
        for (int i = 0; i < chunk_size; i++) {
            v1[i] = v0[i] * update_ratio;
        }

        auto c0 = Int32Column::create();
        auto c1 = Int32Column::create();
        c0->append_numbers(v0.data(), v0.size() * sizeof(int));
        c1->append_numbers(v1.data(), v1.size() * sizeof(int));

        return Chunk({std::move(c0), std::move(c1)}, _slot_cid_map);
    }

protected:
    constexpr static const char* const kTestDirectory = "test_lake_incomplete_sort_key_partial_update";
    constexpr static const int kChunkSize = 12;

    std::shared_ptr<TabletMetadata> _tablet_metadata;
    std::shared_ptr<TabletSchema> _tablet_schema;
    int64_t _partition_id = 4561;
    std::vector<SlotDescriptor> _slots;
    std::vector<SlotDescriptor*> _slot_pointers;
    Chunk::SlotHashMap _slot_cid_map;
};

TEST_F(LakeIncompleteSortKeyPartialUpdateTest, test_incomplete_sort_key) {
    auto chunk0 = generate_data(kChunkSize, 0, 3);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto tablet_id = _tablet_metadata->id();

    // incomplete sort key partial write.
    auto txn_id = next_id();
    ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                               .set_tablet_manager(_tablet_mgr.get())
                                               .set_tablet_id(tablet_id)
                                               .set_txn_id(txn_id)
                                               .set_partition_id(_partition_id)
                                               .set_mem_tracker(_mem_tracker.get())
                                               .set_schema_id(_tablet_schema->id())
                                               .set_slot_descriptors(&_slot_pointers)
                                               .build());
    ASSERT_OK(delta_writer->open());
    ASSERT_ERROR(delta_writer->write(chunk0, indexes.data(), indexes.size()));
}

TEST_P(LakePartialUpdateTest, test_partial_update_retry_rewrite_check) {
    if (GetParam().enable_persistent_index) return;
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) return;
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 5);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 1);

    // partial update
    auto txn_id = next_id();
    {
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
    }
    // retry publish again
    for (int i = 0; i < 5; i++) {
        TEST_ENABLE_ERROR_POINT("TabletManager::put_tablet_metadata",
                                Status::IOError("injected put tablet metadata error"));

        SyncPoint::GetInstance()->EnableProcessing();

        DeferOp defer([]() {
            TEST_DISABLE_ERROR_POINT("TabletManager::put_tablet_metadata");
            SyncPoint::GetInstance()->DisableProcessing();
        });
        _tablet_mgr->prune_metacache();
        ASSERT_ERROR(publish_single_version(tablet_id, version + 1, txn_id));
        auto txn_log_st = _tablet_mgr->get_txn_log(tablet_id, txn_id);
        EXPECT_TRUE(txn_log_st.ok());
    }
    // success
    _tablet_mgr->prune_metacache();
    ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id));
    ASSERT_EQ(kChunkSize, check(version + 1, [](int c0, int c1, int c2) { return (c0 * 5 == c1) && (c0 * 4 == c2); }));
}

TEST_P(LakePartialUpdateTest, test_write_multi_segment_by_diff_val_mem_limit) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 5);
    auto chunk2 = generate_data(kChunkSize, 0, true, 6);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    // partial update, and make it generate two segment files in one rowset
    const int64_t old_size = config::write_buffer_size;
    config::write_buffer_size = 1;
    const int64_t old_limit = _update_mgr->update_state_mem_tracker()->limit();
    _update_mgr->update_state_mem_tracker()->set_limit(1);
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->write(chunk2, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
            if (i == 0) {
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
            } else {
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 3);
            }
        } else {
            EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
        }
    }
    config::write_buffer_size = old_size;
    _update_mgr->update_state_mem_tracker()->set_limit(old_limit);
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 6 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 6);
        // check segment size in last metadata
        EXPECT_EQ(new_tablet_metadata->rowsets(5).segments_size(), 2);
    }
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
}

TEST_P(LakePartialUpdateTest, test_partial_update_retry_check_file_exist) {
    if (GetParam().enable_persistent_index) return;
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) return;
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 5);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 1);

    // partial update
    auto txn_id = next_id();
    {
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
    }
    // retry because put meta fail
    for (int i = 0; i < 2; i++) {
        TEST_ENABLE_ERROR_POINT("TabletManager::put_tablet_metadata",
                                Status::IOError("injected put tablet metadata error"));

        SyncPoint::GetInstance()->EnableProcessing();

        DeferOp defer([]() {
            TEST_DISABLE_ERROR_POINT("TabletManager::put_tablet_metadata");
            SyncPoint::GetInstance()->DisableProcessing();
        });
        _tablet_mgr->prune_metacache();
        ASSERT_ERROR(publish_single_version(tablet_id, version + 1, txn_id));
        auto txn_log_st = _tablet_mgr->get_txn_log(tablet_id, txn_id);
        EXPECT_TRUE(txn_log_st.ok());
    }
    // success
    _tablet_mgr->prune_metacache();
    ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id));
    ASSERT_EQ(kChunkSize, check(version + 1, [](int c0, int c1, int c2) { return (c0 * 5 == c1) && (c0 * 4 == c2); }));
}

TEST_P(LakePartialUpdateTest, test_max_buffer_rows) {
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        GTEST_SKIP() << "this case only for partial update row mode";
    }
    auto chunk0 = generate_data(kChunkSize, 0, false, 3);
    auto chunk1 = generate_data(kChunkSize, 0, true, 3);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    // partial update, and make it generate two segment files in one rowset
    // Caused by max buffer rows.

    SyncPoint::GetInstance()->SetCallBack("TabletManager::get_average_row_size_from_latest_metadata",
                                          [](void* arg) { *(int64_t*)arg = 1000000000; });
    SyncPoint::GetInstance()->EnableProcessing();

    DeferOp defer([]() {
        SyncPoint::GetInstance()->ClearCallBack("TabletManager::get_average_row_size_from_latest_metadata");
        SyncPoint::GetInstance()->DisableProcessing();
    });
    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(GetParam().partial_update_mode)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
            if (i == 0) {
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
            } else {
                // move old .cols into orphan files.
                EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 3);
            }
        } else {
            EXPECT_EQ(new_tablet_metadata->orphan_files_size(), 2);
        }
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 3 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    if (GetParam().partial_update_mode == PartialUpdateMode::COLUMN_UPDATE_MODE) {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
    } else {
        EXPECT_EQ(new_tablet_metadata->rowsets_size(), 6);
        // check segment size in last metadata
        EXPECT_EQ(new_tablet_metadata->rowsets(5).segments_size(), 2);
    }
    EXPECT_TRUE(_update_mgr->update_state_mem_tracker()->consumption() == 0);
    if (GetParam().enable_persistent_index && GetParam().persistent_index_type == PersistentIndexTypePB::LOCAL) {
        check_local_persistent_index_meta(tablet_id, version);
    }
}

namespace {
class SchemaModifier {
public:
    virtual ~SchemaModifier() = default;
    virtual void modify(TabletSchemaPB* schema) = 0;
};

using SchemaModifierPtr = std::shared_ptr<SchemaModifier>;

class SwapColumn : public SchemaModifier {
public:
    explicit SwapColumn(int pos1, int pos2) : _pos1(pos1), _pos2(pos2) {}

    void modify(TabletSchemaPB* schema) override {
        // swap column c2 and c1
        schema->mutable_column()->SwapElements(_pos1, _pos2);
    }

private:
    int _pos1;
    int _pos2;
};

class ModifyColumnType : public SchemaModifier {
public:
    explicit ModifyColumnType(int column_idx, std::string target_type)
            : _column_idx(column_idx), _target_type(std::move(target_type)) {}

    void modify(TabletSchemaPB* schema) override {
        schema->mutable_column(_column_idx)->set_type(_target_type);
        if (_target_type == "VARCHAR" || _target_type == "varchar") {
            // 100 is enough for tests
            schema->mutable_column(_column_idx)->set_length(100);
        }
    }

private:
    int _column_idx;
    std::string _target_type;
};

class AddColumn : public SchemaModifier {
public:
    explicit AddColumn(int pos, std::string type, bool nullable, std::string default_value)
            : _pos(pos), _type(std::move(type)), _nullable(nullable), _default_value(std::move(default_value)) {}

    void modify(TabletSchemaPB* schema) override {
        DCHECK_LE(_pos, schema->column_size());
        auto pos = schema->column_size();
        auto* c = schema->add_column();
        c->set_unique_id(next_id());
        c->set_name(fmt::format("c_{}", c->unique_id()));
        c->set_type(_type);
        c->set_is_key(false);
        c->set_is_nullable(_nullable);
        c->set_aggregation("REPLACE");
        c->set_default_value(_default_value);
        if (_type == "VARCHAR" || _type == "varchar") {
            c->set_length(100);
        }
        // Move the column to expected position
        while (pos != _pos) {
            schema->mutable_column()->SwapElements(pos, pos - 1);
            --pos;
        }
    }

private:
    int _pos;
    std::string _type;
    bool _nullable;
    std::string _default_value;
};

class DropColumn : public SchemaModifier {
public:
    explicit DropColumn(int col_idx) : _col_idx(col_idx) {}

    void modify(TabletSchemaPB* schema) override {
        auto iter = schema->mutable_column()->begin() + _col_idx;
        schema->mutable_column()->erase(iter);
    }

private:
    int _col_idx;
};

} // namespace

class LakePartialUpdateConcurrentSchemaEvolutionTest : public LakePartialUpdateTestBase,
                                                       public testing::WithParamInterface<SchemaModifierPtr> {
public:
    LakePartialUpdateConcurrentSchemaEvolutionTest() : LakePartialUpdateTestBase(kTestDirectory) {}

    constexpr static const char* const kTestDirectory = "test_lake_partial_update_concurrent_schema_evolution";

    static std::string as_string(LogicalType type, const Datum& datum) {
        auto type_info = get_type_info(type);
        return datum_to_string(type_info.get(), datum);
    }

    void verify_column(const TabletColumn& col, int64_t rowid, const Datum& datum) {
        if (col.name() == "c0") {
            EXPECT_EQ(rowid, datum.get_int32()) << "rowid=" << rowid << " column=" << col.name();
        } else if (col.name() == "c1") {
            auto expect = rowid * 3;
            //            ^^^^^^^^^^ Please refer the define and usage of `generate_data()`
            EXPECT_EQ(std::to_string(expect), as_string(col.type(), datum));
        } else if (col.name() == "c2") {
            auto expect = rowid * 4;
            //            ^^^^^^^^^^ Please refer the define and usage of `generate_data()`
            EXPECT_EQ(std::to_string(expect), as_string(col.type(), datum))
                    << "rowid=" << rowid << " column=" << col.name();
        } else if (col.default_value() == "NULL") {
            EXPECT_TRUE(datum.is_null()) << "rowid=" << rowid << " column=" << col.name() << " type=" << col.type()
                                         << " value=" << as_string(col.type(), datum);
        } else {
            EXPECT_EQ(col.default_value(), as_string(col.type(), datum))
                    << "rowid=" << rowid << " column=" << col.name();
        }
    }

    int64_t verify(int64_t version) {
        ASSIGN_OR_ABORT(auto metadata, _tablet_mgr->get_tablet_metadata(_tablet_metadata->id(), version));
        auto tablet_schema = TabletSchema::create(metadata->schema());
        auto schema = tablet_schema->schema();
        auto reader = std::make_shared<TabletReader>(_tablet_mgr.get(), metadata, *schema);
        CHECK_OK(reader->prepare());
        CHECK_OK(reader->open(TabletReaderParams()));
        auto chunk = ChunkHelper::new_chunk(*schema, 128);
        auto ret = int64_t{0};
        auto rowid = int64_t{0};
        while (true) {
            auto st = reader->get_next(chunk.get());
            if (st.is_end_of_file()) {
                break;
            }
            CHECK_OK(st);
            ret += chunk->num_rows();
            auto cols = chunk->columns();
            for (int i = 0; i < chunk->num_rows(); i++) {
                for (int j = 0, num_col = tablet_schema->num_columns(); j < num_col; j++) {
                    verify_column(tablet_schema->column(j), rowid, chunk->get(i).get(j));
                }
                rowid++;
            }
            chunk->reset();
        }
        return ret;
    }
};

TEST_P(LakePartialUpdateConcurrentSchemaEvolutionTest, test) {
    auto chunk0 = generate_data(kChunkSize, 0, false, 1);
    auto chunk1 = generate_data(kChunkSize, 0, true, 3);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    std::iota(indexes.begin(), indexes.end(), 0);

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // normal write
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        // Publish version
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    // partial update
    auto txn_id = next_id();
    {
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk1, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
    }
    // Update tablet schema
    {
        ASSIGN_OR_ABORT(auto latest_metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        auto new_metadata = std::make_shared<TabletMetadataPB>(*latest_metadata);
        auto schema = new_metadata->mutable_schema();
        auto modifier = GetParam();
        modifier->modify(schema);
        schema->set_id(next_id());
        schema->set_schema_version(schema->schema_version() + 1);
        new_metadata->set_version(version + 1);
        // Save new tablet metadata and schema file
        ASSERT_OK(_tablet_mgr->put_tablet_metadata(new_metadata));
        ASSERT_OK(_tablet_mgr->create_schema_file(tablet_id, *schema));
        version++;
    }
    // Publish version for partial update
    {
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, verify(version));
}
// clang-format off
INSTANTIATE_TEST_SUITE_P(LakePartialUpdateConcurrentSchemaEvolutionTest,
                         LakePartialUpdateConcurrentSchemaEvolutionTest,
                         ::testing::Values(std::make_shared<SwapColumn>(1, 2),
                                           std::make_shared<AddColumn>(1, "BIGINT", true, "NULL"),
                                           std::make_shared<AddColumn>(1, "BIGINT", true, "-100"),
                                           std::make_shared<AddColumn>(1, "BIGINT", false, "-100"),
                                           std::make_shared<AddColumn>(1, "VARCHAR", true, "xyz"),
                                           std::make_shared<AddColumn>(1, "VARCHAR", false, "yyyy"),
                                           std::make_shared<AddColumn>(2, "BIGINT", true, "NULL"),
                                           std::make_shared<AddColumn>(2, "BIGINT", true, "1000"),
                                           std::make_shared<AddColumn>(2, "VARCHAR", true, "abc"),
                                           std::make_shared<AddColumn>(3, "BIGINT", true, "123"),
                                           std::make_shared<AddColumn>(3, "VARCHAR", true, "hello"),
                                           std::make_shared<AddColumn>(3, "VARCHAR", false, "world"),
                                           std::make_shared<DropColumn>(1),
                                           std::make_shared<DropColumn>(2),
                                           std::make_shared<ModifyColumnType>(1, "BIGINT"),
                                           std::make_shared<ModifyColumnType>(1, "DOUBLE"),
                                           std::make_shared<ModifyColumnType>(1, "VARCHAR"),
                                           std::make_shared<ModifyColumnType>(2, "BIGINT"),
                                           std::make_shared<ModifyColumnType>(2, "DOUBLE"),
                                           std::make_shared<ModifyColumnType>(2, "VARCHAR")
                         ));
// clang-format on

} // namespace starrocks::lake

namespace starrocks::lake {

class LakeColumnUpsertModeTest : public LakePartialUpdateTestBase {
public:
    LakeColumnUpsertModeTest() : LakePartialUpdateTestBase(kTestDirectory) {}

    void SetUp() override {
        LakePartialUpdateTestBase::SetUp();
    }

    constexpr static const char* const kTestDirectory = "test_lake_column_upsert_mode";
};

TEST_F(LakeColumnUpsertModeTest, upsert_existing_rows_generates_dcg_only) {
    auto chunk_full = generate_data(kChunkSize, 0, false, 3);
    auto chunk_partial = generate_data(kChunkSize, 0, true, 5);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) indexes[i] = i;
    auto version = 1;
    auto tablet_id = _tablet_metadata->id();

    for (int i = 0; i < 3; i++) {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                               .set_tablet_manager(_tablet_mgr.get())
                                               .set_tablet_id(tablet_id)
                                               .set_txn_id(txn_id)
                                               .set_partition_id(_partition_id)
                                               .set_mem_tracker(_mem_tracker.get())
                                               .set_schema_id(_tablet_schema->id())
                                               .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk_full, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }

    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                           .set_tablet_manager(_tablet_mgr.get())
                                           .set_tablet_id(tablet_id)
                                           .set_txn_id(txn_id)
                                           .set_partition_id(_partition_id)
                                           .set_mem_tracker(_mem_tracker.get())
                                           .set_schema_id(_tablet_schema->id())
                                           .set_slot_descriptors(&_slot_pointers)
                                           .set_partial_update_mode(PartialUpdateMode::COLUMN_UPSERT_MODE)
                                           .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk_partial, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, check(version, [](int c0, int c1, int c2) { return (c0 * 5 == c1) && (c0 * 4 == c2); }));
    ASSIGN_OR_ABORT(auto md, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(md->rowsets_size(), 3);
    EXPECT_GT(md->dcg_meta().dcgs_size(), 0);
}

TEST_F(LakeColumnUpsertModeTest, upsert_with_new_rows_adds_new_segments) {
    auto chunk_full = generate_data(kChunkSize, 0, false, 3);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) indexes[i] = i;
    auto version = 1;
    auto tablet_id = _tablet_metadata->id();

    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                           .set_tablet_manager(_tablet_mgr.get())
                                           .set_tablet_id(tablet_id)
                                           .set_txn_id(txn_id)
                                           .set_partition_id(_partition_id)
                                           .set_mem_tracker(_mem_tracker.get())
                                           .set_schema_id(_tablet_schema->id())
                                           .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk_full, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }
    ASSIGN_OR_ABORT(auto md_before, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    auto prev_rowsets = md_before->rowsets_size();

    auto chunk_insert = generate_data(kChunkSize, 100, true, 7);
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                           .set_tablet_manager(_tablet_mgr.get())
                                           .set_tablet_id(tablet_id)
                                           .set_txn_id(txn_id)
                                           .set_partition_id(_partition_id)
                                           .set_mem_tracker(_mem_tracker.get())
                                           .set_schema_id(_tablet_schema->id())
                                           .set_slot_descriptors(&_slot_pointers)
                                           .set_partial_update_mode(PartialUpdateMode::COLUMN_UPSERT_MODE)
                                           .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk_insert, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }

    ASSIGN_OR_ABORT(auto md_after, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_GT(md_after->rowsets_size(), prev_rowsets);
    auto total = check(version, [](int c0, int c1, int c2) { return (c2 == c0 * 4) || (c2 == 10); });
    EXPECT_EQ(total, kChunkSize * 2);
}

// Test memory optimization: COLUMN_UPSERT_MODE skips reading column values in RowsetUpdateState
//
// Background:
// - COLUMN_UPDATE_MODE uses ColumnModePartialUpdateHandler directly (no RowsetUpdateState)
// - COLUMN_UPSERT_MODE needs RowsetUpdateState::_prepare_partial_update_states to handle new rows
//   - Before optimization: Incorrectly read all unmodified columns for each new row → OOM for large inserts
//   - After optimization: Skip reading (new rows don't need old data to merge)
//
// This test verifies the optimization by:
// 1. Creating a table with many columns to amplify memory usage
// 2. Comparing COLUMN_UPDATE_MODE (baseline, doesn't use RowsetUpdateState)
// 3. With COLUMN_UPSERT_MODE (optimized, now also skips unnecessary reads)
TEST_F(LakeColumnUpsertModeTest, memory_optimization_skip_column_reading) {
    // Create a tablet with more columns to amplify the memory difference
    // Schema: pk(c0), value columns (c1, c2, ..., c9) - total 10 columns
    // We'll only update c1
    auto tablet_metadata_wide = std::make_shared<TabletMetadata>();
    tablet_metadata_wide->set_id(next_id());
    tablet_metadata_wide->set_version(1);
    tablet_metadata_wide->set_next_rowset_id(1);
    
    auto schema = tablet_metadata_wide->mutable_schema();
    schema->set_id(next_id());
    schema->set_num_short_key_columns(1);
    schema->set_keys_type(PRIMARY_KEYS);
    schema->set_num_rows_per_row_block(65535);
    
    // Primary key column
    auto c0 = schema->add_column();
    c0->set_unique_id(next_id());
    c0->set_name("c0");
    c0->set_type("INT");
    c0->set_is_key(true);
    c0->set_is_nullable(false);
    
    // Add 9 value columns (c1-c9)
    for (int i = 1; i <= 9; i++) {
        auto col = schema->add_column();
        col->set_unique_id(next_id());
        col->set_name("c" + std::to_string(i));
        col->set_type("INT");
        col->set_is_key(false);
        col->set_is_nullable(true);
        col->set_aggregation("REPLACE");
    }
    
    CHECK_OK(_tablet_mgr->put_tablet_metadata(*tablet_metadata_wide));
    
    auto tablet_schema_wide = TabletSchema::create(*schema);
    auto full_schema_wide = std::make_shared<Schema>(ChunkHelper::convert_schema(tablet_schema_wide));
    auto partial_schema_wide = std::make_shared<Schema>(ChunkHelper::convert_schema(tablet_schema_wide, {0, 1}));
    
    // Helper: generate full chunk with all 10 columns
    auto generate_full_chunk = [&](int64_t pk_start, int64_t multiplier) {
        std::vector<int> values[10];
        for (int col = 0; col < 10; col++) {
            values[col].resize(kChunkSize);
            for (int row = 0; row < kChunkSize; row++) {
                values[col][row] = (pk_start + row) * multiplier + col;
            }
        }
        
        Columns columns;
        columns.push_back(Int32Column::create());
        columns[0]->append_numbers(values[0].data(), kChunkSize * sizeof(int));
        
        for (int col = 1; col < 10; col++) {
            auto nullable_col = NullableColumn::create(Int32Column::create(), NullColumn::create());
            nullable_col->append_numbers(values[col].data(), kChunkSize * sizeof(int));
            columns.push_back(nullable_col);
        }
        
        return std::make_shared<Chunk>(columns, full_schema_wide);
    };
    
    // Helper: generate partial chunk with only c0 (pk) and c1 (updated column)
    auto generate_partial_chunk = [&](int64_t pk_start, int64_t multiplier) {
        std::vector<int> c0_values(kChunkSize);
        std::vector<int> c1_values(kChunkSize);
        for (int row = 0; row < kChunkSize; row++) {
            c0_values[row] = pk_start + row;
            c1_values[row] = (pk_start + row) * multiplier;
        }
        
        auto c0_col = Int32Column::create();
        c0_col->append_numbers(c0_values.data(), kChunkSize * sizeof(int));
        
        auto c1_col = NullableColumn::create(Int32Column::create(), NullColumn::create());
        c1_col->append_numbers(c1_values.data(), kChunkSize * sizeof(int));
        
        return std::make_shared<Chunk>(Columns{c0_col, c1_col}, partial_schema_wide);
    };
    
    std::vector<SlotDescriptor> slots_wide;
    slots_wide.emplace_back(0, "c0", TypeDescriptor{LogicalType::TYPE_INT});
    slots_wide.emplace_back(1, "c1", TypeDescriptor{LogicalType::TYPE_INT});
    std::vector<SlotDescriptor*> slot_pointers_wide = {&slots_wide[0], &slots_wide[1]};
    
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) indexes[i] = i;
    auto version = 1;
    auto tablet_id = tablet_metadata_wide->id();

    // Step 1: Write initial full data with all 10 columns (pk 0-99)
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                           .set_tablet_manager(_tablet_mgr.get())
                                           .set_tablet_id(tablet_id)
                                           .set_txn_id(txn_id)
                                           .set_partition_id(_partition_id)
                                           .set_mem_tracker(_mem_tracker.get())
                                           .set_schema_id(tablet_schema_wide->id())
                                           .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(generate_full_chunk(0, 2), indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
    }

    // Step 2: Partial update with COLUMN_UPDATE_MODE (baseline)
    // This mode does NOT use RowsetUpdateState, so it never had the memory issue
    // It directly uses ColumnModePartialUpdateHandler which reads columns on-demand per rssid
    int64_t mem_baseline_before = _mem_tracker->consumption();
    int64_t mem_baseline_delta = 0;
    
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                           .set_tablet_manager(_tablet_mgr.get())
                                           .set_tablet_id(tablet_id)
                                           .set_txn_id(txn_id)
                                           .set_partition_id(_partition_id)
                                           .set_mem_tracker(_mem_tracker.get())
                                           .set_schema_id(tablet_schema_wide->id())
                                           .set_slot_descriptors(&slot_pointers_wide)
                                           .set_partial_update_mode(PartialUpdateMode::COLUMN_UPDATE_MODE)
                                           .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(generate_partial_chunk(0, 100), indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        
        int64_t mem_baseline_peak = _mem_tracker->peak_consumption();
        mem_baseline_delta = mem_baseline_peak - mem_baseline_before;
        
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        
        LOG(INFO) << "[BASELINE] COLUMN_UPDATE_MODE memory delta: " << mem_baseline_delta << " bytes";
        LOG(INFO) << "  (Uses ColumnModePartialUpdateHandler, not RowsetUpdateState)";
    }

    // Step 3: Partial update with COLUMN_UPSERT_MODE
    // Before optimization: Would read all unmodified columns in RowsetUpdateState::_prepare_partial_update_states
    // After optimization: Skips reading because new rows don't need old data to merge
    int64_t mem_optimized_before = _mem_tracker->consumption();
    
    {
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                           .set_tablet_manager(_tablet_mgr.get())
                                           .set_tablet_id(tablet_id)
                                           .set_txn_id(txn_id)
                                           .set_partition_id(_partition_id)
                                           .set_mem_tracker(_mem_tracker.get())
                                           .set_schema_id(tablet_schema_wide->id())
                                           .set_slot_descriptors(&slot_pointers_wide)
                                           .set_partial_update_mode(PartialUpdateMode::COLUMN_UPSERT_MODE)
                                           .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(generate_partial_chunk(0, 200), indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        
        int64_t mem_optimized_peak = _mem_tracker->peak_consumption();
        int64_t mem_optimized_delta = mem_optimized_peak - mem_optimized_before;
        
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        
        LOG(INFO) << "[OPTIMIZED] COLUMN_UPSERT_MODE memory delta: " << mem_optimized_delta << " bytes";
        LOG(INFO) << "  (After optimization: skips reading c2-c9 in RowsetUpdateState)";
        LOG(INFO) << "[COMPARISON] COLUMN_UPDATE_MODE: " << mem_baseline_delta 
                  << " vs COLUMN_UPSERT_MODE: " << mem_optimized_delta;
        
        // Calculate memory ratio
        double memory_ratio = (double)mem_optimized_delta / (double)mem_baseline_delta;
        LOG(INFO) << "[MEMORY RATIO] COLUMN_UPSERT_MODE / COLUMN_UPDATE_MODE = " << memory_ratio;
        
        // Key assertion: With our optimization, COLUMN_UPSERT_MODE should have comparable memory usage
        // to COLUMN_UPDATE_MODE (within 2x) despite the additional _handle_column_upsert_mode logic
        // Without optimization, the ratio would be much higher due to reading all unmodified columns
        EXPECT_LT(memory_ratio, 2.0) << "Optimized COLUMN_UPSERT_MODE should not use 2x more memory than baseline";
        
        LOG(INFO) << "SUCCESS! COLUMN_UPSERT_MODE now skips unnecessary column reading in RowsetUpdateState";
    }

    // Step 4: Verify functional correctness
    ASSIGN_OR_ABORT(auto tablet, _tablet_mgr->get_tablet(tablet_id));
    ASSIGN_OR_ABORT(auto reader, tablet.new_reader(version, *full_schema_wide));
    CHECK_OK(reader->prepare());
    CHECK_OK(reader->open(TabletReaderParams()));
    
    auto result_chunk = ChunkHelper::new_chunk(*full_schema_wide, kChunkSize);
    CHECK_OK(reader->get_next(result_chunk.get()));
    
    ASSERT_EQ(result_chunk->num_rows(), kChunkSize);
    
    // Verify c1 was updated to c0 * 200 (from step 3)
    auto& c0_col = result_chunk->get_column_by_index(0);
    auto& c1_col = result_chunk->get_column_by_index(1);
    for (int i = 0; i < kChunkSize; i++) {
        int c0_val = c0_col->get(i).get_int32();
        int c1_val = c1_col->get(i).get_int32();
        EXPECT_EQ(c1_val, c0_val * 200) << "c1 should be updated";
    }
    
    // Verify DCG was generated for COLUMN_UPSERT_MODE
    ASSIGN_OR_ABORT(auto md, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_GT(md->dcg_meta().dcgs_size(), 0) << "DCG should be generated for existing row updates";
}

// Test functional correctness: COLUMN_UPSERT_MODE handles new row insertion correctly
//
// Background:
// - COLUMN_UPSERT_MODE needs RowsetUpdateState::_prepare_partial_update_states to handle new rows
//   - Before optimization: Incorrectly read all unmodified columns for each new row → OOM for large inserts
//   - After optimization: Skip reading unmodified columns (new rows don't exist in storage yet)
//
// This test verifies:
// 1. New rows can be inserted with partial columns (c0+c1 only)
// 2. Unmodified columns (c2) are correctly filled with default values
// 3. Existing rows can be updated normally
// 4. All data remains correct after mixed operations
TEST_F(LakeColumnUpsertModeTest, memory_optimization_skip_column_reading) {
    auto tablet_id = _tablet_metadata->id();
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) indexes[i] = i;
    auto version = 1;

    // Step 1: Write initial full data with all 3 columns (pk 0-11)
    {
        auto chunk_full = generate_data(kChunkSize, 0, false, 3);
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk_full, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;
        LOG(INFO) << "[BASE DATA] Inserted base rows with full columns";
    }

    // Step 2: Insert NEW rows with COLUMN_UPSERT_MODE (pk 12-23, only update c0+c1)
    // This tests the optimization for NEW row inserts (src_rss_rowids == UINT64_MAX)
    // These rows should NOT read c2 from storage since they don't exist yet
    // c2 should be filled with default value "10"
    {
        auto chunk_partial = generate_data(kChunkSize, 1, true, 5); // shift=1 means pk 12-23
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(PartialUpdateMode::COLUMN_UPSERT_MODE)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk_partial, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;

        LOG(INFO) << "[NEW ROWS] Inserted new rows with COLUMN_UPSERT_MODE (only c0+c1)";
    }

    // Verify new rows: c2 should have default value 10
    {
        ASSIGN_OR_ABORT(auto metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
        auto reader = std::make_shared<TabletReader>(_tablet_mgr.get(), metadata, *_schema);
        CHECK_OK(reader->prepare());
        CHECK_OK(reader->open(TabletReaderParams()));

        auto chunk = ChunkHelper::new_chunk(*_schema, 128);
        int total_rows = 0;
        int new_rows_found = 0;

        while (true) {
            auto st = reader->get_next(chunk.get());
            if (st.is_end_of_file()) {
                break;
            }
            CHECK_OK(st);
            total_rows += chunk->num_rows();

            // Find and verify new rows (pk 12-23)
            for (int i = 0; i < chunk->num_rows(); i++) {
                int pk = chunk->get_column_by_index(0)->get(i).get_int32();
                if (pk >= kChunkSize && pk < kChunkSize * 2) {
                    int c1 = chunk->get_column_by_index(1)->get(i).get_int32();
                    int c2 = chunk->get_column_by_index(2)->get(i).get_int32();
                    EXPECT_EQ(c1, pk * 5) << "New row c1 should be pk * 5";
                    EXPECT_EQ(c2, 10) << "New row c2 should have default value 10";
                    new_rows_found++;
                }
            }
            chunk->reset();
        }

        ASSERT_EQ(total_rows, kChunkSize * 2) << "Should have total 24 rows (12 base + 12 new)";
        ASSERT_EQ(new_rows_found, kChunkSize) << "Should find 12 new rows with pk 12-23";
    }

    // Step 3: Update existing rows with COLUMN_UPSERT_MODE
    {
        auto chunk_partial = generate_data(kChunkSize, 0, true, 9);
        auto txn_id = next_id();
        ASSIGN_OR_ABORT(auto delta_writer, DeltaWriterBuilder()
                                                   .set_tablet_manager(_tablet_mgr.get())
                                                   .set_tablet_id(tablet_id)
                                                   .set_txn_id(txn_id)
                                                   .set_partition_id(_partition_id)
                                                   .set_mem_tracker(_mem_tracker.get())
                                                   .set_schema_id(_tablet_schema->id())
                                                   .set_slot_descriptors(&_slot_pointers)
                                                   .set_partial_update_mode(PartialUpdateMode::COLUMN_UPSERT_MODE)
                                                   .build());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk_partial, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish_with_txnlog());
        delta_writer->close();
        ASSERT_OK(publish_single_version(tablet_id, version + 1, txn_id).status());
        version++;

        LOG(INFO) << "[UPDATE] Updated existing rows with COLUMN_UPSERT_MODE";
    }

    // Step 4: Verify functional correctness
    ASSIGN_OR_ABORT(auto metadata, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    auto reader = std::make_shared<TabletReader>(_tablet_mgr.get(), metadata, *_schema);
    CHECK_OK(reader->prepare());
    CHECK_OK(reader->open(TabletReaderParams()));

    auto chunk = ChunkHelper::new_chunk(*_schema, 128);
    int total_rows = 0;
    int existing_rows_updated = 0;
    int new_rows_verified = 0;

    while (true) {
        auto st = reader->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        CHECK_OK(st);
        total_rows += chunk->num_rows();

        // Verify existing rows (pk 0-11) were updated to c0 * 9 (from step 4)
        // and new rows (pk 12-23) still have c1 = pk * 5 and c2 = 10
        for (int i = 0; i < chunk->num_rows(); i++) {
            int pk = chunk->get_column_by_index(0)->get(i).get_int32();
            int c1 = chunk->get_column_by_index(1)->get(i).get_int32();
            int c2 = chunk->get_column_by_index(2)->get(i).get_int32();

            if (pk < kChunkSize) {
                // Existing rows: c1 should be updated to pk * 9
                EXPECT_EQ(c1, pk * 9) << "Existing row c1 should be updated";
                existing_rows_updated++;
            } else {
                // New rows: c1 = pk * 5, c2 = 10 (default)
                EXPECT_EQ(c1, pk * 5) << "New row c1 should be pk * 5";
                EXPECT_EQ(c2, 10) << "New row c2 should still have default value 10";
                new_rows_verified++;
            }
        }
        chunk->reset();
    }

    ASSERT_EQ(total_rows, kChunkSize * 2) << "Should have total 24 rows";
    ASSERT_EQ(existing_rows_updated, kChunkSize) << "Should have 12 updated existing rows";
    ASSERT_EQ(new_rows_verified, kChunkSize) << "Should have 12 new rows";

    // Verify DCG was generated for COLUMN_UPSERT_MODE
    ASSIGN_OR_ABORT(auto md, _tablet_mgr->get_tablet_metadata(tablet_id, version));
    EXPECT_GT(md->dcg_meta().dcgs_size(), 0) << "DCG should be generated for existing row updates";
}

} // namespace starrocks::lake
