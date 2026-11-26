// Chunking of the two tap lines: by silence and by direction change (as serialtap.py).
#include <QtTest>

#include "core/chunker.h"

using namespace lt;

class TestChunker : public QObject {
    Q_OBJECT
private slots:
    void gapEndsChunk() {
        Chunker c(0.020);
        QVERIFY(c.feed(0, "*ID", 1.000).empty());
        QVERIFY(c.feed(0, "N?\r", 1.010).empty());            // within the gap: same chunk
        QVERIFY(c.poll(1.025).empty());                        // 15 ms of silence: not yet
        auto out = c.poll(1.031);
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].data, QByteArray("*IDN?\r"));
        QCOMPARE(out[0].dir, 0);
        QCOMPARE(out[0].t, 1.000);
        QCOMPARE(out[0].end, 1.010);
        auto more = c.feed(0, "X", 1.200);
        QVERIFY(more.empty());
        more = c.feed(0, "Y", 1.300);                          // after a gap: the X chunk is complete
        QCOMPARE(more.size(), size_t(1));
        QCOMPARE(more[0].data, QByteArray("X"));
    }

    void directionChangeEndsChunk() {
        Chunker c(0.020);
        c.feed(0, "MEAS?\r", 2.000);
        auto out = c.feed(1, "M 0.5", 2.005);                  // the answer begins before the gap: the question is done
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].data, QByteArray("MEAS?\r"));
        QVERIFY(c.feed(1, " V\r\n", 2.007).empty());
        auto rest = c.flush();
        QCOMPARE(rest.size(), size_t(1));
        QCOMPARE(rest[0].dir, 1);
        QCOMPARE(rest[0].data, QByteArray("M 0.5 V\r\n"));
        QVERIFY(c.flush().empty());
    }

    void longGap() {
        Chunker c(0.5);
        c.feed(1, "a", 0);
        c.feed(1, "b", 0.4);
        c.feed(1, "c", 0.8);                                   // each 0.4 s after the last: one chunk
        QVERIFY(c.poll(1.2).empty());
        auto out = c.poll(1.31);
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].data, QByteArray("abc"));
    }
};

QTEST_GUILESS_MAIN(TestChunker)
#include "test_chunker.moc"
