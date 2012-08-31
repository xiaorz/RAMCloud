/* Copyright (c) 2009-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"
#include "BackupReplica.h"
#include "InMemoryStorage.h"
#include "Object.h"
#include "SegmentIterator.h"

namespace RAMCloud {

class BackupReplicaTest : public ::testing::Test {
  public:
    typedef BackupStorage::Frame Frame;
    BackupReplicaTest()
        : segmentSize(64 * 1024)
        , storage{segmentSize, 2}
        , info{storage, ServerId(99, 0), 88, segmentSize, true}
    {
        Logger::get().setLogLevels(SILENT_LOG_LEVEL);
    }

    /**
     * Helper that simply creates and appends an object to the given segment.
     */
    void
    appendObjectNoReplication(Segment& segment, const char* data,
                              uint32_t bytes, uint64_t tableId,
                              const char* stringKey, uint16_t stringKeyLength)
    {
        Key key(tableId, stringKey, stringKeyLength);
        Object object(key, data, bytes, 0, 0);
        Buffer buffer;
        object.serializeToBuffer(buffer);
        segment.append(LOG_ENTRY_TYPE_OBJ, buffer);
    }

    uint32_t segmentSize;
    InMemoryStorage storage;
    BackupReplica info;
};

TEST_F(BackupReplicaTest, destructor) {
    TestLog::Enable _;
    {
        // Normal replica.
        BackupReplica info{storage, ServerId(99, 0), 88, segmentSize, true};
        info.open(false);
    }
    EXPECT_EQ("~BackupReplica: Backup shutting down with open segment "
              "<99.0,88>, closing out to storage", TestLog::get());
}

TEST_F(BackupReplicaTest, destructorLoading) {
    {
        BackupReplica info{storage, ServerId(99, 0), 88, segmentSize, true};
        info.open(false);
        info.close();
        info.startLoading();
    }
}

void
appendTablet(ProtoBuf::Tablets& tablets,
             uint64_t partitionId,
             uint64_t tableId,
             uint64_t start, uint64_t end,
             uint64_t ctimeHeadSegmentId, uint32_t ctimeHeadSegmentOffset)
{
    ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
    tablet.set_table_id(tableId);
    tablet.set_start_key_hash(start);
    tablet.set_end_key_hash(end);
    tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
    tablet.set_user_data(partitionId);
    tablet.set_ctime_log_head_id(ctimeHeadSegmentId);
    tablet.set_ctime_log_head_offset(ctimeHeadSegmentOffset);
}

void
createTabletList(ProtoBuf::Tablets& tablets)
{
    appendTablet(tablets, 0, 123,
        Key::getHash(123, "10", 2), Key::getHash(123, "10", 2), 0, 0);
    appendTablet(tablets, 1, 123,
        Key::getHash(123, "30", 2), Key::getHash(123, "30", 2), 0, 0);

    // tablet created when log head was > (0, 0)
    appendTablet(tablets, 0, 123,
        Key::getHash(123, "XX", 2), Key::getHash(123, "XX", 2), 12741, 57273);
}

TEST_F(BackupReplicaTest, appendRecoverySegment) {
    info.open(false);
    Segment segment;

    SegmentHeader header = { 99, 88, segmentSize, Segment::INVALID_SEGMENT_ID };
    segment.append(LOG_ENTRY_TYPE_SEGHEADER, &header, sizeof(header));

    appendObjectNoReplication(segment, NULL, 0, 123, "10", 2);

    segment.close();
    Buffer src;
    Segment::Certificate certificate;
    uint32_t appendedBytes = segment.getAppendedLength(certificate);
    segment.appendToBuffer(src, 0, appendedBytes);
    info.append(src, 0, appendedBytes, 0, &certificate);
    info.close();
    info.setRecovering();
    info.startLoading();

    ProtoBuf::Tablets partitions;
    createTabletList(partitions);
    info.buildRecoverySegments(partitions);

    Buffer buffer;
    Status status = info.appendRecoverySegment(0, &buffer, &certificate);
    ASSERT_EQ(STATUS_OK, status);
    EXPECT_EQ(30u, certificate.segmentLength);
    EXPECT_EQ(0x12f3a30bu, certificate.checksum);
    SegmentIterator it(buffer.getRange(0, buffer.getTotalLength()),
                                       buffer.getTotalLength(), certificate);
    EXPECT_FALSE(it.isDone());
    EXPECT_EQ(LOG_ENTRY_TYPE_OBJ, it.getType());
    EXPECT_EQ(28U, it.getLength());

    it.next();
    EXPECT_TRUE(it.isDone());
}

TEST_F(BackupReplicaTest, appendRecoverySegmentSecondarySegment) {
    BackupReplica info{storage, ServerId(99, 0), 88, segmentSize, false};
    info.open(false);
    Segment segment;

    SegmentHeader header = { 99, 88, segmentSize, Segment::INVALID_SEGMENT_ID };
    segment.append(LOG_ENTRY_TYPE_SEGHEADER, &header, sizeof(header));

    appendObjectNoReplication(segment, NULL, 0, 123, "10", 2);

    segment.close();
    Buffer src;
    Segment::Certificate certificate;
    uint32_t appendedBytes = segment.getAppendedLength(certificate);
    segment.appendToBuffer(src, 0, appendedBytes);
    info.append(src, 0, appendedBytes, 0, &certificate);
    info.close();

    ProtoBuf::Tablets partitions;
    createTabletList(partitions);
    info.setRecovering(partitions);

    Buffer buffer;
    while (true) {
        Status status = info.appendRecoverySegment(0, &buffer, &certificate);
        if (status == STATUS_RETRY) {
            buffer.reset();
            continue;
        }
        ASSERT_EQ(status, STATUS_OK);
        break;
    }
    buffer.reset();
    while (true) {
        Status status = info.appendRecoverySegment(0, &buffer, &certificate);
        if (status == STATUS_RETRY) {
            buffer.reset();
            continue;
        }
        ASSERT_EQ(status, STATUS_OK);
        break;
    }
    EXPECT_EQ(30u, certificate.segmentLength);
    EXPECT_EQ(0x12f3a30bu, certificate.checksum);
    SegmentIterator it(buffer.getRange(0, buffer.getTotalLength()),
                        buffer.getTotalLength(),
                        certificate);
    EXPECT_FALSE(it.isDone());
    EXPECT_EQ(LOG_ENTRY_TYPE_OBJ, it.getType());
    EXPECT_EQ(28U, it.getLength());

    it.next();
    EXPECT_TRUE(it.isDone());
}

TEST_F(BackupReplicaTest, appendRecoverySegmentMalformedSegment) {
    info.open(false);
    Buffer src;
    src.appendTo("garbage", 7);
    info.append(src, 0, 7, 0, NULL);
    info.setRecovering();
    info.startLoading();

    ProtoBuf::Tablets partitions;
    createTabletList(partitions);
    info.buildRecoverySegments(partitions);

    Buffer buffer;
    Segment::Certificate certificate;
    EXPECT_THROW(
        IGNORE_RESULT(info.appendRecoverySegment(0, &buffer, &certificate)),
                 SegmentRecoveryFailedException);
}

TEST_F(BackupReplicaTest, appendRecoverySegmentNotYetRecovered) {
    Buffer buffer;
    TestLog::Enable _;
    Segment::Certificate certificate;
    EXPECT_THROW(
        IGNORE_RESULT(info.appendRecoverySegment(0, &buffer, &certificate)),
                 BackupBadSegmentIdException);
    EXPECT_EQ("appendRecoverySegment: Asked for segment <99.0,88> which isn't "
              "recovering", TestLog::get());
}

TEST_F(BackupReplicaTest, appendRecoverySegmentPartitionOutOfBounds) {
    info.open(false);
    Segment segment;
    segment.close();
    Buffer src;
    Segment::Certificate certificate;
    uint32_t appendedBytes = segment.getAppendedLength(certificate);
    segment.appendToBuffer(src, 0, appendedBytes);
    info.append(src, 0, appendedBytes, 0, &certificate);
    info.close();
    info.setRecovering();
    info.startLoading();

    ProtoBuf::Tablets partitions;
    info.buildRecoverySegments(partitions);

    EXPECT_EQ(0u, info.recoverySegmentsLength);
    Buffer buffer;
    TestLog::Enable _;
    EXPECT_THROW(
        IGNORE_RESULT(info.appendRecoverySegment(0, &buffer, &certificate)),
                 BackupBadSegmentIdException);
    EXPECT_EQ("appendRecoverySegment: Asked for recovery segment 0 from "
              "segment <99.0,88> but there are only 0 partitions",
              TestLog::get());
}

#ifdef XXX
class MockSegmentIterator : public SegmentIterator {
  public:
    MockSegmentIterator(LogEntryType type, Log::Position pos)
        : SegmentIterator(),
          type(type),
          header(),
          pos(pos)
    {
    }

    LogEntryType getType() const { return type; }
    Log::Position getLogPosition() const { return pos; }

  private:
    LogEntryType type;
    Log::Position pos;
};

TEST_F(BackupReplicaTest, isEntryAlive) {
    ProtoBuf::Tablets partitions;
    createTabletList(partitions);

    // Tablet's creation time log position was (12741, 57273)
    const ProtoBuf::Tablets::Tablet& tablet(partitions.tablet(2));

    // Is a cleaner segment...
    {
        MockSegmentIterator it(LOG_ENTRY_TYPE_OBJ,
                               12742,
                               Log::Position());
        EXPECT_TRUE(isEntryAlive(it, tablet));
    }
    {
        MockSegmentIterator it(LOG_ENTRY_TYPE_OBJ,
                               12740,
                               Log::Position());
        EXPECT_FALSE(isEntryAlive(it, tablet));
    }
    {
        MockSegmentIterator it(LOG_ENTRY_TYPE_OBJ,
                               12741,
                               Log::Position());
        EXPECT_FALSE(isEntryAlive(it, tablet));
    }

    // Is not a cleaner segment...
    {
        MockSegmentIterator it(LOG_ENTRY_TYPE_OBJ,
                               Segment::INVALID_SEGMENT_ID,
                               Log::Position(12741, 57273));
        EXPECT_TRUE(isEntryAlive(it, tablet));
    }
    {
        MockSegmentIterator it(LOG_ENTRY_TYPE_OBJ,
                               Segment::INVALID_SEGMENT_ID,
                               Log::Position(12741, 57274));
        EXPECT_TRUE(isEntryAlive(it, tablet));
    }
    {
        MockSegmentIterator it(LOG_ENTRY_TYPE_OBJ,
                               Segment::INVALID_SEGMENT_ID,
                               Log::Position(12742, 57273));
        EXPECT_TRUE(isEntryAlive(it, tablet));
    }
    {
        MockSegmentIterator it(LOG_ENTRY_TYPE_OBJ,
                               Segment::INVALID_SEGMENT_ID,
                               Log::Position(12740, 57273));
        EXPECT_FALSE(isEntryAlive(it, tablet));
    }
    {
        MockSegmentIterator it(LOG_ENTRY_TYPE_OBJ,
                               Segment::INVALID_SEGMENT_ID,
                               Log::Position(12741, 57272));
        EXPECT_FALSE(isEntryAlive(it, tablet));
    }
}

TEST_F(BackupReplicaTest, whichPartition) {
    ProtoBuf::Tablets partitions;
    createTabletList(partitions);

    info.open();
    Segment segment;

    // Create some test objects with different keys and append to segment.
    appendObjectNoReplication(segment, NULL, 0, 123, "10", 2);
    appendObjectNoReplication(segment, NULL, 0, 123, "30", 2);
    appendObjectNoReplication(segment, NULL, 0, 123, "40", 2);
    appendObjectNoReplication(segment, NULL, 0, 123, "XX", 2);

    SegmentIterator it(segment);

    it.next();
    auto r = whichPartition(it, partitions);
    EXPECT_TRUE(r);
    EXPECT_EQ(0u, *r);

    it.next();
    r = whichPartition(it, partitions);
    EXPECT_TRUE(r);
    EXPECT_EQ(1u, *r);

    it.next();
    TestLog::Enable _;
    r = whichPartition(it, partitions);
    EXPECT_FALSE(r);
    HashType keyHash = Key::getHash(0, "40", 2);
    EXPECT_EQ(format("whichPartition: Couldn't place object with "
              "<tableId, keyHash> of <123,%lu> into any "
              "of the given tablets for recovery; hopefully it belonged to "
              "a deleted tablet or lives in another log now", keyHash),
              TestLog::get());

    TestLog::reset();
    it.next();
    r = whichPartition(it, partitions);
    EXPECT_FALSE(r);

    keyHash = Key::getHash(0, "XX", 2);
    EXPECT_EQ(format("whichPartition: Skipping object with <tableId, keyHash> "
        "of <123,%lu> because it appears to have existed prior to this "
        "tablet's creation.", keyHash), TestLog::get());
}

TEST_F(BackupReplicaTest, buildRecoverySegment) {
    info.open();
    Segment segment;

    SegmentHeader header = { 99, 88, segmentSize, Segment::INVALID_SEGMENT_ID };
    segment.append(LOG_ENTRY_TYPE_SEGHEADER, &header, sizeof(header));

    appendObjectNoReplication(segment, NULL, 0, 123, "XX", 2);

    segment.close();
    Buffer src;
    Segment::Certificate certificate;
    uint32_t appendedBytes = segment.getAppendedLength(certificate);
    segment.appendToBuffer(src, 0, appendedBytes);
    info.write(src, 0, appendedBytes, 0, &certificate, true);
    info.close();
    info.setRecovering();
    info.startLoading();

    ProtoBuf::Tablets partitions;
    createTabletList(partitions);

    info.buildRecoverySegments(partitions);

    // Make sure subsequent calls have no effect.
    TestLog::Enable _;
    info.buildRecoverySegments(partitions);
    EXPECT_EQ("buildRecoverySegments: Recovery segments already built for "
              "<99,88>", TestLog::get());

    EXPECT_FALSE(info.recoveryException);
    EXPECT_EQ(2u, info.recoverySegmentsLength);
    ASSERT_TRUE(info.recoverySegments);
    EXPECT_EQ(0U, info.recoverySegments[0].getTotalLength());
    EXPECT_EQ(0u, info.recoverySegments[1].getTotalLength());
}

TEST_F(BackupReplicaTest, buildRecoverySegmentMalformedSegment) {
    info.open();
    memcpy(info.segment, "garbage", 7);
    info.setRecovering();
    info.startLoading();

    ProtoBuf::Tablets partitions;
    createTabletList(partitions);

    info.buildRecoverySegments(partitions);
    EXPECT_TRUE(info.recoveryException);
    EXPECT_FALSE(info.recoverySegments);
    EXPECT_EQ(0u, info.recoverySegmentsLength);
}

TEST_F(BackupReplicaTest, buildRecoverySegmentNoTablets) {
    info.open();
    Segment segment;
    segment.close();
    Buffer src;
    Segment::Certificate certificate;
    uint32_t appendedBytes = segment.getAppendedLength(certificate);
    segment.appendToBuffer(src, 0, appendedBytes);
    info.write(src, 0, appendedBytes, 0, &certificate, true);
    info.setRecovering();
    info.startLoading();
    info.buildRecoverySegments(ProtoBuf::Tablets());
    EXPECT_FALSE(info.recoveryException);
    EXPECT_EQ(0u, info.recoverySegmentsLength);
    ASSERT_TRUE(info.recoverySegments);
}

TEST_F(BackupReplicaTest, close) {
    info.open();
    EXPECT_EQ(BackupReplica::OPEN, info.state);
    // The F gets tacked on by close() from the header given during write().
    const char* magic = "kitties!F";
    uint32_t bytesToCopy = downCast<uint32_t>(strlen(magic)) - 1;
    Buffer src;
    Buffer::Chunk::appendToBuffer(&src, magic, bytesToCopy);
    SegmentFooterEntry certificate;
    info.write(src, 0, bytesToCopy, 0, &certificate, false);

    info.close();
    EXPECT_EQ(BackupReplica::CLOSED, info.state);
    {
        // wait for the store op to complete
        BackupReplica::Lock lock(info.mutex);
        info.waitForOngoingOps(lock);
    }

    char seg[segmentSize];
    storage.getSegment(info.storageFrame, seg);
    EXPECT_STREQ(magic, seg);
}

TEST_F(BackupReplicaTest, closeWhileNotOpen) {
    EXPECT_THROW(info.close(), BackupBadSegmentIdException);
}

TEST_F(BackupReplicaTest, free) {
    info.open();
    info.close();
    {
        // wait for the store op to complete
        BackupReplica::Lock lock(info.mutex);
        info.waitForOngoingOps(lock);
    }
    EXPECT_FALSE(info.inMemory());
    info.free();
    EXPECT_EQ(BackupReplica::FREED, info.state);
}

TEST_F(BackupReplicaTest, freeRecoveringSecondary) {
    BackupReplica info{storage, ServerId(99, 0), 88, segmentSize, false};
    info.open();
    info.close();
    info.setRecovering(ProtoBuf::Tablets());
    info.free();
    EXPECT_EQ(BackupReplica::FREED, info.state);
}

TEST_F(BackupReplicaTest, open) {
    info.open();
    ASSERT_NE(static_cast<char*>(NULL), info.segment);
    EXPECT_EQ('\0', info.segment[0]);
    EXPECT_NE(static_cast<Frame*>(NULL), info.storageFrame);
    EXPECT_EQ(BackupReplica::OPEN, info.state);
}

TEST_F(BackupReplicaTest, openStorageAllocationFailure) {
    InMemoryStorage storage{segmentSize, 0};
    BackupReplica info{storage, ServerId(99, 0), 88, segmentSize, true};
    EXPECT_THROW(info.open(), BackupStorageException);
    ASSERT_EQ(static_cast<char*>(NULL), info.segment);
    EXPECT_EQ(static_cast<Frame*>(NULL), info.storageFrame);
    EXPECT_EQ(BackupReplica::UNINIT, info.state);
}

TEST_F(BackupReplicaTest, startLoading) {
    info.open();
    info.close();
    info.startLoading();
    EXPECT_EQ(BackupReplica::CLOSED, info.state);
}

TEST_F(BackupReplicaTest, write) {
    info.open();
    Buffer src;
    const char message[] = "this is a test";
    Buffer::Chunk::appendToBuffer(&src, message, arrayLength(message));
    SegmentFooterEntry certificate(0x1234abcdu);
    info.write(src, 10, 4, 1, &certificate, true);
    EXPECT_EQ(0, memcmp(info.segment, "\0test", 5));
}
#endif

} // namespace RAMCloud
