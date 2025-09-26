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

#include <memory>
#include <string>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <librdkafka/rdkafka.h>

#include "common/status.h"

namespace starrocks::lake {

class KafkaProducerPool;

// Kafka producer for CDC data publishing (used within KafkaProducerPool)
class KafkaProducer {
public:
    
    // Initialize the producer with configuration
    Status init();

    // Initialize transactional producer if transactions are enabled.
    Status init_transactions();
    
    // Set transactional ID for this producer instance (must be called before init)
    void set_transactional_id(const std::string& tid) { _transactional_id = tid; }
    
    // Shutdown the producer
    void shutdown();
    
    // Send message synchronously to Kafka
    // Returns Status::OK() if message is successfully delivered
    Status send_sync(const std::string& topic, const std::string& key, 
                     const std::string& message, int timeout_ms = -1);
    
    // Send message asynchronously to Kafka
    // Begin a Kafka transaction when transactional mode is enabled
    Status begin_transaction();
    // Commit the current Kafka transaction
    Status commit_transaction();
    // Abort the current Kafka transaction
    Status abort_transaction();
    Status send_async(const std::string& topic, const std::string& key, 
                      const std::string& message);
    
    // Check if producer is initialized and ready
    bool is_ready() const { return _initialized.load(); }
    
    // Get topic name
    std::string get_topic_name() const;
    
    // Destructor
    ~KafkaProducer();

private:
    friend class KafkaProducerPool;
    KafkaProducer() = default;
    
    // Initialize Kafka configuration
    Status init_config();
    
    // Create Kafka producer
    Status create_producer();
    
    // Delivery report callback
    static void delivery_report_cb(rd_kafka_t* rk, const rd_kafka_message_t* rkmessage, void* opaque);
    
    // Error callback
    static void error_cb(rd_kafka_t* rk, int err, const char* reason, void* opaque);
    
    // Log callback
    static void log_cb(const rd_kafka_t* rk, int level, const char* fac, const char* buf);

private:
    
    std::atomic<bool> _initialized{false};
    std::atomic<bool> _shutdown{false};
    
    rd_kafka_t* _producer{nullptr};
    rd_kafka_conf_t* _conf{nullptr};
    bool _transactions_inited{false};
    std::string _transactional_id;  // Store transactional.id for this producer instance
    
    mutable std::mutex _mutex;
    
    // Synchronous send support
    struct SyncContext {
        std::mutex mutex;
        std::condition_variable cv;
        bool completed{false};
        rd_kafka_resp_err_t error{RD_KAFKA_RESP_ERR_NO_ERROR};
    };
};

// A pool of Kafka producers for higher concurrency. Each holds a unique transactional.id.
class KafkaProducerPool {
public:
    static KafkaProducerPool* instance();

    Status init_pool();

    // Pick a producer for given tablet_id. The mapping is stable: same tablet_id -> same pool slot.
    KafkaProducer* pick(int64_t tablet_id);

    // Shutdown all producers
    void shutdown();

private:
    KafkaProducerPool() = default;
    Status ensure_size(int size);

private:
    static std::once_flag _init_flag;
    static std::unique_ptr<KafkaProducerPool> _instance;
    std::vector<std::unique_ptr<KafkaProducer>> _pool;
};

} // namespace starrocks::lake
