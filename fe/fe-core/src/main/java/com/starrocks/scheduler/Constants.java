// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.scheduler;

public class Constants {

    public enum TaskType {
        // For Task that can be triggered repeatedly by external.
        MANUAL,
        // For Task that schedules generated by internal periodically.
        PERIODICAL
    }

    public enum TaskState {
        // For TaskType NORMAL TaskState is UNKNOWN.
        UNKNOWN,
        // For TaskType PERIODICAL when TaskState is ACTIVE it means scheduling works.
        ACTIVE,
        PAUSE
    }

    // TaskSource is used to distinguish special Processors for processing tasks from different sources.
    public enum TaskSource {
        CTAS,
        MV
    }

    // PENDING -> RUNNING -> FAILED
    //                    -> SUCCESS
    public enum TaskRunState {
        PENDING,
        RUNNING,
        FAILED,
        SUCCESS,
    }

    // Used to determine the scheduling order of Pending TaskRun to Running TaskRun
    // The bigger the priority, the higher the priority, the default value is LOWEST
    public enum TaskRunPriority {
        LOWEST(0), LOW(20), NORMAL(50), HIGH(80), HIGHEST(100);

        private final int value;

        TaskRunPriority(int value) {
            this.value = value;
        }

        public int value() {
            return value;
        }
    }

}
