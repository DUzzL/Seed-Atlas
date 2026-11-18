#ifndef VANILLADATA_H
#define VANILLADATA_H

// Where the vanilla data pack of a Minecraft version comes from. Datapack
// worlds build on it (packs refer to vanilla density functions and noises),
// and the app must not ship it. It is looked for in this order:
//  1. a file or folder the user chose for the version (kept in the settings),
//  2. the user's own Minecraft installation (the official launcher and some
//     other launchers keep the game's jar of every installed version),
//  3. a copy that was downloaded from Mojang before, with the user's consent,
//     into the app's cache folder.
// The download itself is started by the user, who is asked first (see
// fetchInfo, startDownload). Only Mojang's download hosts are contacted
// (allowedHost), also for redirects.

#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QCryptographicHash;
class QUrl;

struct VanillaSource
{
    enum Kind { None, Chosen, Installation, Downloaded };
    Kind kind = None;
    QString path;       // jar, zip or folder; empty if kind is None
    QString detail;     // for Installation: the launcher it was found in
};

class VanillaData : public QObject
{
    Q_OBJECT
public:
    explicit VanillaData(QObject *parent = nullptr);
    ~VanillaData();

    // The game version whose data an engine version uses ("1.21.1"); this is
    // the one a download fetches. Empty if there is none.
    static QString gameVersion(int mc);
    // All game versions whose data is known to be right for the engine
    // version, the preferred one first.
    static QStringList gameVersions(int mc);
    // Whether the game's own files of the version hold its world generation
    // data (not 1.18.2: its data is in code, only the data generator writes
    // it out). Only then can it be found in an installation or downloaded.
    static bool dataInGameFiles(int mc);
    // The hosts the download may contact, e.g. "piston-data.mojang.com".
    static QStringList allowedHosts();

    // The data of a version, or kind None. Only checks that files exist.
    VanillaSource find(int mc) const;

    // Loads the data (without packs) to see whether it gives a world with an
    // Overworld the map can show. Returns an empty string if it does, else a
    // sentence that says what is wrong. Takes some tens of milliseconds.
    static QString validate(int mc, const QString& path);

    // Remembers a file or folder the user chose for the version, if it is
    // valid. A jar or zip is used where it is; nothing is copied.
    bool choose(int mc, const QString& path, QString *error);
    // Forgets the user's choice for the version.
    void forget(int mc);

    // The folder of downloaded game files and the file of a version in it.
    static QString cacheDir();
    static QString cachedFile(int mc);

    // Whether this build can download (it has the network module).
    static bool canDownload();
    bool busy() const { return activeMc >= 0; }
    int busyVersion() const { return activeMc; }

public slots:
    // Asks Mojang's servers for the download of the version: its size and
    // address. Answers with infoReady() or finished(mc, false, error). Only
    // to be called once the user agreed to the download.
    void fetchInfo(int mc);
    // Downloads the file that infoReady() announced, checks it (SHA-1 and a
    // test load) and stores it in the cache folder. Reports progress() and
    // ends with finished().
    void startDownload();
    void cancel();

signals:
    // size in bytes, host is the server the file comes from
    void infoReady(int mc, qint64 size, const QString& host);
    void progress(int mc, qint64 received, qint64 total);
    void finished(int mc, bool ok, const QString& error);
    // find() may give another answer than before
    void changed();

private:
    void fail(const QString& error);
    void requestVersionJson(const QString& url);
    void finishDownload();
    // https and one of allowedHosts() (test runs: also the host of their
    // own manifest address)
    static bool allowedUrl(const QUrl& url);
#if WITH_NETWORK
    // a GET that only follows redirects to allowed addresses
    QNetworkReply *get(const QString& url, int timeoutMs);
#endif

    int activeMc = -1;
    int stage = 0;
    QString clientUrl;
    QString clientSha1;
    qint64 clientSize = 0;
#if WITH_NETWORK
    QNetworkAccessManager *network = nullptr;
    QNetworkReply *reply = nullptr;
    QFile *partFile = nullptr;
    QCryptographicHash *hash = nullptr;
#endif
};

#endif // VANILLADATA_H
