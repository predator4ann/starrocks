# This file is made available under Elastic License 2.0.
# This file is based on code available under the Apache license here:
#   https://github.com/apache/incubator-doris/blob/master/be/src/env/CMakeLists.txt

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# where to put generated libraries
set(LIBRARY_OUTPUT_PATH "${BUILD_DIR}/src/fs")

# where to put generated binaries
set(EXECUTABLE_OUTPUT_PATH "${BUILD_DIR}/src/fs")

set(EXEC_FILES
    fd_cache.cpp
    fs.cpp
    fs_posix.cpp
    fs_broker.cpp
    fs_memory.cpp
    fs_s3.cpp
    fs_hdfs.cpp
    fs_util.cpp
    )

if ("${USE_STAROS}" STREQUAL "ON")
    list(APPEND EXEC_FILES fs_starlet.cpp)
endif()

add_library(FileSystem STATIC
    ${EXEC_FILES}
)
