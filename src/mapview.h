#ifndef MAPVIEW_H
#define MAPVIEW_H

#include "world.h"

#include <QDockWidget>
#include <QElapsedTimer>
#include <QTimer>

class MapOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit MapOverlay(QWidget *parent = nullptr)
        : QWidget(parent),pos{0,0},bname() {}
    ~MapOverlay() {}

public slots:
    bool event(QEvent *) Q_DECL_OVERRIDE;
    void paintEvent(QPaintEvent *) Q_DECL_OVERRIDE;

public:
    Pos pos;
    QString bname;
    bool showText = true; // off while a status strip shows the cursor info
};

class MapView : public QWidget
{
    Q_OBJECT

public:
    explicit MapView(QWidget *parent = nullptr);
    virtual ~MapView();

    qreal getX();
    qreal getZ();
    qreal getScale() const { return 1.0 / blocks2pix; }
    void getVisible(int *x0, int *z0, int *x1, int *z1);

    void deleteWorld();
    void setSeed(WorldInfo wi, int dim, LayerOpt lopt);
    void setView(qreal x, qreal z, qreal scale = 0);
    void animateView(qreal x, qreal z, qreal scale);
    void zoom(qreal factor);

    // animation
    qreal x_src, z_src, s_src;
    qreal x_dst, z_dst, s_dst;
    qreal s_mul;
    qreal atime;
    QElapsedTimer anielapsed;

    bool getShow(int stype);
    void setShow(int stype, bool v);
    void setConfig(const Config& config);
    void setShapes(const std::vector<Shape>& shapes);
    void refreshBiomeColors();
    void setBiomeHighlight(int biomeId);
    void setBiomeHighlights(const QVector<int>& biomeIds);
    // The highlighted biomes of datapack worlds, by biome id
    // ("minecraft:plains"). They are kept apart from the highlight of the
    // built-in biomes and apply to every datapack world that has them.
    void setDatapackHighlights(const QStringList& biomeIds);
    QStringList getDatapackHighlights() const { return dpHighlightIds; }
    // The structures of datapack worlds that are shown, by id. Vanilla
    // structures that have a map option (villages, ...) follow that option
    // instead (see dpVanillaStructureOpt()).
    void setDatapackStructures(const QStringList& ids);
    QStringList getDatapackStructures() const { return dpStructureIds; }
    // the structures of the shown datapack world that the map shows, one
    // flag per structure of the world (empty without a datapack world)
    std::vector<char> getDatapackStructureMask() const;
    // A message in the middle of the map while there is no world to show
    // (e.g. while data packs are loading).
    void setPlaceholder(const QString& text);

    void timeout();

    void update(int cnt = 1);

    VarPos getActivePos();

    QPixmap screenshot();

    // the biome name and position under the cursor are drawn in the map's
    // corner unless the window shows them elsewhere (cursorInfoChanged)
    void setCursorTextVisible(bool visible);

private:
    void settingsToWorld();
    void reportCursorInfo(const QString& biome, Pos pos, bool hasWorld);
    void runAni();
    void scheduleFrame();

signals:
    void layerChange(int mode, int disp);
    void cursorInfoChanged(const QString& biome, int x, int z, qreal scale, bool hasWorld);

public slots:
    void mapUpdate();
    void showContextMenu(const QPoint &pos);
    void copySeed();
    void copyText(QString txt);
    void onGoto();

    void paintEvent(QPaintEvent *) Q_DECL_OVERRIDE;
    void resizeEvent(QResizeEvent *) Q_DECL_OVERRIDE;

    void wheelEvent(QWheelEvent *) Q_DECL_OVERRIDE;
    void mousePressEvent(QMouseEvent *) Q_DECL_OVERRIDE;
    void mouseMoveEvent(QMouseEvent *) Q_DECL_OVERRIDE;
    void mouseReleaseEvent(QMouseEvent *) Q_DECL_OVERRIDE;

    void keyPressEvent(QKeyEvent *) Q_DECL_OVERRIDE;

public:
    QWorld *world;

    QElapsedTimer elapsed1;
    QElapsedTimer frameelapsed;
    QElapsedTimer actelapsed;
    qreal decay;

    MapOverlay *overlay;

private:
    qreal blocks2pix;
    qreal focusx, focusz;
    qreal prevx, prevz;
    qreal velx, velz;
    qreal mtime;

    bool holding;
    QPoint mstart, mprev;
    Pos bstart;
    bool measure;
    int updatecounter;
    QTimer frameTimer;
    QVector<int> biomeHighlights;
    QStringList dpHighlightIds;
    QStringList dpStructureIds;
    QString placeholder;
    void applyDatapackHighlights();

    std::vector<Shape> shapes;

    bool sshow[D_STRUCT_NUM];
    LayerOpt lopt;
    Config config;

    // last values sent with cursorInfoChanged()
    QString reportedBiome;
    Pos reportedPos = {0, 0};
    qreal reportedScale = 0;
    bool reportedWorld = false;
};

#endif // MAPVIEW_H
