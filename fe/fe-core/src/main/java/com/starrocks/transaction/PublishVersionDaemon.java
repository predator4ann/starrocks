// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/transaction/PublishVersionDaemon.java

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.transaction;

import com.google.common.base.Preconditions;
import com.google.common.collect.Lists;
import com.google.common.collect.Sets;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.MaterializedIndex;
import com.starrocks.catalog.Partition;
import com.starrocks.catalog.Table;
import com.starrocks.catalog.Tablet;
import com.starrocks.catalog.lake.LakeTable;
import com.starrocks.catalog.lake.LakeTablet;
import com.starrocks.common.Config;
import com.starrocks.common.UserException;
import com.starrocks.common.util.MasterDaemon;
import com.starrocks.lake.proto.PublishVersionRequest;
import com.starrocks.lake.proto.PublishVersionResponse;
import com.starrocks.rpc.LakeServiceClient;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.system.Backend;
import com.starrocks.system.SystemInfoService;
import com.starrocks.task.AgentBatchTask;
import com.starrocks.task.AgentTaskExecutor;
import com.starrocks.task.AgentTaskQueue;
import com.starrocks.task.PublishVersionTask;
import com.starrocks.thrift.TNetworkAddress;
import com.starrocks.thrift.TTaskType;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Future;

public class PublishVersionDaemon extends MasterDaemon {

    private static final Logger LOG = LogManager.getLogger(PublishVersionDaemon.class);

    private static final long RETRY_INTERVAL_MS = 1000;

    public PublishVersionDaemon() {
        super("PUBLISH_VERSION", Config.publish_version_interval_ms);
    }

    @Override
    protected void runAfterCatalogReady() {
        try {
            GlobalTransactionMgr globalTransactionMgr = GlobalStateMgr.getCurrentGlobalTransactionMgr();
            List<TransactionState> readyTransactionStates = globalTransactionMgr.getReadyToPublishTransactions();
            if (readyTransactionStates == null || readyTransactionStates.isEmpty()) {
                return;
            }
            List<Long> allBackends = GlobalStateMgr.getCurrentSystemInfo().getBackendIds(false);
            if (allBackends.isEmpty()) {
                LOG.warn("some transaction state need to publish, but no backend exists");
                return;
            }

            if (!Config.use_staros) {
                publishVersionForOlapTable(readyTransactionStates);
                return;
            }

            List<TransactionState> olapTransactions = new ArrayList<>();
            List<TransactionState> lakeTransactions = new ArrayList<>();
            for (TransactionState txnState : readyTransactionStates) {
                if (isLakeTableTransaction(txnState)) {
                    lakeTransactions.add(txnState);
                } else {
                    olapTransactions.add(txnState);
                }
            }

            if (!olapTransactions.isEmpty()) {
                publishVersionForOlapTable(olapTransactions);
            }
            if (!lakeTransactions.isEmpty()) {
                publishVersionForLakeTable(lakeTransactions);
            }
        } catch (Throwable t) {
            LOG.error("errors while publish version to all backends", t);
        }
    }

    private void publishVersionForOlapTable(List<TransactionState> readyTransactionStates) throws UserException {
        GlobalTransactionMgr globalTransactionMgr = GlobalStateMgr.getCurrentGlobalTransactionMgr();

        // every backend-transaction identified a single task
        AgentBatchTask batchTask = new AgentBatchTask();
        // traverse all ready transactions and dispatch the publish version task to all backends
        for (TransactionState transactionState : readyTransactionStates) {
            List<PublishVersionTask> tasks = transactionState.createPublishVersionTask();
            for (PublishVersionTask task : tasks) {
                AgentTaskQueue.addTask(task);
                batchTask.addTask(task);
            }
            if (!tasks.isEmpty()) {
                transactionState.setHasSendTask(true);
                LOG.info("send publish tasks for txn_id: {}", transactionState.getTransactionId());
            }
        }
        if (!batchTask.getAllTasks().isEmpty()) {
            AgentTaskExecutor.submit(batchTask);
        }

        // try to finish the transaction, if failed just retry in next loop
        for (TransactionState transactionState : readyTransactionStates) {
            Map<Long, PublishVersionTask> transTasks = transactionState.getPublishVersionTasks();
            Set<Long> publishErrorReplicaIds = Sets.newHashSet();
            Set<Long> unfinishedBackends = Sets.newHashSet();
            boolean allTaskFinished = true;
            for (PublishVersionTask publishVersionTask : transTasks.values()) {
                if (publishVersionTask.isFinished()) {
                    // sometimes backend finish publish version task, but it maybe failed to change transactionid to version for some tablets
                    // and it will upload the failed tabletinfo to fe and fe will deal with them
                    Set<Long> errReplicas = publishVersionTask.collectErrorReplicas();
                    if (!errReplicas.isEmpty()) {
                        publishErrorReplicaIds.addAll(errReplicas);
                    }
                } else {
                    allTaskFinished = false;
                    // Publish version task may succeed and finish in quorum replicas
                    // but not finish in one replica.
                    // here collect the backendId that do not finish publish version
                    unfinishedBackends.add(publishVersionTask.getBackendId());
                }
            }
            boolean shouldFinishTxn = true;
            if (!allTaskFinished) {
                shouldFinishTxn = globalTransactionMgr.canTxnFinished(transactionState,
                        publishErrorReplicaIds, unfinishedBackends);
            }

            if (shouldFinishTxn) {
                globalTransactionMgr.finishTransaction(transactionState.getDbId(), transactionState.getTransactionId(),
                        publishErrorReplicaIds);
                if (transactionState.getTransactionStatus() != TransactionStatus.VISIBLE) {
                    transactionState.updateSendTaskTime();
                    LOG.debug("publish version for transation {} failed, has {} error replicas during publish",
                            transactionState, publishErrorReplicaIds.size());
                } else {
                    for (PublishVersionTask task : transactionState.getPublishVersionTasks().values()) {
                        AgentTaskQueue.removeTask(task.getBackendId(), TTaskType.PUBLISH_VERSION, task.getSignature());
                    }
                    // clear publish version tasks to reduce memory usage when state changed to visible.
                    transactionState.clearPublishVersionTasks();
                }
            }
        } // end for readyTransactionStates
    }

    // TODO: support mix OlapTable with LakeTable
    boolean isLakeTableTransaction(TransactionState transactionState) {
        Database db = GlobalStateMgr.getCurrentState().getDb(transactionState.getDbId());
        if (db == null) {
            return false;
        }
        if (transactionState.getTableIdList().isEmpty()) {
            return false;
        }
        for (long tableId : transactionState.getTableIdList()) {
            Table table = db.getTable(tableId);
            if (table != null) {
                return table.isLakeTable();
            }
        }
        return false;
    }

    // todo: refine performance
    void publishVersionForLakeTable(List<TransactionState> readyTransactionStates) throws UserException {
        GlobalTransactionMgr globalTransactionMgr = GlobalStateMgr.getCurrentGlobalTransactionMgr();

        for (TransactionState txnState : readyTransactionStates) {
            long txnId = txnState.getTransactionId();
            Database db = GlobalStateMgr.getCurrentState().getDb(txnState.getDbId());
            if (db == null) {
                LOG.info("the database of transaction {} has been deleted", txnId);
                globalTransactionMgr.finishTransaction(txnState.getDbId(), txnId, Sets.newHashSet());
                continue;
            }
            boolean finished = true;
            for (TableCommitInfo tableCommitInfo : txnState.getIdToTableCommitInfos().values()) {
                if (!publishTable(db, txnState, tableCommitInfo)) {
                    finished = false;
                }
            }
            if (finished) {
                globalTransactionMgr.finishTransaction(db.getId(), txnId, null);
            }
        }
    }

    boolean publishTable(Database db, TransactionState txnState, TableCommitInfo tableCommitInfo) {
        long txnId = txnState.getTransactionId();
        long tableId = tableCommitInfo.getTableId();
        LakeTable table = (LakeTable) db.getTable(tableId);
        if (table == null) {
            txnState.removeTable(tableCommitInfo.getTableId());
            LOG.info("Removed table {} from transaction {}", tableId, txnId);
            return true;
        }
        boolean finished = true;
        Preconditions.checkState(table.isLakeTable());
        for (PartitionCommitInfo partitionCommitInfo : tableCommitInfo.getIdToPartitionCommitInfo().values()) {
            long partitionId = partitionCommitInfo.getPartitionId();
            Partition partition = table.getPartition(partitionId);
            if (partition == null) {
                tableCommitInfo.removePartition(partitionId);
                LOG.info("Removed partition {} from transaction {}", partitionId, txnId);
                continue;
            }
            long currentTime = System.currentTimeMillis();
            long versionTime = partitionCommitInfo.getVersionTime();
            if (versionTime > 0) {
                continue;
            }
            if (versionTime < 0 && currentTime < Math.abs(versionTime) + RETRY_INTERVAL_MS) {
                continue;
            }
            if (publishPartition(txnState, table, partition, partitionCommitInfo)) {
                partitionCommitInfo.setVersionTime(System.currentTimeMillis());
            } else {
                partitionCommitInfo.setVersionTime(-System.currentTimeMillis());
                finished = false;
            }
        }
        return finished;
    }

    boolean publishPartition(TransactionState txnState, LakeTable table, Partition partition,
                             PartitionCommitInfo partitionCommitInfo) {
        if (partition.getVisibleVersion() + 1 != partitionCommitInfo.getVersion()) {
            LOG.warn("partiton version is " + partition.getVisibleVersion() + " commit version is " +
                    partitionCommitInfo.getVersion());
            return false;
        }
        boolean finished = true;
        long txnId = txnState.getTransactionId();
        Map<Long, List<Long>> beToTablets = new HashMap<>();
        List<MaterializedIndex> indexes = txnState.getPartitionLoadedTblIndexes(table.getId(), partition);
        for (MaterializedIndex index : indexes) {
            for (Tablet tablet : index.getTablets()) {
                Long beId = choosePublishVersionBackend((LakeTablet) tablet);
                if (beId == null) {
                    LOG.warn("No available backend can execute publish version task");
                    return false;
                }
                beToTablets.computeIfAbsent(beId, k -> Lists.newArrayList()).add(tablet.getId());
            }
        }
        List<Future<PublishVersionResponse>> responseList = Lists.newArrayListWithCapacity(beToTablets.size());
        List<Backend> backendList = Lists.newArrayListWithCapacity(beToTablets.size());
        SystemInfoService systemInfoService = GlobalStateMgr.getCurrentSystemInfo();
        for (Map.Entry<Long, List<Long>> entry : beToTablets.entrySet()) {
            Backend backend = systemInfoService.getBackend(entry.getKey());
            if (backend == null) {
                LOG.warn("Backend {} has been dropped", entry.getKey());
                finished = false;
                continue;
            }
            if (!backend.isAlive()) {
                LOG.warn("Backend {} not alive", backend.getHost());
                finished = false;
                continue;
            }
            TNetworkAddress address = new TNetworkAddress();
            address.setHostname(backend.getHost());
            address.setPort(backend.getBrpcPort());

            LakeServiceClient client = new LakeServiceClient(address);
            PublishVersionRequest request = new PublishVersionRequest();
            request.baseVersion = partitionCommitInfo.getVersion() - 1;
            request.newVersion = partitionCommitInfo.getVersion();
            request.tabletIds = entry.getValue();
            request.txnIds = Lists.newArrayList(txnId);

            try {
                Future<PublishVersionResponse> responseFuture = client.publishVersion(request);
                responseList.add(responseFuture);
                backendList.add(backend);
            } catch (Exception e) {
                LOG.warn(e);
                finished = false;
            }
        }

        for (int i = 0; i < responseList.size(); i++) {
            try {
                PublishVersionResponse response = responseList.get(i).get();
                if (response != null && response.failedTablets != null && !response.failedTablets.isEmpty()) {
                    LOG.warn("Fail to publish tablet {} on BE {}", response.failedTablets, backendList.get(i).getHost());
                    finished = false;
                }
            } catch (Exception e) {
                finished = false;
                LOG.warn(e);
            }
        }
        return finished;
    }

    // Returns null if no backend available.
    private Long choosePublishVersionBackend(LakeTablet tablet) {
        try {
            return tablet.getPrimaryBackendId();
        } catch (UserException e) {
            LOG.info("Fail to get primary backend for tablet {}, choose a random alive backend", tablet.getId());
        }
        List<Long> backendIds = GlobalStateMgr.getCurrentSystemInfo().seqChooseBackendIds(1, true, false);
        if (backendIds.isEmpty()) {
            return null;
        }
        return backendIds.get(0);
    }
}
