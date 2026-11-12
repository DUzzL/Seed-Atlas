#include "updater.h"

#include "config.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QUrl>

#ifndef SEED_ATLAS_BUILD_ID
#define SEED_ATLAS_BUILD_ID "development"
#endif
#ifndef SEED_ATLAS_BUILD_TIME
#define SEED_ATLAS_BUILD_TIME 0
#endif

namespace {

const QUrl kManifestUrl(QStringLiteral(
    "https://github.com/DUzzL/Seed-Atlas/releases/latest/download/update.json"));
const QUrl kReleaseUrl(QStringLiteral(
    "https://github.com/DUzzL/Seed-Atlas/releases/latest"));

QString currentBuildId()
{
    const QByteArray overrideId = qgetenv("SEED_ATLAS_UPDATE_BUILD_ID");
    return overrideId.isEmpty()
        ? QStringLiteral(SEED_ATLAS_BUILD_ID)
        : QString::fromLatin1(overrideId);
}

// Commit time of this build in seconds since 1970 (0: unknown).
qint64 currentBuildTime()
{
    const QByteArray overrideTime = qgetenv("SEED_ATLAS_UPDATE_BUILD_TIME");
    if (!overrideTime.isEmpty())
        return overrideTime.toLongLong();
    return qint64(SEED_ATLAS_BUILD_TIME);
}

QUrl manifestUrl()
{
    const QByteArray overrideUrl = qgetenv("SEED_ATLAS_UPDATE_URL");
    return overrideUrl.isEmpty()
        ? kManifestUrl
        : QUrl::fromUserInput(QString::fromLocal8Bit(overrideUrl));
}

bool isValidBuildId(const QString& buildId)
{
    if (buildId.size() != 40)
        return false;
    for (const QChar character : buildId)
    {
        const ushort code = character.toLower().unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f')))
            return false;
    }
    return true;
}

void setUpdateChecksEnabled(Config *config, bool enabled)
{
    if (config)
        config->checkForUpdates = enabled;
    QSettings settings(appSettingsId(), appSettingsId());
    settings.setValue("updater/enabled", enabled);
    settings.sync();
}

// The update only opens the release page. The installers replace the
// installed copy (the Windows setup upgrades in place, the DMG and the
// Flatpak bundle replace the app), so this copy stays usable until then.
void showUpdatePrompt(QWidget *parent, Config *config, const QString& version, bool ordered)
{
    QString text = ordered
        ? QApplication::translate("UpdaterDialog", "A newer Seed Atlas build is available on GitHub.")
        : QApplication::translate("UpdaterDialog", "A different Seed Atlas build is published on GitHub.");
    if (!version.isEmpty())
        text += "\n" + QApplication::translate("UpdaterDialog", "Published version: %1").arg(version);
    text += "\n\n" + QApplication::translate("UpdaterDialog",
        "Download opens the release page. Installing the new build replaces this copy; "
        "until then this copy keeps working.");

    QMessageBox message(QMessageBox::Information,
        QApplication::translate("UpdaterDialog", "Seed Atlas Update"),
        text, QMessageBox::NoButton, parent);
    QPushButton *downloadButton = message.addButton(
        QApplication::translate("UpdaterDialog", "Download"), QMessageBox::AcceptRole);
    QPushButton *notNowButton = message.addButton(
        QApplication::translate("UpdaterDialog", "Not now"), QMessageBox::RejectRole);
    QCheckBox *doNotAskAgain = new QCheckBox(
        QApplication::translate("UpdaterDialog", "Do not check for updates at start"), &message);
    message.setCheckBox(doNotAskAgain);
    message.setDefaultButton(downloadButton);
    message.setEscapeButton(notNowButton);
    message.exec();

    if (doNotAskAgain->isChecked())
        setUpdateChecksEnabled(config, false);
    if (message.clickedButton() != downloadButton)
        return;

    if (!QDesktopServices::openUrl(kReleaseUrl))
    {
        QMessageBox::warning(parent,
            QApplication::translate("UpdaterDialog", "Seed Atlas Update"),
            QApplication::translate(
                "UpdaterDialog", "The GitHub release page could not be opened:\n%1")
                .arg(kReleaseUrl.toString()));
    }
}

void showCheckError(QWidget *parent, const QString& message, bool quiet)
{
    if (!quiet)
    {
        QMessageBox::warning(parent,
            QApplication::translate("UpdaterDialog", "Seed Atlas Update"), message);
    }
}

void showInformation(QWidget *parent, const QString& message, bool quiet)
{
    if (!quiet)
    {
        QMessageBox::information(parent,
            QApplication::translate("UpdaterDialog", "Seed Atlas Update"), message);
    }
}

} // namespace

UpdateDecision decideUpdate(const QString& localBuildId, qint64 localBuildTime,
                            const QByteArray& manifest)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifest, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return UpdateDecision::Invalid;
    const QJsonObject object = document.object();
    const QString remoteBuildId = object.value(QStringLiteral("buildId")).toString().trimmed();
    if (!isValidBuildId(remoteBuildId))
        return UpdateDecision::Invalid;
    if (remoteBuildId.compare(localBuildId, Qt::CaseInsensitive) == 0)
        return UpdateDecision::UpToDate;

    // "buildTime" (commit time, added in 4.3) orders the builds; manifests
    // without it can only tell that the builds differ.
    const QJsonValue time = object.value(QStringLiteral("buildTime"));
    const qint64 remoteBuildTime = time.isDouble() ? qint64(time.toDouble()) : 0;
    if (remoteBuildTime <= 0 || localBuildTime <= 0)
        return UpdateDecision::Different;
    return remoteBuildTime > localBuildTime ? UpdateDecision::Newer : UpdateDecision::NotNewer;
}

void searchForUpdates(QWidget *parent, Config *config, bool quiet)
{
    const QString localBuildId = currentBuildId().trimmed();
    if (localBuildId.isEmpty() || localBuildId == QStringLiteral("development"))
    {
        showCheckError(parent,
            QApplication::translate(
                "UpdaterDialog", "Update checks are unavailable for this development build."),
            quiet);
        return;
    }
    const qint64 localBuildTime = currentBuildTime();

    // a repeated click while a check is pending would stack the prompts
    static bool checking = false;
    if (checking)
        return;
    checking = true;

    QObject *owner = parent
        ? static_cast<QObject *>(parent)
        : static_cast<QObject *>(qApp);
    QNetworkAccessManager *manager = new QNetworkAccessManager(owner);
    QObject::connect(manager, &QObject::destroyed, []() { checking = false; });
    QNetworkRequest request(manifestUrl());
    request.setTransferTimeout(15000); // Qt does not time out stalled transfers by default
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("User-Agent", "Seed-Atlas-Updater");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = manager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, manager,
        [config, manager, parent, quiet, reply, localBuildId, localBuildTime]() {
        const QNetworkReply::NetworkError networkError = reply->error();
        const QByteArray response = reply->read(64 * 1024); // the manifest is tiny
        reply->deleteLater();
        manager->deleteLater();

        if (networkError != QNetworkReply::NoError)
        {
            showCheckError(parent,
                QApplication::translate(
                    "UpdaterDialog", "Seed Atlas could not check GitHub for updates."),
                quiet);
            return;
        }

        const UpdateDecision decision = decideUpdate(localBuildId, localBuildTime, response);
        switch (decision)
        {
        case UpdateDecision::Invalid:
            showCheckError(parent,
                QApplication::translate(
                    "UpdaterDialog", "GitHub returned invalid update information."),
                quiet);
            return;
        case UpdateDecision::UpToDate:
            showInformation(parent,
                QApplication::translate("UpdaterDialog", "This Seed Atlas build is up to date."),
                quiet);
            return;
        case UpdateDecision::NotNewer:
            showInformation(parent,
                QApplication::translate("UpdaterDialog",
                    "This Seed Atlas build is newer than the published release."),
                quiet);
            return;
        case UpdateDecision::Newer:
        case UpdateDecision::Different:
            break;
        }
        const QJsonDocument document = QJsonDocument::fromJson(response);
        const QString version = document.object().value(QStringLiteral("version")).toString().trimmed();
        showUpdatePrompt(parent, config, version.left(32), decision == UpdateDecision::Newer);
    });
}
