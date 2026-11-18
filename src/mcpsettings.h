#ifndef MCPSETTINGS_H
#define MCPSETTINGS_H

// The settings of the MCP server (seed-atlas --mcp): what an MCP client may
// do and how much of the machine its searches may use. The app's settings
// dialog edits them; the --mcp process only reads them, once at its start.

#include <QString>

class QSettings;
class QWidget;
class QComboBox;
class QSpinBox;

struct McpSettings
{
    enum Rights
    {
        READ_AND_COMPUTE = 0,   // queries, analyses, searches; no files written
        WRITE_FILES = 1,        // also export results to paths the client names
    };
    int rights = READ_AND_COMPUTE;
    int searchThreads = 1;      // threads of all running searches together
    int searchMinutes = 10;     // a search stops after this long

    McpSettings() { reset(); }
    void reset();
    void load(QSettings& settings);
    void save(QSettings& settings) const;
    // Reads the app's settings without writing them. Test runs (with their
    // own settings, see appSettingsId()) may name an ini file instead with
    // SEED_ATLAS_TEST_MCP_SETTINGS.
    void loadReadOnly();

    static int defaultSearchThreads();
    static int maxSearchThreads();
    static constexpr int MAX_SEARCH_MINUTES = 24 * 60;
};

// The command line that starts the server, e.g. "/Applications/Seed
// Atlas.app/Contents/MacOS/seed-atlas --mcp", and a JSON snippet for the
// configuration of MCP clients (Claude Desktop's mcpServers entry).
QString mcpCommandLine();
QString mcpConfigSnippet();

// The widgets of the "MCP" page of the settings dialog.
struct McpSettingsPage
{
    QComboBox *rights = nullptr;
    QSpinBox *threads = nullptr;
    QSpinBox *minutes = nullptr;

    // Builds the page's group boxes into @parent and returns the top widget.
    QWidget *create(QWidget *parent);
    void show(const McpSettings& s);
    McpSettings value() const;
};

#endif // MCPSETTINGS_H
