#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include "src/layoutedit.h"
#include "src/assistant/assistantmcp.h"
#include "mainwindow_test_hooks.h"
#include "tst_layoutedit.h"

#ifndef LIBMAN_NO_ROOM
#include "database.h"
#endif

namespace {

// KLayout layer-properties excerpt: plain entries plus a group with members.
const char *kLyp = R"(<?xml version="1.0" encoding="utf-8"?>
<layer-properties>
  <properties>
    <frame-color>#39bfff</frame-color>
    <name>Metal1.drawing</name>
    <source>8/0</source>
  </properties>
  <properties>
    <name>Metal1.pin</name>
    <source>8/2@1</source>
  </properties>
  <properties>
    <name>Metal2.drawing</name>
    <source>10/0</source>
  </properties>
  <properties>
    <name>Vias</name>
    <source>*/*@*</source>
    <group-members>
      <name>Via1.drawing</name>
      <source>19/0</source>
    </group-members>
  </properties>
  <properties>
    <name>Cont</name>
    <source>6/0</source>
  </properties>
</layer-properties>
)";

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QVector<TechLayer> fixtureLayers(const QTemporaryDir &dir)
{
    const QString lyp = dir.filePath(QStringLiteral("tech.lyp"));
    writeFile(lyp, kLyp);
    return parseKLayoutLyp(lyp);
}

} // namespace

void LayoutEditTest::parseKLayoutLyp_readsNestedGroups()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QVector<TechLayer> layers = fixtureLayers(dir);

    // Wildcard group source is not a layer; its member is.
    QCOMPARE(layers.size(), 5);
    QCOMPARE(layers.at(0).name, QStringLiteral("Metal1"));
    QCOMPARE(layers.at(0).purpose, QStringLiteral("drawing"));
    QCOMPARE(layers.at(0).layer, 8);
    QCOMPARE(layers.at(0).datatype, 0);
    QCOMPARE(layers.at(1).datatype, 2);

    bool sawVia = false;
    for (const TechLayer &layer : layers) {
        if (layer.name == QLatin1String("Via1")) {
            sawVia = layer.layer == 19 && layer.datatype == 0;
        }
    }
    QVERIFY(sawVia);
}

void LayoutEditTest::loadKLayoutTechDbuMicrons_readsLyt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QCOMPARE(loadKLayoutTechDbuMicrons(dir.path()), 0.0);
    QVERIFY(writeFile(dir.filePath(QStringLiteral("tech.lyt")),
                      "<technology><name>t</name><dbu>0.005</dbu></technology>"));
    QCOMPARE(loadKLayoutTechDbuMicrons(dir.path()), 0.005);
}

void LayoutEditTest::findTechLayer_namePurposeAndNumbers()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QVector<TechLayer> layers = fixtureLayers(dir);
    TechLayer found;

    QVERIFY(findTechLayer(layers, QStringLiteral("Metal1"), QString(), &found));
    QCOMPARE(found.fullName(), QStringLiteral("Metal1.drawing"));

    QVERIFY(findTechLayer(layers, QStringLiteral("metal1 PIN"), QString(), &found));
    QCOMPARE(found.datatype, 2);

    QVERIFY(findTechLayer(layers, QStringLiteral("Metal1"), QStringLiteral("pin"), &found));
    QCOMPARE(found.datatype, 2);

    QVERIFY(findTechLayer(layers, QStringLiteral("Metal2.drawing"), QString(), &found));
    QCOMPARE(found.layer, 10);

    // Purpose-less tech entry matches a bare name.
    QVERIFY(findTechLayer(layers, QStringLiteral("Cont"), QString(), &found));
    QCOMPARE(found.layer, 6);

    QVERIFY(findTechLayer(layers, QStringLiteral("10/0"), QString(), &found));
    QCOMPARE(found.fullName(), QStringLiteral("Metal2.drawing"));

    QVERIFY(findTechLayer(layers, QStringLiteral("99/5"), QString(), &found));
    QCOMPARE(found.layer, 99);
    QCOMPARE(found.datatype, 5);
}

void LayoutEditTest::findTechLayer_unknownListsCloseMatches()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QVector<TechLayer> layers = fixtureLayers(dir);
    TechLayer found;
    QString error;

    QVERIFY(!findTechLayer(layers, QStringLiteral("Metal"), QString(), &found, &error));
    QVERIFY2(error.contains(QStringLiteral("Metal1.drawing 8/0")), qPrintable(error));

    QVERIFY(!findTechLayer({}, QStringLiteral("Metal1"), QString(), &found, &error));
    QVERIFY2(error.contains(QStringLiteral("No technology layers")), qPrintable(error));
}

void LayoutEditTest::addRectToLayoutRoom_createsThenAppends()
{
#ifdef LIBMAN_NO_ROOM
    QSKIP("needs ROOM build");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Test/Test.layout.room"));
    TechLayer metal1{QStringLiteral("Metal1"), QStringLiteral("drawing"), 8, 0};
    TechLayer metal2{QStringLiteral("Metal2"), QStringLiteral("drawing"), 10, 0};

    QString error;
    QVERIFY2(addRectToLayoutRoom(path, QStringLiteral("Test"), metal1, 0, 0, 2.0, 4.0, 1000.0, &error),
             qPrintable(error));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY2(addRectToLayoutRoom(path, QStringLiteral("Test"), metal2, 1.0, 0.5, 0.5, 0.25, 1000.0, &error),
             qPrintable(error));
    QVERIFY2(addRectToLayoutRoom(path, QStringLiteral("Test"), metal1, 3, 0, 1, 1, 1000.0, &error),
             qPrintable(error));

    const room::Database db = room::Database::loadFromFile(path.toStdString());
    const room::Cell *cell = db.lib().findCell("Test");
    QVERIFY(cell != nullptr);
    const room::CellContent *content = cell->findContent(room::ViewType::Layout);
    QVERIFY(content != nullptr);
    const std::vector<room::Shape> &shapes = content->block().shapes();
    QCOMPARE(shapes.size(), std::size_t(3));

    // Same GDS layer reuses one layer table entry.
    QCOMPARE(db.lib().layers().size(), std::size_t(2));

    // Shape order is not preserved by save; look rectangles up by box (dbu = 1 nm).
    const auto findRect = [&shapes](std::int64_t llx, std::int64_t lly, std::int64_t urx,
                                    std::int64_t ury) -> const room::Shape::RectData * {
        for (const room::Shape &shape : shapes) {
            const room::Shape::RectData *r = shape.rect();
            if (r != nullptr && r->box.llx == llx && r->box.lly == lly && r->box.urx == urx
                && r->box.ury == ury) {
                return r;
            }
        }
        return nullptr;
    };
    const room::Shape::RectData *m1 = findRect(0, 0, 2000, 4000);
    const room::Shape::RectData *m2 = findRect(1000, 500, 1500, 750);
    const room::Shape::RectData *m1b = findRect(3000, 0, 4000, 1000);
    QVERIFY(m1 != nullptr);
    QVERIFY(m2 != nullptr);
    QVERIFY(m1b != nullptr);

    const room::LayerSpec &spec = db.lib().layers().at(m1->layerId);
    QCOMPARE(int(spec.layerNum), 8);
    QCOMPARE(QString::fromStdString(spec.name), QStringLiteral("Metal1.drawing"));
    QCOMPARE(int(db.lib().layers().at(m2->layerId).layerNum), 10);
    QCOMPARE(m1b->layerId, m1->layerId);
#endif
}

void LayoutEditTest::layoutRectPythonCode_matchesAppliedEdit()
{
    const TechLayer pin{QStringLiteral("Metal1"), QStringLiteral("pin"), 8, 2};
    LayoutRectEdit edit;
    edit.dbuPerMicron = 1000.0;
    edit.llx = 0;
    edit.lly = 0;
    edit.urx = 2000;
    edit.ury = 4000;

    const QString existing =
        layoutRectPythonCode(QStringLiteral("C:\\lib\\Test\\Test.layout.room"), QStringLiteral("Test"), pin, edit);
    QVERIFY2(existing.contains(QStringLiteral("db = room.open('C:/lib/Test/Test.layout.room')")),
             qPrintable(existing));
    QVERIFY(existing.contains(QStringLiteral("get_or_create_cell('Test').ensure_content('layout', 1000)")));
    QVERIFY(existing.contains(QStringLiteral("== (8, 2)")));
    QVERIFY(existing.contains(QStringLiteral("db.add_layer(8, 2, 'Metal1.pin', room.LayerPurpose.PIN)")));
    QVERIFY(existing.contains(QStringLiteral("cnt.add_rect(lid, 0, 0, 2000, 4000)")));
    QVERIFY(existing.endsWith(QStringLiteral("db.save()")));

    edit.createdFile = true;
    const QString created =
        layoutRectPythonCode(QStringLiteral("C:/lib/New/New.layout.room"), QStringLiteral("New"), pin, edit);
    QVERIFY(created.contains(QStringLiteral("db = room.create()")));
    QVERIFY(created.endsWith(QStringLiteral("db.save('C:/lib/New/New.layout.room', 'layout')")));
}

void LayoutEditTest::roomPythonEval_echoesCodeInMessages()
{
    MainWindow w(QString(), QDir::currentPath());
    AssistantMcp *mcp = MainWindowTestHooks::assistantMcp(&w);
    if (mcp == nullptr || !mcp->hasTool(QStringLiteral("room_python_eval"))) {
        QSKIP("assistant MCP tools not registered in this build");
    }
    // Without a client the tool fails, but the code must already be in Messages.
    RoomMcpClient *client = MainWindowTestHooks::swapRoomMcp(&w, nullptr);

    const QString code = QStringLiteral(
        "db = room.open('C:/lib/Test/Test.layout.room')\n"
        "db.find_cell('Test').content('layout').add_rect(0, 0, 0, 2000, 4000)\n"
        "db.save()");
    const QJsonObject result =
        mcp->callTool(QStringLiteral("room_python_eval"), QJsonObject{{QStringLiteral("code"), code}});
    MainWindowTestHooks::swapRoomMcp(&w, client);

    QVERIFY(!result.value(QStringLiteral("ok")).toBool(true));
    const QString log = MainWindowTestHooks::messagesText(&w);
    QVERIFY2(log.contains(QStringLiteral("# room_python_eval")), qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral(">>> db = room.open('C:/lib/Test/Test.layout.room')")),
             qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("... db.find_cell('Test').content('layout').add_rect(0, 0, 0, 2000, 4000)")),
             qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("... db.save()")), qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("Room MCP client missing")), qPrintable(log));
}

void LayoutEditTest::roomProxyWithoutCode_staysOutOfMessages()
{
    MainWindow w(QString(), QDir::currentPath());
    AssistantMcp *mcp = MainWindowTestHooks::assistantMcp(&w);
    if (mcp == nullptr || !mcp->hasTool(QStringLiteral("room_api_help"))) {
        QSKIP("assistant MCP tools not registered in this build");
    }
    RoomMcpClient *client = MainWindowTestHooks::swapRoomMcp(&w, nullptr);
    const QString before = MainWindowTestHooks::messagesText(&w);
    mcp->callTool(QStringLiteral("room_api_help"));
    MainWindowTestHooks::swapRoomMcp(&w, client);
    QCOMPARE(MainWindowTestHooks::messagesText(&w), before);
}

void LayoutEditTest::addRectToLayoutRoom_rejectsEmptySize()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("x.layout.room"));
    QString error;
    QVERIFY(!addRectToLayoutRoom(path, QStringLiteral("X"), TechLayer{}, 0, 0, 0.0, 1.0, 1000.0, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(path));
}
