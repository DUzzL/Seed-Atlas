#ifndef UPDATER_H
#define UPDATER_H

#include <QByteArray>
#include <QString>

class QWidget;
struct Config;

// Checks the manually maintained GitHub release for an available build.
void searchForUpdates(QWidget *parent, Config *config, bool quiet);

enum class UpdateDecision
{
    Invalid,    // the manifest cannot be used
    UpToDate,   // the published build is this one
    Newer,      // the published build is newer
    NotNewer,   // the published build is older (or as old), e.g. a test build
    Different,  // the builds differ, but the manifest cannot order them
};

// Compares this build with an update.json manifest:
//   { "buildId": "<40 hex digits>", "buildTime": <commit time, s>, "version": "..." }
// Only "buildId" is required (manifests before 4.3 hold nothing else).
UpdateDecision decideUpdate(const QString& localBuildId, qint64 localBuildTime,
                            const QByteArray& manifest);

#endif // UPDATER_H
