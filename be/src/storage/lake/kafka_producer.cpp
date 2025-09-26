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

#include "storage/lake/kafka_producer.h"

#include <chrono>
#include <thread>
#include <unistd.h>

#include "common/config.h"
#include "common/logging.h"
#include "gutil/strings/substitute.h"

namespace starrocks::lake {

// Note: KafkaProducer is now only used within KafkaProducerPool, no singleton needed

KafkaProducer::~KafkaProducer() {
    shutdown();
}

Status KafkaProducer::init() {
    if (_initialized.load()) {
        return Status::OK();
    }
    
    std::lock_guard<std::mutex> lock(_mutex);
    if (_initialized.load()) {
        return Status::OK();
    }
    
    // Always initialize KafkaProducer, CDC enable/disable is controlled by cdc_enable parameter
    
    RETURN_IF_ERROR(init_config());
    RETURN_IF_ERROR(create_producer());
    // Note: Singleton KafkaProducer doesn't support transactions (no transactional.id set)
    // Only KafkaProducerPool instances support transactions with unique transactional.id
    
    _initialized.store(true);
    LOG(INFO) << "KafkaProducer initialized successfully";
    
    return Status::OK();
}
Status KafkaProducer::init_transactions() {
    if (_transactions_inited) {
        return Status::OK();
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (_transactions_inited) {
        return Status::OK();
    }
    if (!_producer) {
        return Status::InternalError("KafkaProducer not initialized");
    }
    
    LOG(INFO) << "Starting Kafka transaction initialization with transactional.id: " << _transactional_id;
    
    // Initialize transactions - this call might block if Kafka has issues
    LOG(INFO) << "Calling rd_kafka_init_transactions with timeout: " << config::cdc_kafka_txn_timeout_ms << "ms";
    rd_kafka_error_t* kerr = rd_kafka_init_transactions(_producer, config::cdc_kafka_txn_timeout_ms);
    if (kerr) {
        std::string msg = rd_kafka_error_string(kerr);
        rd_kafka_resp_err_t code = rd_kafka_error_code(kerr);
        rd_kafka_error_destroy(kerr);
        return Status::InternalError(strings::Substitute("init_transactions failed: $0 (code: $1)", msg, static_cast<int>(code)));
    }
    _transactions_inited = true;
    LOG(INFO) << "Kafka transactions initialized successfully for transactional.id: " << _transactional_id;
    return Status::OK();
}

Status KafkaProducer::begin_transaction() {
    if (!config::cdc_kafka_enable_transactions) return Status::OK();
    if (!_transactions_inited) return Status::InternalError("transactions not initialized");
    rd_kafka_error_t* kerr = rd_kafka_begin_transaction(_producer);
    if (kerr) {
        std::string msg = rd_kafka_error_string(kerr);
        rd_kafka_error_destroy(kerr);
        return Status::InternalError(strings::Substitute("begin_transaction failed: $0", msg));
    }
    return Status::OK();
}

Status KafkaProducer::commit_transaction() {
    if (!config::cdc_kafka_enable_transactions) return Status::OK();
    if (!_transactions_inited) return Status::InternalError("transactions not initialized");
    rd_kafka_error_t* kerr = rd_kafka_commit_transaction(_producer, config::cdc_kafka_txn_timeout_ms);
    if (kerr) {
        std::string msg = rd_kafka_error_string(kerr);
        rd_kafka_error_destroy(kerr);
        return Status::InternalError(strings::Substitute("commit_transaction failed: $0", msg));
    }
    return Status::OK();
}

Status KafkaProducer::abort_transaction() {
    if (!config::cdc_kafka_enable_transactions) return Status::OK();
    if (!_transactions_inited) return Status::InternalError("transactions not initialized");
    rd_kafka_error_t* kerr = rd_kafka_abort_transaction(_producer, config::cdc_kafka_txn_timeout_ms);
    if (kerr) {
        std::string msg = rd_kafka_error_string(kerr);
        rd_kafka_error_destroy(kerr);
        return Status::InternalError(strings::Substitute("abort_transaction failed: $0", msg));
    }
    return Status::OK();
}

Status KafkaProducer::init_config() {
    char errstr[512];
    
    // Create configuration object
    _conf = rd_kafka_conf_new();
    if (!_conf) {
        return Status::InternalError("Failed to create Kafka configuration");
    }
    
    // Set basic configuration
    if (rd_kafka_conf_set(_conf, "bootstrap.servers", config::cdc_kafka_brokers.c_str(), 
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        return Status::InternalError(strings::Substitute("Failed to set bootstrap.servers: $0", errstr));
    }
    
    // Set compression
    if (rd_kafka_conf_set(_conf, "compression.type", config::cdc_kafka_compression_type.c_str(), 
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        return Status::InternalError(strings::Substitute("Failed to set compression.type: $0", errstr));
    }
    
    // Set acks
    if (rd_kafka_conf_set(_conf, "acks", config::cdc_kafka_acks.c_str(), 
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        return Status::InternalError(strings::Substitute("Failed to set acks: $0", errstr));
    }
    
    // Set retries
    std::string retries_str = std::to_string(config::cdc_kafka_retries);
    if (rd_kafka_conf_set(_conf, "retries", retries_str.c_str(), 
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        return Status::InternalError(strings::Substitute("Failed to set retries: $0", errstr));
    }
    
    // Set batch size
    std::string batch_size_str = std::to_string(config::cdc_kafka_batch_size);
    if (rd_kafka_conf_set(_conf, "batch.size", batch_size_str.c_str(), 
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        return Status::InternalError(strings::Substitute("Failed to set batch.size: $0", errstr));
    }
    
    // Set linger time
    std::string linger_str = std::to_string(config::cdc_kafka_linger_ms);
    if (rd_kafka_conf_set(_conf, "linger.ms", linger_str.c_str(), 
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        return Status::InternalError(strings::Substitute("Failed to set linger.ms: $0", errstr));
    }
    
    // Set message timeout
    std::string timeout_str = std::to_string(config::cdc_kafka_timeout_ms);
    if (rd_kafka_conf_set(_conf, "message.timeout.ms", timeout_str.c_str(), 
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        return Status::InternalError(strings::Substitute("Failed to set message.timeout.ms: $0", errstr));
    }

    // Set transactional id when transactions enabled AND transactional.id is set
    if (config::cdc_kafka_enable_transactions && !_transactional_id.empty()) {
        // Ensure acks=all when enabling transactions (required for idempotence)
        if (rd_kafka_conf_set(_conf, "acks", "all", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            return Status::InternalError(strings::Substitute("Failed to set acks=all for transactions: $0", errstr));
        }
        
        // Set transactional.id
        if (rd_kafka_conf_set(_conf, "transactional.id", _transactional_id.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            return Status::InternalError(strings::Substitute("Failed to set transactional.id: $0", errstr));
        }
        LOG(INFO) << "Set Kafka transactional.id: " << _transactional_id;
        
        // Enable idempotence (required for transactions)
        if (rd_kafka_conf_set(_conf, "enable.idempotence", "true", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            return Status::InternalError(strings::Substitute("Failed to set enable.idempotence: $0", errstr));
        }
        
        LOG(INFO) << "Kafka transactions configuration completed: acks=all, enable.idempotence=true";
    }
    
    // Set security protocol
    if (rd_kafka_conf_set(_conf, "security.protocol", config::cdc_kafka_security_protocol.c_str(), 
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        return Status::InternalError(strings::Substitute("Failed to set security.protocol: $0", errstr));
    }
    
    // Set SASL configuration if needed
    std::string security_protocol = config::cdc_kafka_security_protocol;
    if (security_protocol == "sasl_plaintext" || security_protocol == "sasl_ssl") {
        if (rd_kafka_conf_set(_conf, "sasl.mechanism", config::cdc_kafka_sasl_mechanism.c_str(), 
                              errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            return Status::InternalError(strings::Substitute("Failed to set sasl.mechanism: $0", errstr));
        }
        
        if (!config::cdc_kafka_sasl_username.empty()) {
            if (rd_kafka_conf_set(_conf, "sasl.username", config::cdc_kafka_sasl_username.c_str(), 
                                  errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
                return Status::InternalError(strings::Substitute("Failed to set sasl.username: $0", errstr));
            }
        }
        
        if (!config::cdc_kafka_sasl_password.empty()) {
            if (rd_kafka_conf_set(_conf, "sasl.password", config::cdc_kafka_sasl_password.c_str(), 
                                  errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
                return Status::InternalError(strings::Substitute("Failed to set sasl.password: $0", errstr));
            }
        }
    }
    
    // Set callbacks
    rd_kafka_conf_set_dr_msg_cb(_conf, delivery_report_cb);
    rd_kafka_conf_set_error_cb(_conf, error_cb);
    rd_kafka_conf_set_log_cb(_conf, log_cb);
    
    // Set client ID
    if (rd_kafka_conf_set(_conf, "client.id", "starrocks-cdc-producer", 
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        return Status::InternalError(strings::Substitute("Failed to set client.id: $0", errstr));
    }
    
    return Status::OK();
}

Status KafkaProducer::create_producer() {
    char errstr[512];
    
    _producer = rd_kafka_new(RD_KAFKA_PRODUCER, _conf, errstr, sizeof(errstr));
    if (!_producer) {
        return Status::InternalError(strings::Substitute("Failed to create Kafka producer: $0", errstr));
    }
    
    // Configuration object is now owned by the producer
    _conf = nullptr;
    
    return Status::OK();
}

void KafkaProducer::shutdown() {
    if (_shutdown.exchange(true)) {
        return; // Already shutdown
    }
    
    std::lock_guard<std::mutex> lock(_mutex);
    
    if (_producer) {
        // Wait for outstanding messages to be delivered
        LOG(INFO) << "Flushing Kafka producer...";
        rd_kafka_flush(_producer, 10000); // 10 seconds timeout
        
        // Destroy producer
        rd_kafka_destroy(_producer);
        _producer = nullptr;
        
        LOG(INFO) << "Kafka producer destroyed";
    }
    
    if (_conf) {
        rd_kafka_conf_destroy(_conf);
        _conf = nullptr;
    }
    
    _initialized.store(false);
}

Status KafkaProducer::send_sync(const std::string& topic, const std::string& key, 
                                const std::string& message, int timeout_ms) {
    if (!_initialized.load()) {
        return Status::InternalError("KafkaProducer not initialized");
    }
    
    if (_shutdown.load()) {
        return Status::InternalError("KafkaProducer is shutdown");
    }
    
    // Create synchronization context
    auto sync_ctx = std::make_shared<SyncContext>();
    
    // Send message asynchronously with callback
    rd_kafka_resp_err_t err = rd_kafka_producev(
        _producer,
        RD_KAFKA_V_TOPIC(topic.c_str()),
        RD_KAFKA_V_KEY(key.c_str(), key.size()),
        RD_KAFKA_V_VALUE(const_cast<void*>(static_cast<const void*>(message.c_str())), message.size()),
        RD_KAFKA_V_OPAQUE(sync_ctx.get()),
        RD_KAFKA_V_END
    );
    
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        return Status::InternalError(strings::Substitute("Failed to produce message: $0", 
                                                         rd_kafka_err2str(err)));
    }
    
    // Wait for delivery confirmation. librdkafka requires polling to drive callbacks.
    // We actively poll in small intervals until timeout expires.
    const int actual_timeout = (timeout_ms > 0) ? timeout_ms : config::cdc_kafka_timeout_ms;
    const auto start_time = std::chrono::steady_clock::now();
    while (true) {
        // Drive delivery report callbacks.
        rd_kafka_poll(_producer, 10 /* ms */);

        // Check completion without holding the poll lock to avoid deadlocks
        {
            std::lock_guard<std::mutex> lk(sync_ctx->mutex);
            if (sync_ctx->completed) {
                if (sync_ctx->error == RD_KAFKA_RESP_ERR_NO_ERROR) {
                    return Status::OK();
                }
                return Status::InternalError(strings::Substitute(
                        "Kafka message delivery failed: $0", rd_kafka_err2str(sync_ctx->error)));
            }
        }

        // Check timeout
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        if (elapsed_ms >= actual_timeout) {
            return Status::TimedOut("Kafka message delivery timeout");
        }

        // Sleep briefly to avoid busy loop
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

Status KafkaProducer::send_async(const std::string& topic, const std::string& key, 
                                 const std::string& message) {
    if (!_initialized.load()) {
        return Status::InternalError("KafkaProducer not initialized");
    }
    
    if (_shutdown.load()) {
        return Status::InternalError("KafkaProducer is shutdown");
    }
    
    rd_kafka_resp_err_t err = rd_kafka_producev(
        _producer,
        RD_KAFKA_V_TOPIC(topic.c_str()),
        RD_KAFKA_V_KEY(key.c_str(), key.size()),
        RD_KAFKA_V_VALUE(const_cast<void*>(static_cast<const void*>(message.c_str())), message.size()),
        RD_KAFKA_V_END
    );
    
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        return Status::InternalError(strings::Substitute("Failed to produce message: $0", 
                                                         rd_kafka_err2str(err)));
    }
    
    // Poll to handle delivery reports
    rd_kafka_poll(_producer, 0);
    
    return Status::OK();
}

std::string KafkaProducer::get_topic_name() const {
    return strings::Substitute("$0", config::cdc_kafka_topic);
}
// -------------------- KafkaProducerPool --------------------
std::once_flag KafkaProducerPool::_init_flag;
std::unique_ptr<KafkaProducerPool> KafkaProducerPool::_instance;

KafkaProducerPool* KafkaProducerPool::instance() {
    std::call_once(_init_flag, []() {
        _instance = std::unique_ptr<KafkaProducerPool>(new KafkaProducerPool());
        (void)_instance->init_pool();
    });
    return _instance.get();
}

Status KafkaProducerPool::ensure_size(int size) {
    if (size <= 0) size = 1;
    if ((int)_pool.size() >= size) return Status::OK();
    _pool.reserve(size);
    // Build a unique prefix for transactional.id if not given
    std::string prefix = config::cdc_kafka_producer_id_prefix;
    if (prefix.empty()) {
        char host[256] = {0};
        gethostname(host, sizeof(host));
        prefix = strings::Substitute("$0-$1", host, getpid());
    }
    for (int i = (int)_pool.size(); i < size; ++i) {
        auto prod = std::unique_ptr<KafkaProducer>(new KafkaProducer());
        // Generate unique transactional.id for this producer instance
        std::string my_tid = strings::Substitute("$0-$1", prefix, i);
        prod->set_transactional_id(my_tid);
        RETURN_IF_ERROR(prod->init());
        if (config::cdc_kafka_enable_transactions) {
            RETURN_IF_ERROR(prod->init_transactions());
        }
        _pool.emplace_back(std::move(prod));
    }
    return Status::OK();
}

Status KafkaProducerPool::init_pool() {
    int size = std::max(1, (int)config::cdc_kafka_pool_size);
    return ensure_size(size);
}

KafkaProducer* KafkaProducerPool::pick(int64_t tablet_id) {
    if (_pool.empty()) {
        Status st = init_pool();
        if (!st.ok()) {
            LOG(ERROR) << "Failed to initialize Kafka producer pool: " << st.to_string();
            return nullptr;
        }
    }
    if (_pool.empty()) {
        LOG(ERROR) << "Kafka producer pool is still empty after initialization";
        return nullptr;
    }
    size_t idx = (size_t)(tablet_id >= 0 ? tablet_id : -tablet_id) % _pool.size();
    return _pool[idx].get();
}

void KafkaProducerPool::shutdown() {
    for (auto& p : _pool) {
        if (p) p->shutdown();
    }
}


void KafkaProducer::delivery_report_cb(rd_kafka_t* rk, const rd_kafka_message_t* rkmessage, void* opaque) {
    // For per-message opaque passed via RD_KAFKA_V_OPAQUE, retrieve from rkmessage->_private
    if (rkmessage && rkmessage->_private) {
        auto* sync_ctx = static_cast<SyncContext*>(rkmessage->_private);
        if (sync_ctx) {
            std::lock_guard<std::mutex> lock(sync_ctx->mutex);
            sync_ctx->error = rkmessage->err;
            sync_ctx->completed = true;
            sync_ctx->cv.notify_one();
        }
    }
    
    if (rkmessage->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        LOG(WARNING) << "Kafka message delivery failed: " << rd_kafka_err2str(rkmessage->err)
                     << " (topic: " << rd_kafka_topic_name(rkmessage->rkt) << ")";
    } else {
        VLOG(2) << "Kafka message delivered successfully to topic: " 
                << rd_kafka_topic_name(rkmessage->rkt)
                << " partition: " << rkmessage->partition
                << " offset: " << rkmessage->offset;
    }
}

void KafkaProducer::error_cb(rd_kafka_t* rk, int err, const char* reason, void* opaque) {
    LOG(ERROR) << "Kafka producer error: " << rd_kafka_err2str(static_cast<rd_kafka_resp_err_t>(err))
               << " reason: " << reason;
}

void KafkaProducer::log_cb(const rd_kafka_t* rk, int level, const char* fac, const char* buf) {
    // Map librdkafka log levels to our log levels
    switch (level) {
        case 0: // Emergency
        case 1: // Alert  
        case 2: // Critical
        case 3: // Error
            LOG(ERROR) << "Kafka[" << fac << "]: " << buf;
            break;
        case 4: // Warning
            LOG(WARNING) << "Kafka[" << fac << "]: " << buf;
            break;
        case 5: // Notice
        case 6: // Info
            LOG(INFO) << "Kafka[" << fac << "]: " << buf;
            break;
        case 7: // Debug
        default:
            VLOG(1) << "Kafka[" << fac << "]: " << buf;
            break;
    }
}

} // namespace starrocks::lake
