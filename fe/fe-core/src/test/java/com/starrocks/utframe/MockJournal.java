// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.utframe;

import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.common.io.DataOutputBuffer;
import com.starrocks.ha.HAProtocol;
import com.starrocks.journal.Journal;
import com.starrocks.journal.JournalCursor;
import com.starrocks.journal.JournalEntity;
import com.starrocks.journal.JournalException;
import com.starrocks.server.GlobalStateMgr;
import org.apache.commons.collections.map.HashedMap;

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

public class MockJournal implements Journal {
    private final AtomicLong nextJournalId = new AtomicLong(1);
    private final Map<Long, JournalEntity> values = Maps.newConcurrentMap();
    private Map<Long, JournalEntity> staggingEntityMap = new HashedMap();

    public MockJournal() {
    }

    @Override
    public void open() throws InterruptedException, JournalException {
    }

    @Override
    public void rollJournal(long journalId) {

    }

    @Override
    public long getMaxJournalId() {
        return nextJournalId.get();
    }

    @Override
    public long getMinJournalId() {
        return 1;
    }

    @Override
    public void close() {
    }

    // only for MockedJournal
    protected JournalEntity read(long journalId) {
        return values.get(journalId);
    }

    @Override
    public JournalCursor read(long fromKey, long toKey) throws JournalException {
        if (toKey < fromKey || fromKey < 0) {
            return null;
        }

        return new MockJournalCursor(this, fromKey, toKey);
    }

    @Override
    public void deleteJournals(long deleteJournalToId) {
        values.remove(deleteJournalToId);
    }

    @Override
    public long getFinalizedJournalId() {
        return 0;
    }

    @Override
    public List<Long> getDatabaseNames() {
        return Lists.newArrayList(0L);
    }

    @Override
    public void batchWriteBegin() throws InterruptedException, JournalException {
        staggingEntityMap.clear();
    }

    @Override
    public void batchWriteAppend(long journalId, DataOutputBuffer buffer)
            throws InterruptedException, JournalException {
        JournalEntity je = new JournalEntity();
        try {
            DataInputStream in = new DataInputStream(new ByteArrayInputStream(buffer.getData()));
            je.setOpCode(in.readShort());
            je.setData(null);
        } catch (IOException e) {
            e.printStackTrace();
        }
        staggingEntityMap.put(journalId, je);
    }

    @Override
    public void batchWriteCommit() throws InterruptedException, JournalException {
        values.putAll(staggingEntityMap);
        staggingEntityMap.clear();
    }

    @Override
    public void batchWriteAbort() throws InterruptedException, JournalException {
        staggingEntityMap.clear();
    }

    private static class MockJournalCursor implements JournalCursor {
        private final MockJournal instance;
        private long start;
        private final long end;

        public MockJournalCursor(MockJournal instance, long start, long end) {
            this.instance = instance;
            this.start = start;
            this.end = end;
        }

        @Override
        public JournalEntity next() {
            if (start > end) {
                return null;
            }
            JournalEntity je = instance.read(start);
            start++;
            return je;
        }

        @Override
        public void close() {
        }
    }

    public static class MockProtocol implements HAProtocol {

        @Override
        public long getEpochNumber() {
            return 0;
        }

        @Override
        public boolean fencing() {
            return true;
        }

        @Override
        public List<InetSocketAddress> getObserverNodes() {
            return Lists.newArrayList();
        }

        @Override
        public List<InetSocketAddress> getElectableNodes(boolean leaderIncluded) {
            return Lists.newArrayList();
        }

        @Override
        public InetSocketAddress getLeader() {
            return null;
        }

        @Override
        public List<InetSocketAddress> getNoneLeaderNodes() {
            return Lists.newArrayList();
        }

        @Override
        public void transferToMaster() {
        }

        @Override
        public void transferToNonMaster() {
        }

        @Override
        public boolean isLeader() {
            return true;
        }

        @Override
        public boolean removeElectableNode(String nodeName) {
            return true;
        }
    }
}
