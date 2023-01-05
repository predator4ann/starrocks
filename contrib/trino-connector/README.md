# StarRocks Connector for Trino
A connector for StarRocks that can be used with [Trino](https://trino.io/).

## Getting started
Java runtime environment
- a 64-bit version of Java 17, with a minimum required version of 17.0.3.

Build from source code
```
mvn clean package 
```

Installing and configuring
- extract the plugin ZIP file `target/trino-starrocks-392.zip` and move the extracted directory into the Trino plugin directory.
- configure the catalog properties file into the Trino etc directory. For example, [starrocks.properties](./examples/starrocks.properties)

## Resources
1. [Trino documentation](https://trino.io/docs/current/)