#include <QtTest/QtTest>

#include <QTemporaryDir>
#include <QFile>

#include "src/snapshotscene.h"
#include "src/klayoutCellResolver.h"
#include "tst_snapshot_layout.h"

#ifndef LIBMAN_NO_ROOM
#include "database.h"

#include <QtEndian>
#include <cmath>

namespace {

room::Shape snapshotRect(std::int64_t llx, std::int64_t lly, std::int64_t urx, std::int64_t ury)
{
    room::Shape::RectData data;
    data.box = room::Box(llx, lly, urx, ury);
    data.layerId = 1;
    return room::Shape(data);
}

// TOP: one frame rect + CHILD placed as a 3x2 array. CHILD: one 100x100 rect.
QString writeArrayFixture(const QTemporaryDir &dir)
{
    room::Database db;
    db.setFileView(room::ViewType::Layout);
    room::Lib &lib = db.lib();

    room::CellContent &child = lib.getOrCreateCell("CHILD").getOrCreateContent(room::ViewType::Layout);
    child.block().shapes().push_back(snapshotRect(0, 0, 100, 100));

    room::CellContent &top = lib.getOrCreateCell("TOP").getOrCreateContent(room::ViewType::Layout);
    top.block().shapes().push_back(snapshotRect(0, 0, 2000, 2000));
    room::Instance array("CHILD", room::Transform(500, 500));
    array.setArray(3, 2, 300, 300);
    top.block().instances().push_back(array);

    lib.recomputeAllBBoxes();
    lib.refreshIndex(room::ViewType::Layout);
    const QString path = dir.filePath(QStringLiteral("cmim_like.layout.room"));
    db.saveToFile(path.toStdString(), room::ViewType::Layout);
    return path;
}

class OasStream {
public:
    void uint(quint64 v)
    {
        do {
            quint8 byte = quint8(v & 0x7F);
            v >>= 7;
            if (v != 0) {
                byte |= 0x80;
            }
            m_bytes.append(char(byte));
        } while (v != 0);
    }
    void sint(qint64 v) { uint((quint64(v < 0 ? -v : v) << 1) | (v < 0 ? 1 : 0)); }
    void str(const QByteArray &s)
    {
        uint(quint64(s.size()));
        m_bytes.append(s);
    }
    void byte(quint8 b) { m_bytes.append(char(b)); }
    void rect(qint64 x, qint64 y, quint64 w, quint64 h)
    {
        byte(20);   // RECTANGLE
        byte(0x7B); // W H X Y D L
        uint(1);    // layer
        uint(0);    // datatype
        uint(w);
        uint(h);
        sint(x);
        sint(y);
    }
    QByteArray &bytes() { return m_bytes; }

private:
    QByteArray m_bytes;
};

// Same hierarchy as writeArrayFixture: TOP frame + CHILD as a 3x2 array (300 pitch).
QString writeOasArrayFixture(const QTemporaryDir &dir)
{
    OasStream oas;
    oas.bytes().append("%SEMI-OASIS\r\n");
    oas.byte(1);          // START
    oas.str("1.0");
    oas.byte(0);          // real: positive integer
    oas.uint(1000);       // grid steps per micron
    oas.uint(0);          // table offsets follow here
    for (int i = 0; i < 12; ++i) {
        oas.uint(0);
    }
    oas.byte(14);         // CELL by name
    oas.str("CHILD");
    oas.rect(0, 0, 100, 100);
    oas.byte(14);
    oas.str("TOP");
    oas.rect(0, 0, 2000, 2000);
    oas.byte(17);         // PLACEMENT
    oas.byte(0xB8);       // C X Y R
    oas.str("CHILD");
    oas.sint(500);
    oas.sint(500);
    oas.byte(1);          // repetition: rectangular grid
    oas.uint(3 - 2);
    oas.uint(2 - 2);
    oas.uint(300);
    oas.uint(300);
    oas.byte(2);          // END, padded to 256 bytes
    oas.str(QByteArray(252, '\0'));
    oas.uint(0);          // no validation

    const QString path = dir.filePath(QStringLiteral("chip.oas"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(oas.bytes()) != oas.bytes().size()) {
        return QString();
    }
    return path;
}

class GdsStream {
public:
    void record(quint8 type, quint8 dataType, const QByteArray &payload = QByteArray())
    {
        appendBe<quint16>(quint16(4 + payload.size()));
        m_bytes.append(char(type));
        m_bytes.append(char(dataType));
        m_bytes.append(payload);
    }
    void int16s(quint8 type, const QVector<qint16> &values)
    {
        QByteArray payload;
        for (qint16 v : values) {
            payload.append(be(v));
        }
        record(type, 0x02, payload);
    }
    void text(quint8 type, QByteArray value)
    {
        if (value.size() % 2 != 0) {
            value.append('\0');
        }
        record(type, 0x06, value);
    }
    void xy(const QVector<qint32> &coords)
    {
        QByteArray payload;
        for (qint32 v : coords) {
            payload.append(be(v));
        }
        record(0x10, 0x03, payload);
    }
    void units(double user, double meters)
    {
        record(0x03, 0x05, real8(user) + real8(meters));
    }
    void rect(qint32 llx, qint32 lly, qint32 urx, qint32 ury)
    {
        record(0x08, 0x00); // BOUNDARY
        int16s(0x0D, {1});  // LAYER
        int16s(0x0E, {0});  // DATATYPE
        xy({llx, lly, urx, lly, urx, ury, llx, ury, llx, lly});
        record(0x11, 0x00); // ENDEL
    }
    const QByteArray &bytes() const { return m_bytes; }

private:
    template<typename T> static QByteArray be(T v)
    {
        QByteArray out(int(sizeof(T)), Qt::Uninitialized);
        qToBigEndian(v, out.data());
        return out;
    }
    template<typename T> void appendBe(T v) { m_bytes.append(be(v)); }
    static QByteArray real8(double v)
    {
        quint64 bits = 0;
        if (v != 0.0) {
            int exponent = 64;
            while (v >= 1.0) { v /= 16.0; ++exponent; }
            while (v < 1.0 / 16.0) { v *= 16.0; --exponent; }
            bits = (quint64(exponent) << 56) | quint64(std::ldexp(v, 56));
        }
        return be(bits);
    }

    QByteArray m_bytes;
};

// TOP: 2000x2000 frame + one CHILD placement. CHILD: one 100x100 rect.
QString writeGdsFixture(const QTemporaryDir &dir)
{
    const QVector<qint16> stamp = {2026, 1, 1, 0, 0, 0, 2026, 1, 1, 0, 0, 0};
    GdsStream gds;
    gds.int16s(0x00, {600});       // HEADER
    gds.int16s(0x01, stamp);       // BGNLIB
    gds.text(0x02, "snaplib");     // LIBNAME
    gds.units(0.001, 1e-9);
    gds.int16s(0x05, stamp);       // BGNSTR
    gds.text(0x06, "CHILD");       // STRNAME
    gds.rect(0, 0, 100, 100);
    gds.record(0x07, 0x00);        // ENDSTR
    gds.int16s(0x05, stamp);
    gds.text(0x06, "TOP");
    gds.rect(0, 0, 2000, 2000);
    gds.record(0x0A, 0x00);        // SREF
    gds.text(0x12, "CHILD");       // SNAME
    gds.xy({500, 500});
    gds.record(0x11, 0x00);
    gds.record(0x07, 0x00);
    gds.record(0x04, 0x00);        // ENDLIB

    const QString path = dir.filePath(QStringLiteral("chip.gds"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(gds.bytes()) != gds.bytes().size()) {
        return QString();
    }
    return path;
}

} // namespace
#endif

void SnapshotLayoutPolicyTest::layoutSnapshotFocusCell_rootUsesResolvedTop()
{
    QCOMPARE(layoutSnapshotFocusCell(false, QString(), QStringLiteral("TOP"),
                                     QStringLiteral("wafer")),
             QStringLiteral("TOP"));
}

void SnapshotLayoutPolicyTest::layoutSnapshotFocusCell_rootFallsBackToGroup()
{
    QCOMPARE(layoutSnapshotFocusCell(false, QString(), QString(), QStringLiteral("wafer")),
             QStringLiteral("wafer"));
}

void SnapshotLayoutPolicyTest::layoutSnapshotFocusCell_treeCellWins()
{
    QCOMPARE(layoutSnapshotFocusCell(true, QStringLiteral("DIE_3"), QStringLiteral("TOP"),
                                     QStringLiteral("wafer")),
             QStringLiteral("DIE_3"));
}

void SnapshotLayoutPolicyTest::layoutSnapshotFocusCell_emptyTreeFallsBack()
{
    QCOMPARE(layoutSnapshotFocusCell(true, QString(), QStringLiteral("TOP"),
                                     QStringLiteral("wafer")),
             QStringLiteral("TOP"));
}

void SnapshotLayoutPolicyTest::layoutSubtreeLoadAttempts_mismatchedGroupThenStemThenEmpty()
{
    const QStringList attempts = layoutSubtreeLoadAttempts(
        QStringLiteral("cmim_2u3_tm1fix"),
        QStringLiteral("C:/lib/cmim_2u3_tm1fix/cmim_2u3_tm1fix.layout.room"));

    QCOMPARE(attempts.size(), 2); // requested == stem, then empty
    QCOMPARE(attempts.at(0), QStringLiteral("cmim_2u3_tm1fix"));
    QVERIFY(attempts.at(1).isEmpty());
}

void SnapshotLayoutPolicyTest::layoutSubtreeLoadAttempts_dedupesIdenticalNames()
{
    const QStringList attempts = layoutSubtreeLoadAttempts(
        QStringLiteral("TOP"),
        QStringLiteral("/tmp/chip.layout.room"));

    QCOMPARE(attempts.size(), 3);
    QCOMPARE(attempts.at(0), QStringLiteral("TOP"));
    QCOMPARE(attempts.at(1), QStringLiteral("chip"));
    QVERIFY(attempts.at(2).isEmpty());
}

void SnapshotLayoutPolicyTest::layoutSnapshotPathStem_stripsLayoutRoomSuffix()
{
    QCOMPARE(layoutSnapshotPathStem(QStringLiteral("a/b/cmim_2u3_tm1fix.layout.room")),
             QStringLiteral("cmim_2u3_tm1fix"));
    QCOMPARE(layoutSnapshotPathStem(QStringLiteral("wafer.layout.room")),
             QStringLiteral("wafer"));
}

void SnapshotLayoutPolicyTest::isRoomLayoutSnapshotPath_acceptsLayoutRoom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString layout = dir.filePath(QStringLiteral("cell.layout.room"));
    {
        QFile f(layout);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("room");
    }
    QVERIFY(isRoomLayoutSnapshotPath(layout));
}

void SnapshotLayoutPolicyTest::isRoomLayoutSnapshotPath_rejectsEmmodelAndMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString em = dir.filePath(QStringLiteral("cell.emmodel.room"));
    {
        QFile f(em);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("em");
    }
    QVERIFY(!isRoomLayoutSnapshotPath(em));
    QVERIFY(!isRoomLayoutSnapshotPath(dir.filePath(QStringLiteral("missing.layout.room"))));
    QVERIFY(!isRoomLayoutSnapshotPath(QString()));
}

void SnapshotLayoutPolicyTest::resolveKLayoutRootCell_emStyleGroupMissingUsesRichestTop()
{
    // EM/LibMan group name often differs from GDS tops inside *.layout.room.
    LayoutHierarchySnapshot hierarchy;
    hierarchy.topCells << QStringLiteral("TOP") << QStringLiteral("PAD");
    hierarchy.allCells.insert(QStringLiteral("TOP"));
    hierarchy.allCells.insert(QStringLiteral("PAD"));
    hierarchy.allCells.insert(QStringLiteral("VIA"));
    hierarchy.childCounts.insert(QStringLiteral("TOP"), 12);
    hierarchy.childCounts.insert(QStringLiteral("PAD"), 1);

    QCOMPARE(resolveKLayoutRootCell(hierarchy,
                                    QStringLiteral("cmim_2u3_tm1fix"),
                                    QStringLiteral("cmim_2u3_tm1fix")),
             QStringLiteral("TOP"));
}

void SnapshotLayoutPolicyTest::loadCoreSnapshot_layoutGroupNameMissingFallsBackToTop()
{
#ifdef LIBMAN_NO_ROOM
    QSKIP("needs ROOM build");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeArrayFixture(dir);

    // EM regression: LibMan group name is not a cell inside *.layout.room.
    const SnapshotScene scene =
        loadCoreSnapshot(path, QStringLiteral("layout"), {}, QStringLiteral("cmim_2u3_tm1fix"));
    QVERIFY2(!scene.empty(), qPrintable(scene.message));
    QVERIFY(!scene.message.contains(QStringLiteral("not found")));
#endif
}

void SnapshotLayoutPolicyTest::loadCoreSnapshot_layoutDrawsInstanceArrays()
{
#ifdef LIBMAN_NO_ROOM
    QSKIP("needs ROOM build");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeArrayFixture(dir);

    const SnapshotScene scene = loadCoreSnapshot(path, QStringLiteral("layout"), {}, QStringLiteral("TOP"));
    QVERIFY2(!scene.empty(), qPrintable(scene.message));
    // Frame + every array element (3x2), not just the first placement.
    QVERIFY2(scene.strokes.size() >= 1 + 6,
             qPrintable(QStringLiteral("strokes=%1").arg(scene.strokes.size())));
    QVERIFY(scene.bounds.contains(QRectF(0, 0, 2000, 2000)));
#endif
}

void SnapshotLayoutPolicyTest::loadCoreSnapshot_layoutTreeCellFramesOnlyThatCell()
{
#ifdef LIBMAN_NO_ROOM
    QSKIP("needs ROOM build");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeArrayFixture(dir);

    // Expanded tree item: snapshot shows CHILD alone, not the TOP frame.
    const SnapshotScene scene = loadCoreSnapshot(path, QStringLiteral("layout"), {}, QStringLiteral("CHILD"));
    QVERIFY2(!scene.empty(), qPrintable(scene.message));
    QCOMPARE(scene.strokes.size(), 1);
    QVERIFY(scene.bounds.width() <= 100.0 + 1e-6);
    QVERIFY(scene.bounds.height() <= 100.0 + 1e-6);
#endif
}

void SnapshotLayoutPolicyTest::isForeignLayoutSnapshotPath_matchesGdsAndOasis()
{
    QVERIFY(isForeignLayoutSnapshotPath(QStringLiteral("C:/lib/chip.gds")));
    QVERIFY(isForeignLayoutSnapshotPath(QStringLiteral("/lib/chip.GDS2")));
    QVERIFY(isForeignLayoutSnapshotPath(QStringLiteral("/lib/chip.oas")));
    QVERIFY(isForeignLayoutSnapshotPath(QStringLiteral("/lib/chip.oasis")));
    QVERIFY(!isForeignLayoutSnapshotPath(QStringLiteral("/lib/chip.lstr")));
    QVERIFY(!isForeignLayoutSnapshotPath(QStringLiteral("/lib/chip.layout.room")));
}

void SnapshotLayoutPolicyTest::loadCoreSnapshot_gdsRootDrawsTopWithChild()
{
#ifdef LIBMAN_NO_ROOM
    QSKIP("needs ROOM build");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeGdsFixture(dir);
    QVERIFY(!path.isEmpty());

    const SnapshotScene scene = loadCoreSnapshot(path, QStringLiteral("layout"), {}, QStringLiteral("TOP"));
    QVERIFY2(!scene.empty(), qPrintable(scene.message));
    QVERIFY2(scene.strokes.size() >= 2,
             qPrintable(QStringLiteral("strokes=%1").arg(scene.strokes.size())));
#endif
}

void SnapshotLayoutPolicyTest::loadCoreSnapshot_gdsTreeCellDrawsOnlyThatCell()
{
#ifdef LIBMAN_NO_ROOM
    QSKIP("needs ROOM build");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeGdsFixture(dir);
    QVERIFY(!path.isEmpty());

    const SnapshotScene scene = loadCoreSnapshot(path, QStringLiteral("layout"), {}, QStringLiteral("CHILD"));
    QVERIFY2(!scene.empty(), qPrintable(scene.message));
    QCOMPARE(scene.strokes.size(), 1);
#endif
}

void SnapshotLayoutPolicyTest::loadCoreSnapshot_oasWithoutCellPicksTop()
{
#ifdef LIBMAN_NO_ROOM
    QSKIP("needs ROOM build");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeOasArrayFixture(dir);
    QVERIFY(!path.isEmpty());

    // No requested cell: the unreferenced TOP, not the CHILD leaf.
    const SnapshotScene scene = loadCoreSnapshot(path, QStringLiteral("layout"), {}, QString());
    QVERIFY2(!scene.empty(), qPrintable(scene.message));
    QVERIFY2(scene.strokes.size() >= 1 + 6,
             qPrintable(QStringLiteral("strokes=%1").arg(scene.strokes.size())));

    const SnapshotScene child = loadCoreSnapshot(path, QStringLiteral("layout"), {}, QStringLiteral("CHILD"));
    QVERIFY2(!child.empty(), qPrintable(child.message));
    QCOMPARE(child.strokes.size(), 1);
#endif
}

void SnapshotLayoutPolicyTest::foreignLayoutSnapshotSkipReason_skipsOnlyHugeFiles()
{
    const QString gds = QStringLiteral("C:/nowhere/huge_never_cached.gds");
    const QString oas = QStringLiteral("C:/nowhere/huge.oas");
    QVERIFY(foreignLayoutSnapshotSkipReason(gds, kMaxGdsSnapshotBytes).isEmpty());
    QVERIFY(foreignLayoutSnapshotSkipReason(gds, kMaxGdsSnapshotBytes + 1).contains(QStringLiteral("GDS")));
    QVERIFY(foreignLayoutSnapshotSkipReason(oas, kMaxOasisSnapshotBytes).isEmpty());
    QVERIFY(foreignLayoutSnapshotSkipReason(oas, kMaxOasisSnapshotBytes + 1).contains(QStringLiteral("OASIS")));
    QVERIFY(foreignLayoutSnapshotSkipReason(QStringLiteral("C:/x/chip.layout.room"), qint64(1) << 40).isEmpty());
}
