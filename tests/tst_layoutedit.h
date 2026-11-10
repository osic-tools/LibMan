#ifndef TST_LAYOUTEDIT_H
#define TST_LAYOUTEDIT_H

#include <QObject>

class LayoutEditTest : public QObject
{
    Q_OBJECT

private slots:
    void parseKLayoutLyp_readsNestedGroups();
    void loadKLayoutTechDbuMicrons_readsLyt();
    void findTechLayer_namePurposeAndNumbers();
    void findTechLayer_unknownListsCloseMatches();
    void addRectToLayoutRoom_createsThenAppends();
    void addRectToLayoutRoom_rejectsEmptySize();
    void layoutRectPythonCode_matchesAppliedEdit();
    void roomPythonEval_echoesCodeInMessages();
    void roomProxyWithoutCode_staysOutOfMessages();
};

#endif // TST_LAYOUTEDIT_H
