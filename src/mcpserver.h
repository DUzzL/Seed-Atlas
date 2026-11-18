#ifndef MCPSERVER_H
#define MCPSERVER_H

// The MCP server of the app: "seed-atlas --mcp" runs without a window and
// speaks the Model Context Protocol (JSON-RPC 2.0, one message per line on
// stdin and stdout, protocol version 2025-06-18). MCP clients such as Claude
// Desktop or Claude Code start it and call its tools: world queries,
// analyses, seed searches and datapack worlds (see mcptools.h).
//
// Only the messages of the protocol reach stdout: the process moves its
// stdout to stderr before anything else runs, so stray prints of other code
// end up in the log, and writes its messages to a private copy of stdout.

#include "mcpsettings.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QObject>
#include <QThreadPool>

#include <atomic>
#include <memory>
#include <thread>

struct McpCall;
class McpRegistry;

class McpServer : public QObject
{
    Q_OBJECT
public:
    McpServer(const McpSettings& settings, int inFd, int outFd, QObject *parent = nullptr);
    ~McpServer();

    // Starts reading stdin; the application quits when it ends.
    void start();

    const McpSettings& settings() const { return conf; }
    McpRegistry *registry() const { return reg.get(); }

    // Thread safe: writes one message.
    void send(const QJsonObject& message);
    // Thread safe: a progress notification of a call (if it asked for them).
    void progress(McpCall *call, double done, double total, const QString& message);

private:
    void onLine(const QByteArray& line);
    void onEnd();
    void handle(const QJsonObject& msg);
    void reply(const QJsonValue& id, const QJsonObject& result);
    void error(const QJsonValue& id, int code, const QString& message,
        const QJsonValue& data = QJsonValue());
    void callTool(const QJsonValue& id, const QJsonObject& params);
    void finishCall(const std::shared_ptr<McpCall>& call, const QJsonObject& result);
    void readLoop();
    void writeRaw(const QByteArray& bytes);

    McpSettings conf;
    int inFd;
    int outFd;
    std::thread reader;
    QMutex writeMutex;
    bool initialized = false;
    bool ending = false;
    QMutex callsMutex;
    QHash<QString, std::shared_ptr<McpCall>> calls;
    QThreadPool pool;
    std::unique_ptr<McpRegistry> reg;
};

// The entry point of "seed-atlas --mcp" (from main()).
int runMcpServer(int argc, char *argv[]);

#endif // MCPSERVER_H
