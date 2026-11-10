#ifndef LAYOUTEDIT_H
#define LAYOUTEDIT_H

#include <QString>
#include <QVector>

/*!
 * \brief One technology layer: "Metal1" + "drawing" → GDS 8/0.
 */
struct TechLayer
{
    QString name;     /*!< Layer name without purpose, e.g. "Metal1". */
    QString purpose;  /*!< Purpose, e.g. "drawing", "pin"; empty when the tech has none. */
    int     layer = 0;
    int     datatype = 0;

    QString fullName() const { return purpose.isEmpty() ? name : name + QLatin1Char('.') + purpose; }
};

/*! Layers from every KLayout *.lyp in \a techDir ("Metal1.drawing" + "8/0"). */
QVector<TechLayer> loadKLayoutTechLayers(const QString &techDir);

/*! Layers from one KLayout layer-properties file. */
QVector<TechLayer> parseKLayoutLyp(const QString &lypPath);

/*! Database unit in microns from the first *.lyt in \a techDir (0 when absent). */
double loadKLayoutTechDbuMicrons(const QString &techDir);

/*!
 * \brief Finds a layer by "Metal1", "Metal1.drawing", "Metal1 drawing" or "8/0".
 *
 * \a purpose applies when \a query has none; empty means "drawing". Case-insensitive.
 * Returns false and fills \a error (with close matches) when nothing fits.
 */
bool findTechLayer(const QVector<TechLayer> &layers,
                   const QString &query,
                   const QString &purpose,
                   TechLayer *out,
                   QString *error = nullptr);

/*! What addRectToLayoutRoom() actually wrote (database units of the target content). */
struct LayoutRectEdit
{
    double dbuPerMicron = 1000.0;
    qint64 llx = 0;
    qint64 lly = 0;
    qint64 urx = 0;
    qint64 ury = 0;
    bool   createdFile = false;
};

/*!
 * \brief Appends one rectangle to \a cellName in a ROOM layout file.
 *
 * Creates the file (and the cell) when missing. Coordinates are in microns;
 * \a dbuPerMicron is used only for a new layout content.
 */
bool addRectToLayoutRoom(const QString &roomPath,
                         const QString &cellName,
                         const TechLayer &layer,
                         double xUm, double yUm, double widthUm, double heightUm,
                         double dbuPerMicron,
                         QString *error = nullptr,
                         LayoutRectEdit *applied = nullptr);

/*! ROOM Python code equivalent to an addRectToLayoutRoom() call (for the Messages log). */
QString layoutRectPythonCode(const QString &roomPath,
                             const QString &cellName,
                             const TechLayer &layer,
                             const LayoutRectEdit &edit);

#endif // LAYOUTEDIT_H
