#include "updater.h"

#include "config.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#ifndef SEED_ATLAS_BUILD_ID
#define SEED_ATLAS_BUILD_ID "development"
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

bool scheduleSelfRemoval(QString *error)
{
#ifdef Q_OS_WIN
    const QDir installDir(QCoreApplication::applicationDirPath());
    const QStringList uninstallers = installDir.entryList(
        QStringList() << QStringLiteral("unins???.exe"), QDir::Files, QDir::Name);
    if (uninstallers.isEmpty())
    {
        *error = QApplication::translate(
            "UpdaterDialog",
            "Seed Atlas could not find its uninstaller. This portable copy was not deleted.");
        return false;
    }

    const QString uninstaller = installDir.absoluteFilePath(uninstallers.first());
    const QStringList arguments = {
        QStringLiteral("/VERYSILENT"),
        QStringLiteral("/SUPPRESSMSGBOXES"),
        QStringLiteral("/NORESTART")
    };
    if (!QProcess::startDetached(uninstaller, arguments, installDir.absolutePath()))
    {
        *error = QApplication::translate(
            "UpdaterDialog", "Seed Atlas could not start its uninstaller.");
        return false;
    }
    return true;
#elif defined(Q_OS_MACOS)
    QDir bundleDir(QCoreApplication::applicationDirPath());
    if (bundleDir.dirName() != QStringLiteral("MacOS") || !bundleDir.cdUp() ||
        bundleDir.dirName() != QStringLiteral("Contents") || !bundleDir.cdUp())
    {
        *error = QApplication::translate(
            "UpdaterDialog", "Seed Atlas is not running from a macOS application bundle.");
        return false;
    }

    const QFileInfo bundle(bundleDir.absolutePath());
    if (!bundle.isDir() || !bundle.fileName().endsWith(QStringLiteral(".app"), Qt::CaseInsensitive) ||
        !QFile::moveToTrash(bundle.absoluteFilePath()))
    {
        *error = QApplication::translate(
            "UpdaterDialog",
            "Seed Atlas could not move itself to the Trash. Please remove the application manually.");
        return false;
    }
    return true;
#elif defined(Q_OS_LINUX)
    if (qEnvironmentVariable("FLATPAK_ID") == QStringLiteral("org.seedatlas.SeedAtlas"))
    {
        const QString flatpakSpawn = QStandardPaths::findExecutable(QStringLiteral("flatpak-spawn"));
        const QStringList arguments = {
            QStringLiteral("--host"),
            QStringLiteral("flatpak"),
            QStringLiteral("uninstall"),
            QStringLiteral("--user"),
            QStringLiteral("--noninteractive"),
            QStringLiteral("org.seedatlas.SeedAtlas")
        };
        if (!flatpakSpawn.isEmpty() && QProcess::startDetached(flatpakSpawn, arguments))
            return true;

        *error = QApplication::translate(
            "UpdaterDialog", "Seed Atlas could not start the Flatpak uninstaller.");
        return false;
    }

    const QString executable = QCoreApplication::applicationFilePath();
    if (!QFileInfo(executable).isFile() || !QFile::moveToTrash(executable))
    {
        *error = QApplication::translate(
            "UpdaterDialog",
            "Seed Atlas could not move its executable to the Trash. Please remove it manually.");
        return false;
    }
    return true;
#else
    *error = QApplication::translate(
        "UpdaterDialog", "Automatic removal is not supported on this operating system.");
    return false;
#endif
}

void showUpdatePrompt(QWidget *parent, Config *config)
{
    QMessageBox message(QMessageBox::Information,
        QApplication::translate("UpdaterDialog", "Seed Atlas Update"),
        QApplication::translate(
            "UpdaterDialog",
            "A new Seed Atlas update is available. Choosing Update opens the GitHub "
            "download page and removes this installed copy."),
        QMessageBox::NoButton, parent);
    QPushButton *updateButton = message.addButton(
        QApplication::translate("UpdaterDialog", "Update"), QMessageBox::AcceptRole);
    QPushButton *notNowButton = message.addButton(
        QApplication::translate("UpdaterDialog", "Not now"), QMessageBox::RejectRole);
    QCheckBox *doNotAskAgain = new QCheckBox(
        QApplication::translate("UpdaterDialog", "Do not ask again"), &message);
    message.setCheckBox(doNotAskAgain);
    message.setDefaultButton(updateButton);
    message.setEscapeButton(notNowButton);
    message.exec();

    if (doNotAskAgain->isChecked())
        setUpdateChecksEnabled(config, false);
    if (message.clickedButton() != updateButton)
        return;

    if (!QDesktopServices::openUrl(kReleaseUrl))
    {
        QMessageBox::warning(parent,
            QApplication::translate("UpdaterDialog", "Seed Atlas Update"),
            QApplication::translate(
                "UpdaterDialog", "The GitHub release page could not be opened."));
        return;
    }

    if (qEnvironmentVariableIsSet("SEED_ATLAS_UPDATE_DRY_RUN"))
        return;

    QString error;
    if (!scheduleSelfRemoval(&error))
    {
        QMessageBox::warning(parent,
            QApplication::translate("UpdaterDialog", "Seed Atlas Update"), error);
        return;
    }

    // Closing the windows first preserves settings and the current session.
    QTimer::singleShot(0, qApp, []() {
        QApplication::closeAllWindows();
        QCoreApplication::quit();
    });
}

void showCheckError(QWidget *parent, const QString& message, bool quiet)
{
    if (!quiet)
    {
        QMessageBox::warning(parent,
            QApplication::translate("UpdaterDialog", "Seed Atlas Update"), message);
    }
}

} // namespace

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

    QObject *owner = parent
        ? static_cast<QObject *>(parent)
        : static_cast<QObject *>(qApp);
    QNetworkAccessManager *manager = new QNetworkAccessManager(owner);
    QNetworkRequest request(manifestUrl());
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("User-Agent", "Seed-Atlas-Updater");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = manager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, manager,
        [config, manager, parent, quiet, reply, localBuildId]() {
        const QNetworkReply::NetworkError networkError = reply->error();
        const QByteArray response = reply->readAll();
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

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
        const QString remoteBuildId = document.isObject()
            ? document.object().value(QStringLiteral("buildId")).toString().trimmed()
            : QString();
        if (parseError.error != QJsonParseError::NoError ||
            !isValidBuildId(remoteBuildId))
        {
            showCheckError(parent,
                QApplication::translate(
                    "UpdaterDialog", "GitHub returned invalid update information."),
                quiet);
            return;
        }

        if (remoteBuildId == localBuildId)
        {
            if (!quiet)
            {
                QMessageBox::information(parent,
                    QApplication::translate("UpdaterDialog", "Seed Atlas Update"),
                    QApplication::translate(
                        "UpdaterDialog", "This Seed Atlas build is up to date."));
            }
            return;
        }

        showUpdatePrompt(parent, config);
    });
}
