// Behavioral test for PyroWave partial decode-unit delivery.
//
// Compiles RtpVideoQueue.c + VideoDepacketizer.c against stubs for the rest of
// the library, feeds synthetic RTP video packets with chosen losses, and checks
// what reaches submitDecodeUnit().
//
// FEC percentage is 0 throughout, which keeps Reed-Solomon out of the picture
// entirely (reconstructFrame() never touches it when there are no parity
// shards), so the stubs below can be trivial.
//
// Build and run (from the repo root; also worth running with -DLC_DEBUG to
// exercise the LC_ASSERTs):
//
//   MLCC=third_party/moonlight-common-c
//   clang -g -O0 -I $MLCC/src -I $MLCC/enet/include \
//         -I $MLCC/nanors -I $MLCC/nanors/deps/obl \
//         -o /tmp/pw_partial_test tools/test_pyrowave_partial.c \
//         $MLCC/src/RtpVideoQueue.c $MLCC/src/VideoDepacketizer.c \
//         $MLCC/src/ByteBuffer.c && /tmp/pw_partial_test

#include "Limelight-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- globals

int AppVersionQuad[4] = { 7, 1, 450, 0 };
STREAM_CONFIGURATION StreamConfig;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
int NegotiatedVideoFormat;

// ------------------------------------------------------------------ stubs

void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS status) { (void)status; }
void connectionSawFrame(uint32_t frameIndex) { (void)frameIndex; }
void connectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame) { (void)startFrame; (void)endFrame; }
void connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR) { (void)frameIndex; (void)frameIsLTR; }
void notifyKeyFrameReceived(void) {}
void LiRequestIdrFrame(void) {}
bool isReferenceFrameInvalidationEnabled(void) { return false; }
bool LiGetCurrentHostDisplayHdrMode(void) { return false; }
int reed_solomon_init(void) { return 0; }

uint64_t PltGetMicroseconds(void) {
    static uint64_t t = 1000000;
    return (t += 1000);
}

void LiInitializeVideoCallbacks(PDECODER_RENDERER_CALLBACKS cb) { (void)cb; }

// The decode unit queue is unused because we run with CAPABILITY_DIRECT_SUBMIT
int LbqInitializeLinkedBlockingQueue(PLINKED_BLOCKING_QUEUE q, int size) { (void)q; (void)size; return 0; }
void LbqSignalQueueShutdown(PLINKED_BLOCKING_QUEUE q) { (void)q; }
PLINKED_BLOCKING_QUEUE_ENTRY LbqDestroyLinkedBlockingQueue(PLINKED_BLOCKING_QUEUE q) { (void)q; return NULL; }
PLINKED_BLOCKING_QUEUE_ENTRY LbqFlushQueueItems(PLINKED_BLOCKING_QUEUE q) { (void)q; return NULL; }
int LbqOfferQueueItem(PLINKED_BLOCKING_QUEUE q, void* d, PLINKED_BLOCKING_QUEUE_ENTRY e) { (void)q; (void)d; (void)e; return 0; }
int LbqWaitForQueueElement(PLINKED_BLOCKING_QUEUE q, void** d) { (void)q; (void)d; return -1; }
int LbqPollQueueElement(PLINKED_BLOCKING_QUEUE q, void** d) { (void)q; (void)d; return -1; }
int LbqPeekQueueElement(PLINKED_BLOCKING_QUEUE q, void** d) { (void)q; (void)d; return -1; }
void LbqSignalQueueUserWake(PLINKED_BLOCKING_QUEUE q) { (void)q; }
int LbqGetItemCount(PLINKED_BLOCKING_QUEUE q) { (void)q; return 0; }

void LiCompleteVideoFrameInternalStub(void) {}

// ------------------------------------------------------------- test state

#define MAX_CAPTURED 16

typedef struct {
    int frameNumber;
    int frameType;
    bool isPartial;
    int fullLength;
    unsigned char data[8192];
} CAPTURED_DU;

static CAPTURED_DU captured[MAX_CAPTURED];
static int capturedCount;

static int testSubmitDecodeUnit(PDECODE_UNIT du) {
    CAPTURED_DU* c = &captured[capturedCount++];
    PLENTRY entry;
    int off = 0;

    c->frameNumber = du->frameNumber;
    c->frameType = du->frameType;
    c->isPartial = du->isPartial;
    c->fullLength = du->fullLength;

    for (entry = du->bufferList; entry != NULL; entry = entry->next) {
        memcpy(&c->data[off], entry->data, entry->length);
        off += entry->length;
    }
    if (off != du->fullLength) {
        printf("  !! fullLength %d but chain held %d bytes\n", du->fullLength, off);
    }
    return DR_OK;
}

// -------------------------------------------------------- packet building

#define PKT_SIZE      64                                  // StreamConfig.packetSize
#define PAYLOAD_BYTES (PKT_SIZE - (int)sizeof(NV_VIDEO_PACKET))
#define FRAME_HDR_LEN 8

static RTP_VIDEO_QUEUE queue;
static uint16_t nextSeq;

// Builds one RTP video packet and hands it to the queue (or drops it).
// payload is the frame's byte stream; this packet carries the slice at
// [packetIndex * PAYLOAD_BYTES ...], where packet 0 begins with the 8-byte
// frame header just like the host emits.
static void sendPacket(uint32_t frameIndex, int packetIndex, int packetsInBlock,
                       uint8_t blockNum, uint8_t lastBlockNum, uint32_t dataShards,
                       const unsigned char* frameBytes, int frameLen,
                       uint32_t streamPacketIndex, bool drop) {
    int receiveSize = PKT_SIZE + MAX_RTP_HEADER_SIZE;
    int bufSize = receiveSize + (int)sizeof(RTPV_QUEUE_ENTRY);
    unsigned char* buf = calloc(1, bufSize);
    PRTP_PACKET rtp = (PRTP_PACKET)buf;
    PRTPV_QUEUE_ENTRY entry = (PRTPV_QUEUE_ENTRY)&buf[receiveSize];
    PNV_VIDEO_PACKET nv;
    unsigned char* payload;
    int dataOffset = (int)sizeof(RTP_PACKET) + 4;
    int off, avail;

    rtp->header = 0x80 | FLAG_EXTENSION;
    rtp->packetType = 0;
    rtp->sequenceNumber = nextSeq++;
    rtp->timestamp = 90000;
    rtp->ssrc = 0;

    nv = (PNV_VIDEO_PACKET)(buf + dataOffset);
    nv->frameIndex = frameIndex;
    nv->streamPacketIndex = streamPacketIndex << 8;
    nv->flags = FLAG_CONTAINS_PIC_DATA;
    if (packetIndex == 0) {
        nv->flags |= FLAG_SOF;
    }
    if (packetIndex == packetsInBlock - 1) {
        nv->flags |= FLAG_EOF;
    }
    nv->multiFecFlags = 0x10;
    nv->multiFecBlocks = (uint8_t)((blockNum << 4) | (lastBlockNum << 6));
    // fecIndex in bits 12..21, data shards in 22..31, percentage 0
    nv->fecInfo = ((uint32_t)packetIndex << 12) | (dataShards << 22);

    // Payload: the frame byte stream, sliced across packets of this block
    payload = (unsigned char*)(nv + 1);
    off = packetIndex * PAYLOAD_BYTES;
    avail = frameLen - off;
    if (avail > PAYLOAD_BYTES) {
        avail = PAYLOAD_BYTES;
    }
    if (avail > 0) {
        memcpy(payload, &frameBytes[off], avail);
    }

    if (drop) {
        free(buf);
        return;
    }

    if (RtpvAddPacket(&queue, rtp, dataOffset + PKT_SIZE, entry) != RTPF_RET_QUEUED) {
        free(buf);
    }
}

// Assembles a frame's byte stream: 8-byte host frame header + a recognizable
// body (byte i = i & 0xFF) so we can verify exactly which bytes were delivered.
static int buildFrame(unsigned char* out, int bodyLen) {
    int i;
    out[0] = 0x01;              // short header
    out[1] = 0; out[2] = 0;     // processing latency
    out[3] = 2;                 // IDR (PyroWave frames are all intra)
    out[4] = 0; out[5] = 0;     // lastPayloadLen, filled by caller
    out[6] = 0; out[7] = 0;
    for (i = 0; i < bodyLen; i++) {
        out[FRAME_HDR_LEN + i] = (unsigned char)(i & 0xFF);
    }
    return FRAME_HDR_LEN + bodyLen;
}

static void setLastPayloadLen(unsigned char* frame, int totalLen) {
    // Host computes this as total % payload_blocksize (or the full size)
    int last = totalLen % PAYLOAD_BYTES;
    if (last == 0) {
        last = PAYLOAD_BYTES;
    }
    frame[4] = (unsigned char)(last & 0xFF);
    frame[5] = (unsigned char)((last >> 8) & 0xFF);
}

// ------------------------------------------------------------------ tests

static int failures;

static void expect(bool cond, const char* what) {
    printf("    %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        failures++;
    }
}

static void resetAll(int videoFormat) {
    NegotiatedVideoFormat = videoFormat;
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.packetSize = PKT_SIZE;
    memset(&VideoCallbacks, 0, sizeof(VideoCallbacks));
    VideoCallbacks.submitDecodeUnit = testSubmitDecodeUnit;
    VideoCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;
    capturedCount = 0;
    nextSeq = 0;
    memset(captured, 0, sizeof(captured));
    initializeVideoDepacketizer(PKT_SIZE);
    RtpvInitializeQueue(&queue);
}

// Sends a single-FEC-block frame, dropping the packet indices in dropList.
static void sendSingleBlockFrame(uint32_t frameIndex, int bodyLen, uint32_t* spi,
                                 const int* dropList, int dropCount) {
    unsigned char frame[8192];
    int total = buildFrame(frame, bodyLen);
    int packets = (total + PAYLOAD_BYTES - 1) / PAYLOAD_BYTES;
    int i, j;

    setLastPayloadLen(frame, total);

    for (i = 0; i < packets; i++) {
        bool drop = false;
        for (j = 0; j < dropCount; j++) {
            if (dropList[j] == i) {
                drop = true;
            }
        }
        sendPacket(frameIndex, i, packets, 0, 0, (uint32_t)packets,
                   frame, total, (*spi)++, drop);
    }
}

static bool bodyBytesMatch(const CAPTURED_DU* c, int expectedBodyLen) {
    int i;
    if (c->fullLength != expectedBodyLen) {
        return false;
    }
    for (i = 0; i < expectedBodyLen; i++) {
        if (c->data[i] != (unsigned char)(i & 0xFF)) {
            printf("      byte %d: got 0x%02x want 0x%02x\n", i, c->data[i], (unsigned char)(i & 0xFF));
            return false;
        }
    }
    return true;
}

static void testCompleteFrame(void) {
    uint32_t spi = 0;
    printf("  complete frame is delivered whole and not marked partial\n");
    resetAll(VIDEO_FORMAT_PYROWAVE_444);

    // 5 packets' worth of body
    sendSingleBlockFrame(1, PAYLOAD_BYTES * 5 - FRAME_HDR_LEN, &spi, NULL, 0);

    expect(capturedCount == 1, "one decode unit delivered");
    if (capturedCount == 1) {
        expect(!captured[0].isPartial, "not marked partial");
        expect(captured[0].frameNumber == 1, "frame number 1");
        expect(captured[0].frameType == FRAME_TYPE_IDR, "typed as IDR");
        expect(bodyBytesMatch(&captured[0], PAYLOAD_BYTES * 5 - FRAME_HDR_LEN),
               "payload bytes exact");
    }
}

static void testHoleInMiddle(void) {
    uint32_t spi = 0;
    int drops[1] = { 5 };
    int bodyLen = PAYLOAD_BYTES * 8 - FRAME_HDR_LEN;
    printf("  frame with a hole delivers the prefix, next frame is unaffected\n");
    resetAll(VIDEO_FORMAT_PYROWAVE_444);

    // Frame 1 loses packet 5 of 8; frame 2 is clean and triggers the salvage
    sendSingleBlockFrame(1, bodyLen, &spi, drops, 1);
    sendSingleBlockFrame(2, bodyLen, &spi, NULL, 0);

    expect(capturedCount == 2, "two decode units delivered");
    if (capturedCount == 2) {
        expect(captured[0].isPartial, "frame 1 marked partial");
        expect(captured[0].frameNumber == 1, "frame 1 numbered correctly");
        // Packets 0-4 survived: 5 packets of payload, minus the 8-byte header
        expect(bodyBytesMatch(&captured[0], PAYLOAD_BYTES * 5 - FRAME_HDR_LEN),
               "prefix is exactly packets 0-4, no padding or gap");
        expect(!captured[1].isPartial, "frame 2 not partial");
        expect(bodyBytesMatch(&captured[1], bodyLen), "frame 2 payload intact");
    }
}

static void testFirstPacketLost(void) {
    uint32_t spi = 0;
    int drops[1] = { 0 };
    int bodyLen = PAYLOAD_BYTES * 6 - FRAME_HDR_LEN;
    printf("  frame whose first packet is lost delivers nothing, no stall after\n");
    resetAll(VIDEO_FORMAT_PYROWAVE_444);

    sendSingleBlockFrame(1, bodyLen, &spi, drops, 1);
    sendSingleBlockFrame(2, bodyLen, &spi, NULL, 0);

    expect(capturedCount == 1, "only the good frame delivered");
    if (capturedCount == 1) {
        expect(captured[0].frameNumber == 2, "delivered frame is frame 2");
        expect(!captured[0].isPartial, "frame 2 not partial");
    }
}

static void testNoStallAfterPartial(void) {
    uint32_t spi = 0;
    int drops[1] = { 4 };
    int bodyLen = PAYLOAD_BYTES * 6 - FRAME_HDR_LEN;
    int i;
    printf("  a partial frame does not put the depacketizer into an IDR wait\n");
    resetAll(VIDEO_FORMAT_PYROWAVE_444);

    sendSingleBlockFrame(1, bodyLen, &spi, drops, 1);
    for (i = 2; i <= 5; i++) {
        sendSingleBlockFrame((uint32_t)i, bodyLen, &spi, NULL, 0);
    }

    expect(capturedCount == 5, "partial frame plus four complete frames");
    if (capturedCount == 5) {
        expect(captured[0].isPartial, "first is partial");
        expect(!captured[1].isPartial && !captured[2].isPartial &&
               !captured[3].isPartial && !captured[4].isPartial,
               "the rest are complete");
    }
}

static void testFormatGating(void) {
    // Feeding a fabricated bitstream through the H.264/HEVC depacketizer would
    // just trip its Annex B parser, so check the gate itself: partial delivery
    // must arm for every PyroWave profile and for nothing else.
    static const struct { int format; const char* name; bool expected; } cases[] = {
        { VIDEO_FORMAT_PYROWAVE,       "PyroWave 4:2:0 8-bit",  true  },
        { VIDEO_FORMAT_PYROWAVE_444,   "PyroWave 4:4:4 8-bit",  true  },
        { VIDEO_FORMAT_PYROWAVE10_420, "PyroWave 4:2:0 10-bit", true  },
        { VIDEO_FORMAT_PYROWAVE10_444, "PyroWave 4:4:4 10-bit", true  },
        { VIDEO_FORMAT_H264,           "H.264",                 false },
        { VIDEO_FORMAT_H265,           "HEVC",                  false },
        { VIDEO_FORMAT_H265_MAIN10,    "HEVC Main10",           false },
        { VIDEO_FORMAT_AV1_MAIN8,      "AV1 Main8",             false },
        { VIDEO_FORMAT_AV1_MAIN10,     "AV1 Main10",            false },
    };
    size_t i;

    printf("  partial delivery arms only for PyroWave formats\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        resetAll(cases[i].format);
        expect(queue.partialDeliveryEnabled == cases[i].expected, cases[i].name);
    }
}

static void testMultiBlockPartial(void) {
    uint32_t spi = 0;
    unsigned char frame[8192];
    int perBlock = 4;
    int bodyLen = PAYLOAD_BYTES * (perBlock * 2) - FRAME_HDR_LEN;
    int total = buildFrame(frame, bodyLen);
    int i;
    printf("  multi-block frame: completed block 0 plus prefix of block 1\n");
    resetAll(VIDEO_FORMAT_PYROWAVE_444);
    setLastPayloadLen(frame, total);

    // Block 0: all 4 packets arrive
    for (i = 0; i < perBlock; i++) {
        sendPacket(1, i, perBlock, 0, 1, (uint32_t)perBlock, frame, total, spi++, false);
    }
    // Block 1: packet 2 of 4 is lost. Its payload continues where block 0 left
    // off, so shift the slice base by one block's worth.
    for (i = 0; i < perBlock; i++) {
        int globalIndex = perBlock + i;
        bool drop = (i == 2);
        sendPacket(1, i, perBlock, 1, 1, (uint32_t)perBlock,
                   &frame[globalIndex * PAYLOAD_BYTES - i * PAYLOAD_BYTES],
                   total - globalIndex * PAYLOAD_BYTES + i * PAYLOAD_BYTES,
                   spi++, drop);
    }
    // A new frame triggers the salvage of frame 1
    sendSingleBlockFrame(2, PAYLOAD_BYTES * 3 - FRAME_HDR_LEN, &spi, NULL, 0);

    expect(capturedCount == 2, "partial frame 1 plus complete frame 2");
    if (capturedCount == 2) {
        expect(captured[0].isPartial, "frame 1 partial");
        // block 0 (4 packets) + block 1 packets 0-1 = 6 packets of payload
        expect(bodyBytesMatch(&captured[0], PAYLOAD_BYTES * 6 - FRAME_HDR_LEN),
               "delivered block 0 entirely plus block 1 prefix");
    }
}

int main(void) {
    printf("PyroWave partial decode-unit delivery\n\n");

    testCompleteFrame();
    testHoleInMiddle();
    testFirstPacketLost();
    testNoStallAfterPartial();
    testFormatGating();
    testMultiBlockPartial();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}

// --------------------------------------------------- late-bound link stubs

CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;

reed_solomon* reed_solomon_new(int d, int p) { (void)d; (void)p; return NULL; }
int reed_solomon_decode(reed_solomon* rs, uint8_t** shards, uint8_t* marks, int nr, int bs) {
    (void)rs; (void)shards; (void)marks; (void)nr; (void)bs; return -1;
}
void reed_solomon_release(reed_solomon* rs) { (void)rs; }
