# StarRocks CDC (Change Data Capture) 功能文档

## 📋 目录

- [0. 功能介绍](#0-功能介绍)
- [1. 表参数配置](#1-表参数配置)
- [2. BE 配置参数](#2-be-配置参数)
- [3. 消息格式](#3-消息格式)
- [4. Benchmark 测试](#4-benchmark-测试)
- [5. 最佳实践](#5-最佳实践)
- [6. 故障排查](#6-故障排查)

---

## 0. 功能介绍

### 什么是 CDC？

CDC (Change Data Capture，变更数据捕获) 是一种用于识别和捕获数据库中数据变更的技术。StarRocks CDC 功能可以实时捕获表的所有数据变更操作（INSERT、UPDATE、DELETE），并将变更事件推送到 Kafka，供下游系统消费。

### 核心特性

- ✅ **实时捕获**：基于事务的增量数据捕获，保证数据一致性
- ✅ **高性能**：Streaming 模式，即时发送，低内存占用
- ✅ **类型安全**：支持 JSON 和 Protobuf 两种序列化格式，保留原生数据类型
- ✅ **Kafka 集成**：原生支持 Kafka，配置简单，性能优异
- ✅ **仅主键表**：专为 Primary Key 表设计，确保数据正确性
- ✅ **动态配置**：关键参数支持运行时修改，无需重启

### 应用场景

1. **实时数据同步**：同步数据到其他数据库或数据仓库
2. **实时数据分析**：将变更数据流式处理用于实时分析
3. **审计日志**：记录所有数据变更历史
4. **缓存失效**：通知下游系统更新缓存
5. **事件驱动架构**：触发业务流程和微服务通信

### 系统架构

```
┌──────────────┐
│  StarRocks   │
│  (主键表)     │
└──────┬───────┘
       │
       │ Transaction Commit
       │
       ▼
┌──────────────┐
│CDC Collector │  ← 捕获变更数据
│  (BE 层)      │
└──────┬───────┘
       │
       │ Serialize (JSON/Protobuf)
       │
       ▼
┌──────────────┐
│    Kafka     │  ← 实时推送
│   (Topic)    │
└──────┬───────┘
       │
       │ Consume
       │
       ▼
┌──────────────┐
│  下游系统     │  ← Flink/Spark/应用
└──────────────┘
```

### 数据流

```
INSERT/UPDATE/DELETE 
  → Transaction Commit
  → CDC Data Collection (BE)
  → Type-aware Serialization
  → Kafka Producer (即时发送)
  → Kafka Topic
  → 下游消费者
```

---

## 1. 表参数配置

### 1.1 表属性

CDC 功能通过表属性 `cdc_enable` 来启用或禁用。

| 属性名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `cdc_enable` | BOOLEAN | `false` | 是否启用 CDC 功能 |

**限制条件**：
- ✅ **仅支持主键表 (PRIMARY KEY)**
- ❌ 不支持聚合表 (AGGREGATE KEY)
- ❌ 不支持明细表 (DUPLICATE KEY)
- ❌ 不支持唯一键表 (UNIQUE KEY)

### 1.2 建表示例

#### 创建启用 CDC 的表

```sql
-- 创建主键表并启用 CDC
CREATE TABLE user_events (
    user_id BIGINT NOT NULL,
    event_time DATETIME NOT NULL,
    event_type VARCHAR(50),
    event_data JSON,
    updated_at DATETIME
)
PRIMARY KEY (user_id, event_time)
DISTRIBUTED BY HASH(user_id) BUCKETS 10
PROPERTIES (
    "cdc_enable" = "true",           -- 启用 CDC
    "replication_num" = "3"
);
```

#### 创建标准主键表（后续启用 CDC）

```sql
-- 先创建表
CREATE TABLE orders (
    order_id BIGINT NOT NULL,
    user_id BIGINT NOT NULL,
    order_status VARCHAR(20),
    order_amount DECIMAL(10, 2),
    create_time DATETIME,
    update_time DATETIME
)
PRIMARY KEY (order_id)
DISTRIBUTED BY HASH(order_id) BUCKETS 16
PROPERTIES (
    "replication_num" = "3"
);
```

### 1.3 修改表示例

#### 启用 CDC

```sql
-- 为已存在的表启用 CDC
ALTER TABLE orders 
SET ("cdc_enable" = "true");
```

#### 禁用 CDC

```sql
-- 禁用 CDC 功能
ALTER TABLE orders 
SET ("cdc_enable" = "false");
```

#### 验证 CDC 状态

```sql
-- 查看表属性
SHOW CREATE TABLE orders;

-- 或者查看表详情
DESC TABLE orders;
```

### 1.4 完整示例：电商订单表

```sql
-- 1. 创建订单表（启用 CDC）
CREATE TABLE ecommerce_orders (
    order_id BIGINT NOT NULL COMMENT '订单ID',
    user_id BIGINT NOT NULL COMMENT '用户ID',
    order_no VARCHAR(64) COMMENT '订单号',
    order_status VARCHAR(20) COMMENT '订单状态：pending/paid/shipped/delivered/cancelled',
    total_amount DECIMAL(12, 2) COMMENT '订单总额',
    currency VARCHAR(3) DEFAULT 'CNY' COMMENT '货币类型',
    payment_method VARCHAR(20) COMMENT '支付方式',
    shipping_address VARCHAR(500) COMMENT '收货地址',
    created_at DATETIME NOT NULL COMMENT '创建时间',
    updated_at DATETIME COMMENT '更新时间',
    deleted_at DATETIME COMMENT '删除时间（软删除）'
)
PRIMARY KEY (order_id)
DISTRIBUTED BY HASH(order_id) BUCKETS 32
PROPERTIES (
    "cdc_enable" = "true",
    "replication_num" = "3",
    "storage_medium" = "SSD",
    "compression" = "LZ4"
)
COMMENT = '电商订单表 - 已启用CDC实时同步';

-- 2. 创建订单商品明细表（启用 CDC）
CREATE TABLE ecommerce_order_items (
    item_id BIGINT NOT NULL COMMENT '明细ID',
    order_id BIGINT NOT NULL COMMENT '订单ID',
    product_id BIGINT NOT NULL COMMENT '商品ID',
    product_name VARCHAR(200) COMMENT '商品名称',
    quantity INT COMMENT '数量',
    unit_price DECIMAL(10, 2) COMMENT '单价',
    subtotal DECIMAL(12, 2) COMMENT '小计',
    created_at DATETIME NOT NULL COMMENT '创建时间'
)
PRIMARY KEY (item_id)
DISTRIBUTED BY HASH(order_id) BUCKETS 32
PROPERTIES (
    "cdc_enable" = "true",
    "replication_num" = "3"
)
COMMENT = '订单商品明细表 - 已启用CDC';
```

---

## 2. BE 配置参数

所有 CDC 相关配置参数位于 `be/src/common/config.h`。

### 2.1 核心配置

| 参数名 | 类型 | 默认值 | 动态 | 说明 |
|--------|------|--------|------|------|
| **`cdc_enable`** | mBool | `false` | ✅ | CDC 全局开关（动态） |
| **`cdc_output_format`** | mString | `"json"` | ✅ | 输出格式：`json` 或 `protobuf` |

### 2.2 Kafka 连接配置

| 参数名 | 类型 | 默认值 | 动态 | 说明 |
|--------|------|--------|------|------|
| **`cdc_kafka_brokers`** | String | `"localhost:9092"` | ❌ | Kafka broker 地址（逗号分隔） |
| **`cdc_kafka_topic`** | String | `"starrocks_cdc"` | ❌ | Kafka topic 名称 |

### 2.3 Kafka 性能配置

| 参数名 | 类型 | 默认值 | 动态 | 说明 |
|--------|------|--------|------|------|
| **`cdc_kafka_max_message_size`** | mInt64 | `1048576` (1MB) | ✅ | 单条消息最大大小（字节） |
| **`cdc_kafka_max_rows_per_message`** | mInt32 | `1024` | ✅ | 每条消息最大行数 |
| **`cdc_kafka_timeout_ms`** | mInt32 | `30000` (30s) | ✅ | 发送超时时间（毫秒） |
| **`cdc_kafka_batch_size`** | Int32 | `16384` (16KB) | ❌ | 批量发送大小 |
| **`cdc_kafka_linger_ms`** | Int32 | `5` | ❌ | 批量等待时间（毫秒） |

### 2.4 Kafka 可靠性配置

| 参数名 | 类型 | 默认值 | 动态 | 说明 |
|--------|------|--------|------|------|
| **`cdc_kafka_compression_type`** | String | `"zstd"` | ❌ | 压缩类型：`none`/`gzip`/`snappy`/`lz4`/`zstd` |
| **`cdc_kafka_acks`** | String | `"all"` | ❌ | ACK 策略：`0`/`1`/`all` |
| **`cdc_kafka_retries`** | Int32 | `3` | ❌ | 重试次数 |

### 2.5 Kafka 安全配置

| 参数名 | 类型 | 默认值 | 动态 | 说明 |
|--------|------|--------|------|------|
| **`cdc_kafka_security_protocol`** | String | `"plaintext"` | ❌ | 安全协议：`plaintext`/`ssl`/`sasl_plaintext`/`sasl_ssl` |
| **`cdc_kafka_sasl_mechanism`** | String | `"PLAIN"` | ❌ | SASL 机制：`PLAIN`/`SCRAM-SHA-256`/`SCRAM-SHA-512` |
| **`cdc_kafka_sasl_username`** | String | `""` | ❌ | SASL 用户名 |
| **`cdc_kafka_sasl_password`** | String | `""` | ❌ | SASL 密码 |

### 2.6 配置说明

#### 动态配置修改方式

**通过 SQL**（如果 FE 支持）：
```sql
-- 修改输出格式
UPDATE information_schema.be_configs SET value = 'protobuf' WHERE name = 'cdc_output_format';

-- 修改超时时间
UPDATE information_schema.be_configs SET value = '60000' WHERE name = 'cdc_kafka_timeout_ms';

-- 启用 CDC
UPDATE information_schema.be_configs SET value = 'true' WHERE name = 'cdc_enable';
```

**通过 HTTP API**：
```bash
# 修改输出格式为 Protobuf
curl -X POST http://be_host:be_http_port/api/update_config?cdc_output_format=protobuf

# 修改超时时间为 60 秒
curl -X POST http://be_host:be_http_port/api/update_config?cdc_kafka_timeout_ms=60000

# 调整每条消息最大行数
curl -X POST http://be_host:be_http_port/api/update_config?cdc_kafka_max_rows_per_message=2000
```

#### 配置文件修改

**静态配置**（需要重启 BE）：

编辑 `be/conf/be.conf`：
```ini
# CDC 全局开关
cdc_enable = true

# 输出格式
cdc_output_format = json

# Kafka 配置
cdc_kafka_brokers = kafka1:9092,kafka2:9092,kafka3:9092
cdc_kafka_topic = starrocks_cdc_prod
cdc_kafka_compression_type = zstd
cdc_kafka_acks = all

# 性能调优
cdc_kafka_max_message_size = 2097152
cdc_kafka_max_rows_per_message = 2000
cdc_kafka_timeout_ms = 30000
cdc_kafka_batch_size = 32768
cdc_kafka_linger_ms = 10
```

#### 生产环境推荐配置

```ini
# === 生产环境 CDC 配置示例 ===

# 全局开关
cdc_enable = true
cdc_output_format = protobuf  # Protobuf 更高效

# Kafka 集群
cdc_kafka_brokers = kafka1:9092,kafka2:9092,kafka3:9092
cdc_kafka_topic = prod_starrocks_cdc

# 性能优化
cdc_kafka_max_message_size = 2097152      # 2MB
cdc_kafka_max_rows_per_message = 5000     # 5000 行
cdc_kafka_timeout_ms = 60000              # 60s
cdc_kafka_batch_size = 65536              # 64KB
cdc_kafka_linger_ms = 10                  # 10ms

# 可靠性
cdc_kafka_compression_type = zstd         # 最佳压缩比
cdc_kafka_acks = all                      # 最高可靠性
cdc_kafka_retries = 5                     # 重试 5 次

# 安全（如果需要）
cdc_kafka_security_protocol = sasl_ssl
cdc_kafka_sasl_mechanism = SCRAM-SHA-256
cdc_kafka_sasl_username = cdc_user
cdc_kafka_sasl_password = ********
```

---

## 3. 消息格式

StarRocks CDC 支持两种序列化格式：**JSON** 和 **Protobuf**。

### 3.1 消息结构

#### 通用字段

| 字段名 | 类型 | 说明 |
|--------|------|------|
| `tid` | int64 | Tablet ID |
| `txn` | int64 | Transaction ID |
| `ver` | int64 | Version |
| `ts` | int64 | Timestamp（Unix 时间戳，秒） |
| `ops` | array | 操作列表 |

#### 操作（Operation）字段

| 字段名 | 类型 | 说明 |
|--------|------|------|
| `op` | string | 操作类型：`"u"`（UPDATE/INSERT）或 `"d"`（DELETE） |
| `data` | array | 行数据列表 |

#### 行数据（Row）

每行数据是一个对象，包含表的所有列，key 为列名，value 为列值（类型化）。

### 3.2 JSON 格式

#### 特点
- ✅ 人类可读
- ✅ 保留原生类型（int/float/bool/string）
- ✅ 易于调试
- ⚠️ 体积较大

#### JSON 消息示例

##### INSERT 操作

```json
{
  "tid": 10001,
  "txn": 123456,
  "ver": 2,
  "ts": 1699876543,
  "ops": [
    {
      "op": "u",
      "data": [
        {
          "order_id": 1001,
          "user_id": 5001,
          "order_status": "paid",
          "total_amount": 299.99,
          "currency": "CNY",
          "created_at": "2024-01-15 10:30:00",
          "updated_at": "2024-01-15 10:35:00"
        },
        {
          "order_id": 1002,
          "user_id": 5002,
          "order_status": "pending",
          "total_amount": 599.50,
          "currency": "CNY",
          "created_at": "2024-01-15 10:31:00",
          "updated_at": null
        }
      ]
    }
  ]
}
```

##### UPDATE 操作

```json
{
  "tid": 10001,
  "txn": 123457,
  "ver": 3,
  "ts": 1699876600,
  "ops": [
    {
      "op": "u",
      "data": [
        {
          "order_id": 1001,
          "user_id": 5001,
          "order_status": "shipped",
          "total_amount": 299.99,
          "currency": "CNY",
          "created_at": "2024-01-15 10:30:00",
          "updated_at": "2024-01-15 11:00:00"
        }
      ]
    }
  ]
}
```

##### DELETE 操作

```json
{
  "tid": 10001,
  "txn": 123458,
  "ver": 4,
  "ts": 1699876700,
  "ops": [
    {
      "op": "d",
      "data": [
        {
          "order_id": 1002
        }
      ]
    }
  ]
}
```

##### 混合操作（批量）

```json
{
  "tid": 10001,
  "txn": 123459,
  "ver": 5,
  "ts": 1699876800,
  "ops": [
    {
      "op": "u",
      "data": [
        {
          "order_id": 1003,
          "user_id": 5003,
          "order_status": "paid",
          "total_amount": 199.00,
          "created_at": "2024-01-15 11:15:00"
        },
        {
          "order_id": 1004,
          "user_id": 5004,
          "order_status": "paid",
          "total_amount": 399.00,
          "created_at": "2024-01-15 11:16:00"
        }
      ]
    },
    {
      "op": "d",
      "data": [
        {
          "order_id": 1001
        }
      ]
    }
  ]
}
```

#### 数据类型映射（JSON）

| StarRocks 类型 | JSON 类型 | 示例 |
|---------------|-----------|------|
| `BOOLEAN` | `boolean` | `true`, `false` |
| `TINYINT/SMALLINT/INT` | `number` (int) | `123` |
| `BIGINT` | `number` (int64) | `9223372036854775807` |
| `FLOAT` | `number` (float) | `3.14` |
| `DOUBLE` | `number` (double) | `3.141592653589793` |
| `VARCHAR/CHAR` | `string` | `"Hello"` |
| `DATE` | `string` | `"2024-01-15"` |
| `DATETIME` | `string` | `"2024-01-15 10:30:00"` |
| `NULL` | `null` | `null` |

### 3.3 Protobuf 格式

#### 特点
- ✅ 二进制格式，体积最小
- ✅ 序列化/反序列化速度最快
- ✅ 强类型，类型安全
- ⚠️ 不可直接阅读（需工具）

#### Protobuf Schema

```protobuf
syntax = "proto2";

package starrocks;

// 列值（支持多种类型）
message CdcColumnValuePB {
    optional bool is_null = 1;
    oneof value {
        bool bool_value = 2;
        int32 int32_value = 3;
        int64 int64_value = 4;
        float float_value = 5;
        double double_value = 6;
        string string_value = 7;
        bytes bytes_value = 8;
    }
}

// 行数据
message CdcRowDataPB {
    map<string, CdcColumnValuePB> columns = 1;
}

// 操作
message CdcOperationDataPB {
    optional string operation_type = 1;  // "u" 或 "d"
    repeated CdcRowDataPB rows = 2;
}

// 事务数据（完整消息）
message CdcTransactionDataPB {
    optional int64 tablet_id = 1;
    optional int64 txn_id = 2;
    optional int64 version = 3;
    optional int64 timestamp = 4;
    
    repeated CdcOperationDataPB operations = 9;
}
```

#### Protobuf 反序列化示例（Python）

```python
import sys
from google.protobuf import json_format
from kafka import KafkaConsumer
from cdc_pb2 import CdcTransactionDataPB

def consume_cdc_protobuf():
    consumer = KafkaConsumer(
        'starrocks_cdc',
        bootstrap_servers=['localhost:9092'],
        value_deserializer=lambda m: CdcTransactionDataPB.FromString(m)
    )
    
    for message in consumer:
        cdc_data = message.value
        
        print(f"Tablet ID: {cdc_data.tablet_id}")
        print(f"Txn ID: {cdc_data.txn_id}")
        print(f"Version: {cdc_data.version}")
        print(f"Timestamp: {cdc_data.timestamp}")
        
        for operation in cdc_data.operations:
            print(f"\nOperation: {operation.operation_type}")
            for row in operation.rows:
                print("  Row:")
                for col_name, col_value in row.columns.items():
                    if col_value.is_null:
                        print(f"    {col_name}: NULL")
                    elif col_value.HasField('int64_value'):
                        print(f"    {col_name}: {col_value.int64_value} (int64)")
                    elif col_value.HasField('double_value'):
                        print(f"    {col_name}: {col_value.double_value} (double)")
                    elif col_value.HasField('string_value'):
                        print(f"    {col_name}: {col_value.string_value} (string)")
                    elif col_value.HasField('bool_value'):
                        print(f"    {col_name}: {col_value.bool_value} (bool)")

if __name__ == "__main__":
    consume_cdc_protobuf()
```

#### Protobuf 反序列化示例（Java）

```java
import org.apache.kafka.clients.consumer.*;
import com.starrocks.cdc.CdcProtos.*;

public class CdcProtobufConsumer {
    public static void main(String[] args) {
        Properties props = new Properties();
        props.put("bootstrap.servers", "localhost:9092");
        props.put("group.id", "cdc-consumer");
        props.put("key.deserializer", "org.apache.kafka.common.serialization.StringDeserializer");
        props.put("value.deserializer", "org.apache.kafka.common.serialization.ByteArrayDeserializer");
        
        KafkaConsumer<String, byte[]> consumer = new KafkaConsumer<>(props);
        consumer.subscribe(Arrays.asList("starrocks_cdc"));
        
        while (true) {
            ConsumerRecords<String, byte[]> records = consumer.poll(Duration.ofMillis(100));
            for (ConsumerRecord<String, byte[]> record : records) {
                try {
                    CdcTransactionDataPB cdcData = CdcTransactionDataPB.parseFrom(record.value());
                    
                    System.out.println("Tablet ID: " + cdcData.getTabletId());
                    System.out.println("Txn ID: " + cdcData.getTxnId());
                    
                    for (CdcOperationDataPB operation : cdcData.getOperationsList()) {
                        System.out.println("Operation: " + operation.getOperationType());
                        for (CdcRowDataPB row : operation.getRowsList()) {
                            System.out.println("  Row:");
                            for (Map.Entry<String, CdcColumnValuePB> entry : row.getColumnsMap().entrySet()) {
                                String colName = entry.getKey();
                                CdcColumnValuePB colValue = entry.getValue();
                                
                                if (colValue.getIsNull()) {
                                    System.out.println("    " + colName + ": NULL");
                                } else if (colValue.hasInt64Value()) {
                                    System.out.println("    " + colName + ": " + colValue.getInt64Value());
                                } else if (colValue.hasStringValue()) {
                                    System.out.println("    " + colName + ": " + colValue.getStringValue());
                                }
                            }
                        }
                    }
                } catch (Exception e) {
                    e.printStackTrace();
                }
            }
        }
    }
}
```

### 3.4 格式对比

| 特性 | JSON | Protobuf |
|------|------|----------|
| **可读性** | ✅ 人类可读 | ❌ 二进制（需工具） |
| **数据大小** | ~200 KB (基准) | ~75 KB (-62%) |
| **序列化速度** | 慢 | 快 (2-5x) |
| **反序列化速度** | 慢 | 快 (2-5x) |
| **类型安全** | ⚠️ 弱类型 | ✅ 强类型 |
| **Schema Evolution** | ❌ 无 | ✅ 支持 |
| **调试便利性** | ✅ 易 | ⚠️ 需工具 |
| **推荐场景** | 开发/调试 | 生产环境 |

---

## 4. 故障排查

### 4.1 常见错误

#### 错误 1: CDC 不生效

**症状**：
```
ALTER TABLE xxx SET ("cdc_enable" = "true");
-- 成功，但无消息发送
```

**排查步骤**：
1. 检查全局开关：
   ```sql
   SELECT * FROM information_schema.be_configs WHERE name = 'cdc_enable';
   ```
   或
   ```bash
   curl http://be:port/api/show_config | grep cdc_enable
   ```

2. 检查表类型：
   ```sql
   SHOW CREATE TABLE xxx;
   -- 确认是 PRIMARY KEY
   ```

3. 检查 BE 日志：
   ```bash
   tail -f be/log/be.INFO | grep CDC
   ```

#### 错误 2: Kafka 连接失败

**症状**：
```
BE 日志：Failed to connect to Kafka broker
```

**排查步骤**：
1. 检查网络连通性：
   ```bash
   telnet kafka_host 9092
   ```

2. 检查配置：
   ```bash
   curl http://be:port/api/show_config | grep cdc_kafka
   ```

3. 检查 Kafka 状态：
   ```bash
   kafka-topics.sh --list --bootstrap-server kafka:9092
   ```

#### 错误 3: 消息发送超时

**症状**：
```
BE 日志：Kafka send timeout
```

**解决方案**：
```bash
# 增加超时时间
curl -X POST http://be:port/api/update_config?cdc_kafka_timeout_ms=60000
```

### 4.2 性能调优

#### 场景 1: 延迟过高

**诊断**：
- 检查 Kafka consumer lag
- 检查 BE CPU/内存使用
- 检查网络带宽

**优化**：
```ini
# 增加批量大小
cdc_kafka_batch_size = 65536

# 减少每条消息行数
cdc_kafka_max_rows_per_message = 2000

# 使用 Protobuf
cdc_output_format = protobuf
```

#### 场景 2: 内存占用过高

**优化**：
```ini
# 使用 Protobuf 减少内存
cdc_output_format = protobuf

# 减小批量大小
cdc_kafka_max_rows_per_message = 1000

# 减小消息大小
cdc_kafka_max_message_size = 1048576
```

### 4.3 日志查看

#### BE 日志位置

```bash
# CDC 相关日志
be/log/be.INFO

# 查看 CDC 日志
tail -f be/log/be.INFO | grep CDC

# 查看错误
tail -f be/log/be.WARNING | grep CDC
tail -f be/log/be.ERROR | grep CDC
```

#### 日志示例

```
I0115 10:30:00.123 CDC: Starting to collect delete data for tablet=10001, txn_id=123456
I0115 10:30:00.156 CDC: Successfully collected updated column data, segment_id=5, rows=1000
I0115 10:30:00.178 CDC delete sent: tablet=10001, txn=123456, rows=50
```

