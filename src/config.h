#ifndef CONFIG_H
#define CONFIG_H

#include "seedatlas-engine/finders.h"

#include <QSettings>
#include <QString>
#include <QFont>
#include <QTextStream>

#include <vector>


#define APP_STRING "seed-atlas"

inline QString appSettingsId()
{
    const QByteArray testId = qgetenv("SEED_ATLAS_TEST_SETTINGS_ID");
    return testId.isEmpty() ? QStringLiteral(APP_STRING) : QString::fromLocal8Bit(testId);
}

// The folder of the files next to the settings: the automatic session, its
// backup and the Lua scripts. Test runs that select their own settings (with
// SEED_ATLAS_TEST_SETTINGS_DIR or SEED_ATLAS_TEST_SETTINGS_ID) get their own
// folder, so they never touch the files of the real installation. The folder
// is created if it does not exist.
QString appConfigDir();
#define APP_DISPLAY_NAME "Seed Atlas"

#define PRECOMPUTE48_BUFSIZ ((int64_t)1 << 30)

enum { MC_DEFAULT = MC_NEWEST };

struct ExtGenConfig
{
    bool experimentalVers;
    bool estimateTerrain;
    bool saltOverride;
    uint64_t salts[FEATURE_NUM];

    ExtGenConfig() { reset(); }

    void reset();
    void load(QSettings& settings);
    void save(QSettings& settings);
    void load() { QSettings s(appSettingsId(), appSettingsId()); load(s); }
    void save() { QSettings s(appSettingsId(), appSettingsId()); save(s); }

    // the settings that change generated worlds (not experimentalVers)
    bool sameGeneration(const ExtGenConfig& o) const;
};

extern unsigned char g_biomeColors[256][3];
extern unsigned char g_tempsColors[256][3];

extern qreal g_fontscale;
extern qreal g_iconscale;

// Keep the extended generator settings in global scope. g_extgen belongs to
// the GUI thread (or to the start of a headless run); worker threads read the
// published copy of activeExtGen() instead, which never changes while they
// hold it. Call publishExtGen() after changing g_extgen.
extern ExtGenConfig g_extgen;
void publishExtGen();
const ExtGenConfig& activeExtGen();
// Replaces g_extgen with the generation settings of a session (keeping the
// experimental versions option) and publishes the result.
void applyExtGen(const ExtGenConfig& extgen);


struct WorldInfo
{
    int mc;
    bool large;
    uint64_t seed;
    int y;

    WorldInfo() { reset(); }

    bool equals(const WorldInfo& wi) const;

    void reset();
    void load(QSettings& settings);
    void save(QSettings& settings);
    void load() { QSettings s(appSettingsId(), appSettingsId()); load(s); }
    void save() { QSettings s(appSettingsId(), appSettingsId()); save(s); }
    bool read(const QString& line);
    void write(QTextStream& stream);
};


enum {
    LOPT_BIOMES,
    LOPT_NOISE_PARA,
    LOPT_NOISE_T_4 = LOPT_NOISE_PARA,
    LOPT_NOISE_H_4,
    LOPT_NOISE_C_4,
    LOPT_NOISE_E_4,
    LOPT_NOISE_D_4,
    LOPT_NOISE_W_4,
    LOPT_RIVER_4,
    LOPT_OCEAN_256,
    LOPT_NOOCEAN_1,
    LOPT_BETA_T_1,
    LOPT_BETA_H_1,
    LOPT_HEIGHT,
    LOPT_TERRAIN,
    LOPT_STRUCTS,
    LOPT_MAX,
};

struct LayerOpt
{
    int8_t mode;
    int8_t disp[LOPT_MAX];

    LayerOpt() { reset(); }

    void reset();

    int activeDisp() const;
    bool activeDifference(const LayerOpt& l) const;
    bool isClimate(int mc) const;
};

enum {
    D_NONE = -1,
    // generics
    D_GRID,
    D_SLIME,
    // structures
    D_DESERT,
    D_JUNGLE,
    D_IGLOO,
    D_HUT,
    D_VILLAGE,
    D_MANSION,
    D_MONUMENT,
    D_OCEANRUIN,
    D_SHIPWRECK,
    D_TREASURE,
    D_MINESHAFT,
    D_WELL,
    D_GEODE,
    D_OUTPOST,
    D_ANCIENTCITY,
    D_TRAILRUINS,
    D_CHAMBERS,
    D_CAMP,
    D_OREVEIN,
    D_PORTAL,
    D_PORTALN,
    D_FORTESS,
    D_BASTION,
    D_ENDCITY,
    D_ENDCITYSHIP,
    D_GATEWAY,
    // non-recurring structures
    D_SPAWN,
    D_STRONGHOLD,
    D_STRUCT_NUM
};

QString mapopt2display(int opt);
const char *mapopt2str(int opt);
int str2mapopt(const char *s);
int mapopt2stype(int opt);

struct MapConfig
{
    struct Opt
    {
        double scale;
        bool enabled;
        Opt() : scale(),enabled() {}
    };
    Opt opts[D_STRUCT_NUM];
    bool zoomEnabled;

    MapConfig(bool init = true);

    double scale(int opt) const { return opts[opt].scale; }
    bool enabled(int opt) const { return opts[opt].enabled; }
    bool hasScale(int opt) const;
    int getDim(int opt) const;
    int getTileSize(int opt) const;

    bool sameMapOpts(const MapConfig& a) const;

    void reset();
    void load(QSettings& settings);
    void save(QSettings& settings);
    void load() { QSettings s(appSettingsId(), appSettingsId()); load(s); }
    void save() { QSettings s(appSettingsId(), appSettingsId()); save(s); }
};


// Keep STYLE_DARK at index 1 for settings compatibility with 4.1/4.2 builds.
enum { STYLE_SYSTEM, STYLE_DARK, STYLE_LIGHT };

struct Config
{
    bool smoothMotion;
    bool showBBoxes;
    bool restoreSession;
    bool restoreWindow;
    bool checkForUpdates;
    bool advancedSearch; // show the expert seed finding options
    int autosaveCycle;
    int uistyle;
    int maxMatching;
    int gridSpacing;
    int gridMultiplier;
    int mapCacheSize;
    int cpuUsagePercent;
    int mapThreads;
    bool useGpu; // searches may use the GPU where it is faster
    QString biomeColorPath;
    QString separator;
    QString quote;
    QFont fontNorm;
    QFont fontMono;
    qreal iconScale;

    Config() { reset(); }

    void reset();
    void load(QSettings& settings);
    void save(QSettings& settings);
    void load() { QSettings s(appSettingsId(), appSettingsId()); load(s); }
    void save() { QSettings s(appSettingsId(), appSettingsId()); save(s); }
};

int threadCountForCpuUsage(int percent);
int configuredThreadCount();
void setConfiguredCpuUsagePercent(int percent);

enum { GEN48_AUTO, GEN48_QH, GEN48_QM, GEN48_LIST, GEN48_NONE };
enum { IDEAL, CLASSIC, NORMAL, BARELY, IDEAL_SALTED };

struct Gen48Config
{
    int mode;
    QString slist48path;
    uint64_t salt;
    uint64_t listsalt;
    int qual;
    int qmarea;
    bool manualarea;
    int x1, z1, x2, z2;

    Gen48Config() { reset(); }

    void reset();
    bool read(const QString& line);
    void write(QTextStream& stream);

    uint64_t estimateSeedCnt(uint64_t slist48len);
};

// search type options from combobox
enum { SEARCH_INC = 0, SEARCH_BLOCKS = 1, SEARCH_LIST = 2, SEARCH_48ONLY = 3 };

struct SearchConfig
{
    int searchtype;
    QString slist64path;
    int threads;
    uint64_t startseed;
    bool stoponres;
    uint64_t smin;
    uint64_t smax;
    // World generation settings the results depend on. reset() takes the
    // current g_extgen; a session file overrides them only with lines it
    // actually contains, which are written only for values that differ from
    // the defaults (so default sessions stay readable by older versions).
    ExtGenConfig extgen;
    bool extgenSaltsRead;

    SearchConfig() { reset(); }

    void reset();
    bool read(const QString& line);
    void write(QTextStream& stream);
};


Q_DECLARE_METATYPE(int64_t)
Q_DECLARE_METATYPE(uint64_t)
Q_DECLARE_METATYPE(Pos)
Q_DECLARE_METATYPE(Config)


#endif // CONFIG_H
