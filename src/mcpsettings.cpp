#include "mcpsettings.h"

#include "config.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

int McpSettings::maxSearchThreads()
{
    return std::max(1, QThread::idealThreadCount());
}

int McpSettings::defaultSearchThreads()
{
    return std::max(1, QThread::idealThreadCount() / 2);
}

void McpSettings::reset()
{
    rights = READ_AND_COMPUTE;
    searchThreads = defaultSearchThreads();
    searchMinutes = 10;
}

void McpSettings::load(QSettings& settings)
{
    reset();
    const int r = settings.value("mcp/rights", rights).toInt();
    rights = r == WRITE_FILES ? WRITE_FILES : READ_AND_COMPUTE;
    searchThreads = std::clamp(settings.value("mcp/searchThreads", searchThreads).toInt(),
        1, maxSearchThreads());
    searchMinutes = std::clamp(settings.value("mcp/searchMinutes", searchMinutes).toInt(),
        1, MAX_SEARCH_MINUTES);
}

void McpSettings::save(QSettings& settings) const
{
    settings.setValue("mcp/rights", rights);
    settings.setValue("mcp/searchThreads", searchThreads);
    settings.setValue("mcp/searchMinutes", searchMinutes);
}

void McpSettings::loadReadOnly()
{
    const bool test = !qEnvironmentVariableIsEmpty("SEED_ATLAS_TEST_SETTINGS_DIR")
        || !qEnvironmentVariableIsEmpty("SEED_ATLAS_TEST_SETTINGS_ID");
    const QString testFile = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_MCP_SETTINGS"));
    if (test && !testFile.isEmpty())
    {
        QSettings ini(testFile, QSettings::IniFormat);
        load(ini);
        return;
    }
    // (QSettings only writes back what was changed: reading leaves the
    // stored settings alone)
    QSettings settings(appSettingsId(), appSettingsId());
    load(settings);
}

QString mcpCommandLine()
{
    QString path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (path.contains(' '))
        path = '"' + path + '"';
    return path + " --mcp";
}

QString mcpConfigSnippet()
{
    QJsonObject server;
    server["command"] = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    server["args"] = QJsonArray{ "--mcp" };
    QJsonObject servers;
    servers["seed-atlas"] = server;
    QJsonObject root;
    root["mcpServers"] = servers;
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QWidget *McpSettingsPage::create(QWidget *parent)
{
    QWidget *page = new QWidget(parent);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    QGroupBox *access = new QGroupBox(QCoreApplication::translate("McpSettings",
        "MCP server"), page);
    QFormLayout *form = new QFormLayout(access);
    rights = new QComboBox(access);
    rights->setObjectName("mcpRights");
    rights->addItem(QCoreApplication::translate("McpSettings", "Read and compute"),
        int(McpSettings::READ_AND_COMPUTE));
    rights->addItem(QCoreApplication::translate("McpSettings", "Also write files"),
        int(McpSettings::WRITE_FILES));
    rights->setToolTip(QCoreApplication::translate("McpSettings",
        "Read and compute: clients can query worlds, run analyses and searches. They never "
        "write files and never change the settings of the app.\n"
        "Also write files: clients can also export search results to files they name."));
    form->addRow(QCoreApplication::translate("McpSettings", "Clients may:"), rights);
    threads = new QSpinBox(access);
    threads->setObjectName("mcpThreads");
    threads->setRange(1, McpSettings::maxSearchThreads());
    threads->setToolTip(QCoreApplication::translate("McpSettings",
        "The threads that all searches of a client may use together."));
    form->addRow(QCoreApplication::translate("McpSettings", "Search threads:"), threads);
    minutes = new QSpinBox(access);
    minutes->setObjectName("mcpMinutes");
    minutes->setRange(1, McpSettings::MAX_SEARCH_MINUTES);
    minutes->setSuffix(QCoreApplication::translate("McpSettings", " min"));
    minutes->setToolTip(QCoreApplication::translate("McpSettings",
        "A search that a client started stops after this time."));
    form->addRow(QCoreApplication::translate("McpSettings", "Longest search:"), minutes);
    layout->addWidget(access);

    QGroupBox *connect = new QGroupBox(QCoreApplication::translate("McpSettings",
        "Connect a client"), page);
    QVBoxLayout *connectLayout = new QVBoxLayout(connect);
    QLabel *how = new QLabel(connect);
    how->setWordWrap(true);
    how->setTextInteractionFlags(Qt::TextSelectableByMouse);
    how->setText(QCoreApplication::translate("McpSettings",
        "MCP clients such as Claude Desktop or Claude Code start Seed Atlas without a window "
        "with this command and talk to it over its standard input and output:\n\n%1\n\n"
        "Claude Code: claude mcp add seed-atlas -- %1\n"
        "Claude Desktop: add the copied entry to \"mcpServers\" in "
        "claude_desktop_config.json. The server reads these settings when it starts.")
        .arg(mcpCommandLine()));
    connectLayout->addWidget(how);
    QPushButton *copy = new QPushButton(QCoreApplication::translate("McpSettings",
        "Copy client configuration"), connect);
    copy->setObjectName("mcpCopyConfig");
    copy->setToolTip(QCoreApplication::translate("McpSettings",
        "Copies a configuration entry with the path of this program for Claude Desktop "
        "(claude_desktop_config.json) or a project's .mcp.json of Claude Code."));
    QObject::connect(copy, &QPushButton::clicked, copy, [copy]() {
        if (QClipboard *clipboard = QApplication::clipboard())
            clipboard->setText(mcpConfigSnippet());
        const QString text = copy->text();
        copy->setText(QCoreApplication::translate("McpSettings", "Copied"));
        QTimer::singleShot(1500, copy, [copy, text]() { copy->setText(text); });
    });
    QHBoxLayout *row = new QHBoxLayout;
    row->addWidget(copy);
    row->addStretch();
    connectLayout->addLayout(row);
    layout->addWidget(connect);
    return page;
}

void McpSettingsPage::show(const McpSettings& s)
{
    rights->setCurrentIndex(std::max(0, rights->findData(s.rights)));
    threads->setValue(s.searchThreads);
    minutes->setValue(s.searchMinutes);
}

McpSettings McpSettingsPage::value() const
{
    McpSettings s;
    s.rights = rights->currentData().toInt();
    s.searchThreads = threads->value();
    s.searchMinutes = minutes->value();
    return s;
}
