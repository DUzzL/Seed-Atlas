#include "vanilladata.h"

#include "config.h"
#include "datapack/dpworld.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#if WITH_NETWORK
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#endif

namespace {

// The game versions whose vanilla data belongs to an engine version. The
// names of the engine versions are not the game's ("1.19" is 1.19.4, "1.20"
// is 1.20.6), so this is a table of its own. Only versions whose data the
// datapack generator was checked with are listed (see dp::versionSupported).
struct VersionData
{
    int mc;
    const char *versions[3]; // the preferred one first
    // The game's jars hold the data (from 1.19; 1.18.2 has its worldgen data
    // in code, only its data generator writes it out).
    bool inJar = true;
};

const VersionData kVersionData[] = {
    { MC_1_18_2,  { "1.18.2", nullptr, nullptr }, false },
    { MC_1_19_2,  { "1.19.2", nullptr, nullptr } },
    { MC_1_19_4,  { "1.19.4", nullptr, nullptr } },
    { MC_1_20_6,  { "1.20.6", "1.20.1", nullptr } },
    { MC_1_21_1,  { "1.21.1", nullptr, nullptr } },
    { MC_1_21_2,  { "1.21.2", nullptr, nullptr } },
    { MC_1_21_3,  { "1.21.3", nullptr, nullptr } },
    { MC_1_21_4,  { "1.21.4", nullptr, nullptr } },
    { MC_1_21_5,  { "1.21.5", nullptr, nullptr } },
    { MC_1_21_6,  { "1.21.6", nullptr, nullptr } },
    { MC_1_21_7,  { "1.21.7", nullptr, nullptr } },
    { MC_1_21_8,  { "1.21.8", nullptr, nullptr } },
    { MC_1_21_9,  { "1.21.9", nullptr, nullptr } },
    { MC_1_21_10, { "1.21.10", nullptr, nullptr } },
    { MC_1_21_11, { "1.21.11", nullptr, nullptr } },
    // 26.1 - 26.1.2 have the same worldgen data
    { MC_26_1,    { "26.1", "26.1.1", "26.1.2" } },
    { MC_26_1_1,  { "26.1.1", "26.1", "26.1.2" } },
    { MC_26_1_2,  { "26.1.2", "26.1", "26.1.1" } },
    { MC_26_2,    { "26.2", nullptr, nullptr } },
    { MC_26_3,    { "26.3", nullptr, nullptr } },
};

const char *const kManifestUrl =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

bool testSettings()
{
    return !qEnvironmentVariableIsEmpty("SEED_ATLAS_TEST_SETTINGS_DIR")
        || !qEnvironmentVariableIsEmpty("SEED_ATLAS_TEST_SETTINGS_ID");
}

// Test runs can name another manifest address, e.g. one that does not answer.
QString testManifest()
{
    if (testSettings() && !qEnvironmentVariableIsEmpty("SEED_ATLAS_TEST_VANILLA_MANIFEST"))
        return QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_VANILLA_MANIFEST"));
    return QString();
}

struct LauncherRoot
{
    QString path;
    QString name;
};

// The data folders of the launchers that keep the game's jar per version.
QList<LauncherRoot> launcherRoots()
{
    QList<LauncherRoot> roots;
    // Test runs name their own folders (or none), so that they neither depend
    // on nor read the Minecraft installation of the machine.
    if (qEnvironmentVariableIsSet("SEED_ATLAS_TEST_VANILLA_ROOTS") || testSettings())
    {
        const QString list = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_VANILLA_ROOTS"));
        for (const QString& path : list.split(QDir::listSeparator(), Qt::SkipEmptyParts))
            roots.append({QDir::fromNativeSeparators(path), QStringLiteral("Minecraft")});
        return roots;
    }
    const QString home = QDir::homePath();
#if defined(Q_OS_MACOS)
    const QString support = home + "/Library/Application Support";
    roots.append({support + "/minecraft", QStringLiteral("Minecraft Launcher")});
    roots.append({support + "/PrismLauncher", QStringLiteral("Prism Launcher")});
    roots.append({support + "/ModrinthApp", QStringLiteral("Modrinth App")});
#elif defined(Q_OS_WIN)
    QString appdata = QDir::fromNativeSeparators(QString::fromLocal8Bit(qgetenv("APPDATA")));
    if (appdata.isEmpty())
        appdata = home + "/AppData/Roaming";
    roots.append({appdata + "/.minecraft", QStringLiteral("Minecraft Launcher")});
    roots.append({appdata + "/PrismLauncher", QStringLiteral("Prism Launcher")});
    roots.append({appdata + "/ModrinthApp", QStringLiteral("Modrinth App")});
#else
    roots.append({home + "/.minecraft", QStringLiteral("Minecraft Launcher")});
    roots.append({home + "/.var/app/com.mojang.Minecraft/.minecraft",
        QStringLiteral("Minecraft Launcher")});
    roots.append({home + "/.local/share/PrismLauncher", QStringLiteral("Prism Launcher")});
    roots.append({home + "/.var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher",
        QStringLiteral("Prism Launcher")});
    roots.append({home + "/.local/share/ModrinthApp", QStringLiteral("Modrinth App")});
#endif
    return roots;
}

// The game's jar of a version below a launcher folder, or an empty string.
QString jarInRoot(const QString& root, const QString& version)
{
    const QString candidates[] = {
        // the official launcher
        root + "/versions/" + version + "/" + version + ".jar",
        // Prism Launcher and other MultiMC descendants
        root + "/libraries/com/mojang/minecraft/" + version + "/minecraft-" + version + "-client.jar",
        // Modrinth App
        root + "/meta/versions/" + version + "/" + version + ".jar",
    };
    for (const QString& path : candidates)
    {
        if (QFileInfo(path).isFile())
            return path;
    }
    return QString();
}

QString settingsKey(int mc)
{
    return QStringLiteral("datapacks/vanilla/") + VanillaData::gameVersion(mc);
}

} // namespace

VanillaData::VanillaData(QObject *parent)
    : QObject(parent)
{
}

VanillaData::~VanillaData()
{
    cancel();
}

QStringList VanillaData::gameVersions(int mc)
{
    QStringList versions;
    for (const VersionData& data : kVersionData)
    {
        if (data.mc != mc)
            continue;
        for (const char *version : data.versions)
            if (version)
                versions.append(QLatin1String(version));
    }
    return versions;
}

bool VanillaData::dataInGameFiles(int mc)
{
    for (const VersionData& data : kVersionData)
        if (data.mc == mc)
            return data.inJar;
    return true;
}

QStringList VanillaData::allowedHosts()
{
    // What Mojang's version manifest and version files name (piston-meta for
    // the lists, piston-data for the game files), and the older names of the
    // same services.
    return {QStringLiteral("piston-meta.mojang.com"), QStringLiteral("piston-data.mojang.com"),
        QStringLiteral("launchermeta.mojang.com"), QStringLiteral("launcher.mojang.com")};
}

bool VanillaData::allowedUrl(const QUrl& url)
{
    const QString test = testManifest();
    if (!test.isEmpty())
    {
        const QUrl own(test);
        if (url.scheme() == own.scheme() && url.host() == own.host() && url.port() == own.port())
            return true;
    }
    return url.isValid() && url.scheme() == QLatin1String("https")
        && allowedHosts().contains(url.host().toLower());
}

QString VanillaData::gameVersion(int mc)
{
    const QStringList versions = gameVersions(mc);
    return versions.isEmpty() ? QString() : versions.first();
}

QString VanillaData::cacheDir()
{
    // Test runs keep their downloads next to their own settings.
    if (testSettings())
        return appConfigDir() + "/vanilla-cache";
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        base = appConfigDir();
    return base + "/vanilla";
}

QString VanillaData::cachedFile(int mc)
{
    const QString version = gameVersion(mc);
    return version.isEmpty() ? QString() : cacheDir() + "/" + version + ".jar";
}

VanillaSource VanillaData::find(int mc) const
{
    VanillaSource source;
    const QStringList versions = gameVersions(mc);
    if (versions.isEmpty())
        return source;

    QSettings settings(appSettingsId(), appSettingsId());
    const QString chosen = settings.value(settingsKey(mc)).toString();
    if (!chosen.isEmpty() && QFileInfo::exists(chosen))
    {
        source.kind = VanillaSource::Chosen;
        source.path = chosen;
        return source;
    }

    // Without data in the game's jars only the user's choice can give it.
    if (!dataInGameFiles(mc))
        return source;

    for (const QString& version : versions)
    {
        for (const LauncherRoot& root : launcherRoots())
        {
            const QString jar = jarInRoot(root.path, version);
            if (jar.isEmpty())
                continue;
            source.kind = VanillaSource::Installation;
            source.path = jar;
            source.detail = root.name;
            return source;
        }
    }

    const QString cached = cachedFile(mc);
    if (QFileInfo(cached).isFile())
    {
        source.kind = VanillaSource::Downloaded;
        source.path = cached;
    }
    return source;
}

QString VanillaData::validate(int mc, const QString& path)
{
    if (!QFileInfo::exists(path))
        return tr("\"%1\" does not exist.").arg(QDir::toNativeSeparators(path));
    dp::Request request;
    request.mc = mc;
    request.vanillaPath = path;
    dp::Report report;
    const std::shared_ptr<const dp::World> world = dp::World::load(request, &report);
    for (const dp::Issue& issue : report.issues)
    {
        if (issue.severity == dp::Issue::Error)
            return issue.text;
    }
    const int overworld = world->dimension(QStringLiteral("minecraft:overworld"));
    if ((overworld < 0 || !world->dimensions()[overworld].supported) && !dataInGameFiles(mc))
    {
        return tr("This is not the world generation data of Minecraft %1. The game's files do not "
            "contain it: choose the folder reports/worldgen that the game's data generator writes, "
            "or a folder that holds data/minecraft/worldgen.").arg(gameVersion(mc));
    }
    if (overworld < 0 || !world->dimensions()[overworld].supported)
    {
        return tr("This is not the world generation data of Minecraft %1. Choose the game's "
            "own file \"%1.jar\", the server file, or a folder that holds "
            "data/minecraft/worldgen.").arg(gameVersion(mc));
    }
    return QString();
}

bool VanillaData::choose(int mc, const QString& path, QString *error)
{
    if (gameVersion(mc).isEmpty())
    {
        if (error)
            *error = tr("Datapack worlds are not available for this Minecraft version.");
        return false;
    }
    const QString problem = validate(mc, path);
    if (!problem.isEmpty())
    {
        if (error)
            *error = problem;
        return false;
    }
    QSettings settings(appSettingsId(), appSettingsId());
    settings.setValue(settingsKey(mc), QFileInfo(path).absoluteFilePath());
    emit changed();
    return true;
}

void VanillaData::forget(int mc)
{
    if (gameVersion(mc).isEmpty())
        return;
    QSettings settings(appSettingsId(), appSettingsId());
    settings.remove(settingsKey(mc));
    emit changed();
}

bool VanillaData::canDownload()
{
#if WITH_NETWORK
    return true;
#else
    return false;
#endif
}

#if WITH_NETWORK

namespace {

enum { STAGE_IDLE, STAGE_MANIFEST, STAGE_VERSION, STAGE_READY, STAGE_FILE };

// Downloads (and the stale files of interrupted ones) carry this ending.
const char *const kPartSuffix = ".part";

// Qt's transfer timeout counts from the last data that arrived: a slow but
// steady transfer is not cut off, a stalled one ends with TimeoutError.
const int kListTimeoutMs = 30000;
const int kFileTimeoutMs = 60000;

QString networkProblem(QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::TimeoutError)
        return VanillaData::tr("Mojang's download server stopped sending data. Check the internet "
            "connection and try again, or choose the game file yourself.");
    return VanillaData::tr("Mojang's download server could not be reached (%1). "
        "Check the internet connection, or choose the game file yourself.")
        .arg(reply->errorString());
}

} // namespace

QNetworkReply *VanillaData::get(const QString& url, int timeoutMs)
{
    QNetworkRequest request{QUrl(url)};
    // every redirect is checked against the hosts of the download
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setTransferTimeout(timeoutMs);
    QNetworkReply *r = network->get(request);
    connect(r, &QNetworkReply::redirected, this, [this, r](const QUrl& to) {
        if (allowedUrl(to))
            emit r->redirectAllowed();
        else
            fail(tr("Mojang's server sent the download on to another address (%1), which is not "
                "one of Mojang's download servers. Nothing was downloaded from there.")
                .arg(to.host()));
    });
    return r;
}

void VanillaData::fail(const QString& error)
{
    const int mc = activeMc;
    if (reply)
    {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        reply = nullptr;
    }
    if (partFile)
    {
        partFile->close();
        partFile->remove();
        delete partFile;
        partFile = nullptr;
    }
    delete hash;
    hash = nullptr;
    activeMc = -1;
    stage = STAGE_IDLE;
    if (mc >= 0)
        emit finished(mc, false, error);
}

void VanillaData::cancel()
{
    if (activeMc >= 0)
        fail(tr("The download was cancelled."));
}

void VanillaData::fetchInfo(int mc)
{
    if (activeMc >= 0)
        return;
    const QString version = gameVersion(mc);
    if (version.isEmpty())
    {
        emit finished(mc, false, tr("Datapack worlds are not available for this Minecraft version."));
        return;
    }
    if (!dataInGameFiles(mc))
    {
        emit finished(mc, false, tr("The game files of Minecraft %1 do not contain its world generation "
            "data. Choose the folder reports/worldgen that the game's data generator writes, or a "
            "folder that holds data/minecraft/worldgen.").arg(version));
        return;
    }
    if (!network)
        network = new QNetworkAccessManager(this);
    activeMc = mc;
    stage = STAGE_MANIFEST;
    QString manifest = QLatin1String(kManifestUrl);
    if (!testManifest().isEmpty())
        manifest = testManifest();
    reply = get(manifest, kListTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, version]() {
        QNetworkReply *done = reply;
        reply = nullptr;
        done->deleteLater();
        if (done->error() != QNetworkReply::NoError)
            return fail(networkProblem(done));
        const QJsonArray versions = QJsonDocument::fromJson(done->readAll())
            .object().value(QLatin1String("versions")).toArray();
        for (const QJsonValue& entry : versions)
        {
            const QJsonObject object = entry.toObject();
            if (object.value(QLatin1String("id")).toString() == version)
                return requestVersionJson(object.value(QLatin1String("url")).toString());
        }
        fail(tr("Mojang's version list does not name Minecraft %1.").arg(version));
    });
}

void VanillaData::requestVersionJson(const QString& url)
{
    if (!allowedUrl(QUrl(url)))
        return fail(tr("Mojang's version list gave an unexpected address."));
    stage = STAGE_VERSION;
    reply = get(url, kListTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *done = reply;
        reply = nullptr;
        done->deleteLater();
        if (done->error() != QNetworkReply::NoError)
            return fail(networkProblem(done));
        const QJsonObject client = QJsonDocument::fromJson(done->readAll()).object()
            .value(QLatin1String("downloads")).toObject()
            .value(QLatin1String("client")).toObject();
        clientUrl = client.value(QLatin1String("url")).toString();
        clientSha1 = client.value(QLatin1String("sha1")).toString().toLower();
        clientSize = qint64(client.value(QLatin1String("size")).toDouble());
        if (!allowedUrl(QUrl(clientUrl)) || clientSha1.size() != 40 || clientSize <= 0)
            return fail(tr("Mojang's version data does not describe the game file."));
        stage = STAGE_READY;
        emit infoReady(activeMc, clientSize, QUrl(clientUrl).host());
    });
}

void VanillaData::startDownload()
{
    if (activeMc < 0 || stage != STAGE_READY)
        return;
    const QString target = cachedFile(activeMc);
    if (!QDir().mkpath(cacheDir()))
        return fail(tr("The folder \"%1\" could not be created.")
            .arg(QDir::toNativeSeparators(cacheDir())));
    // Files of downloads that ended without cleaning up (a crash, a lost
    // disk) are removed after a day; a running download of another instance
    // of the app has its own name and is younger.
    const QDir cache(cacheDir());
    for (const QFileInfo& old : cache.entryInfoList({QStringLiteral("*") + kPartSuffix}, QDir::Files))
        if (old.lastModified().secsTo(QDateTime::currentDateTime()) > 24 * 3600)
            QFile::remove(old.absoluteFilePath());
    // a name of its own: two instances of the app may download the same version
    partFile = new QFile(target + QStringLiteral(".%1-%2").arg(QCoreApplication::applicationPid())
        .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'))
        + QLatin1String(kPartSuffix));
    if (!partFile->open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        const QString error = partFile->errorString();
        delete partFile;
        partFile = nullptr;
        return fail(tr("The file could not be written: %1").arg(error));
    }
    hash = new QCryptographicHash(QCryptographicHash::Sha1);
    stage = STAGE_FILE;
    reply = get(clientUrl, kFileTimeoutMs);
    connect(reply, &QNetworkReply::readyRead, this, [this]() {
        const QByteArray chunk = reply->readAll();
        hash->addData(chunk);
        if (partFile->write(chunk) != chunk.size())
            fail(tr("The file could not be written: %1").arg(partFile->errorString()));
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        emit progress(activeMc, received, total > 0 ? total : clientSize);
    });
    connect(reply, &QNetworkReply::finished, this, &VanillaData::finishDownload);
}

void VanillaData::finishDownload()
{
    QNetworkReply *done = reply;
    reply = nullptr;
    done->deleteLater();
    if (done->error() != QNetworkReply::NoError)
        return fail(networkProblem(done));
    const QByteArray rest = done->readAll();
    hash->addData(rest);
    partFile->write(rest);
    const bool written = partFile->flush() && partFile->error() == QFileDevice::NoError;
    const qint64 size = partFile->size();
    partFile->close();
    const QString sha1 = QString::fromLatin1(hash->result().toHex());
    if (!written)
        return fail(tr("The file could not be written completely."));
    if (size != clientSize || sha1 != clientSha1)
        return fail(tr("The downloaded file is damaged (its checksum does not match). Try again."));

    const int mc = activeMc;
    const QString part = partFile->fileName();
    const QString target = cachedFile(mc);
    delete partFile;
    partFile = nullptr;
    delete hash;
    hash = nullptr;

    const QString problem = validate(mc, part);
    if (!problem.isEmpty())
    {
        QFile::remove(part);
        activeMc = -1;
        stage = STAGE_IDLE;
        emit finished(mc, false, problem);
        return;
    }
    QFile::remove(target);
    if (!QFile::rename(part, target))
    {
        QFile::remove(part);
        activeMc = -1;
        stage = STAGE_IDLE;
        emit finished(mc, false, tr("The file could not be stored in \"%1\".")
            .arg(QDir::toNativeSeparators(cacheDir())));
        return;
    }
    activeMc = -1;
    stage = STAGE_IDLE;
    emit finished(mc, true, QString());
    emit changed();
}

#else // no network module (WebAssembly)

void VanillaData::fail(const QString&) {}
void VanillaData::requestVersionJson(const QString&) {}
void VanillaData::finishDownload() {}
void VanillaData::cancel() {}
void VanillaData::startDownload() {}

void VanillaData::fetchInfo(int mc)
{
    emit finished(mc, false, tr("This build cannot download files. Choose the game file yourself."));
}

#endif
