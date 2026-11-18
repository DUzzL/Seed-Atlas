#include "mcpserver.h"

#include "mcptools.h"
#include "aboutdialog.h"
#include "config.h"
#include "gpufilter.h"
#include "datapackworld.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSettings>
#include <QThreadPool>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define mcp_dup _dup
#define mcp_dup2 _dup2
#define mcp_read _read
#define mcp_write _write
#define mcp_fileno _fileno
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#define mcp_dup dup
#define mcp_dup2 dup2
#define mcp_read read
#define mcp_write write
#define mcp_fileno fileno
#endif

namespace {

const char *const PROTOCOL_VERSION = "2025-06-18";
// The versions this server speaks; the answer to any other is
// PROTOCOL_VERSION (the client then decides whether it can go on). Earlier
// versions are not offered: they had batches, which 2025-06-18 removed.
const char *const KNOWN_VERSIONS[] = { "2025-06-18" };

// JSON-RPC error codes
enum
{
    PARSE_ERROR = -32700,
    INVALID_REQUEST = -32600,
    METHOD_NOT_FOUND = -32601,
    INVALID_PARAMS = -32602,
    INTERNAL_ERROR = -32603,
};

// the longest message the server reads (longer lines are refused)
constexpr size_t MAX_LINE = size_t(16) << 20;
// tool calls that run at the same time (more wait in the queue)
constexpr int MAX_CONCURRENT_CALLS = 4;

QString idKey(const QJsonValue& id)
{
    if (id.isString())
        return "s:" + id.toString();
    return "n:" + QString::number(id.toDouble(), 'g', 17);
}

bool validId(const QJsonValue& id)
{
    if (id.isString())
        return true;
    if (!id.isDouble())
        return false;
    const double d = id.toDouble();
    return std::isfinite(d) && d == std::floor(d);
}

void logLine(const QString& text)
{
    const QByteArray bytes = text.toUtf8() + '\n';
    fwrite(bytes.constData(), 1, size_t(bytes.size()), stderr);
    fflush(stderr);
}

void messageToStderr(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    logLine(QStringLiteral("seed-atlas: ") + msg);
}

} // namespace

void McpCall::progress(double done, double total, const QString& message)
{
    if (server)
        server->progress(this, done, total, message);
}

McpServer::McpServer(const McpSettings& settings, int inFd, int outFd, QObject *parent)
    : QObject(parent)
    , conf(settings)
    , inFd(inFd)
    , outFd(outFd)
{
    pool.setMaxThreadCount(MAX_CONCURRENT_CALLS);
    // The datapack generator limits the nesting of what it compiles; the
    // default stack of secondary threads (512 KiB on macOS) is too small for
    // it in some builds, as for the app's datapack loader.
    pool.setStackSize(8u << 20);
    pool.setExpiryTimeout(-1);
    reg.reset(new McpRegistry(this));
}

McpServer::~McpServer()
{
    ending = true;
    {
        QMutexLocker lock(&callsMutex);
        for (const std::shared_ptr<McpCall>& call : std::as_const(calls))
            call->cancel = true;
    }
    reg->shutdown();
    pool.waitForDone();
    if (reader.joinable())
        reader.detach(); // blocked in read(); the process ends
}

void McpServer::start()
{
    reader = std::thread([this]() { readLoop(); });
}

void McpServer::readLoop()
{
    QByteArray buffer;
    bool skipping = false; // inside a line that is too long
    char chunk[65536];
    while (true)
    {
        const auto n = mcp_read(inFd, chunk, sizeof(chunk));
        if (n <= 0)
            break;
        int start = 0;
        for (int i = 0; i < int(n); i++)
        {
            if (chunk[i] != '\n')
                continue;
            if (!skipping)
            {
                buffer.append(chunk + start, i - start);
                QByteArray line = buffer;
                QMetaObject::invokeMethod(this, [this, line]() { onLine(line); },
                    Qt::QueuedConnection);
            }
            buffer.clear();
            skipping = false;
            start = i + 1;
        }
        if (!skipping)
        {
            buffer.append(chunk + start, int(n) - start);
            if (size_t(buffer.size()) > MAX_LINE)
            {   // answered as a parse error once the line ends
                buffer.clear();
                skipping = true;
                QMetaObject::invokeMethod(this, [this]() {
                    error(QJsonValue(), PARSE_ERROR, "Message too long");
                }, Qt::QueuedConnection);
            }
        }
    }
    if (!buffer.trimmed().isEmpty() && !skipping)
    {
        QByteArray line = buffer;
        QMetaObject::invokeMethod(this, [this, line]() { onLine(line); }, Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(this, [this]() { onEnd(); }, Qt::QueuedConnection);
}

void McpServer::onEnd()
{
    // the client closed stdin: stop everything and quit
    logLine("seed-atlas: MCP client disconnected, stopping");
    ending = true;
    {
        QMutexLocker lock(&callsMutex);
        for (const std::shared_ptr<McpCall>& call : std::as_const(calls))
            call->cancel = true;
    }
    reg->shutdown();
    pool.waitForDone();
    QCoreApplication::quit();
}

void McpServer::writeRaw(const QByteArray& bytes)
{
    const char *p = bytes.constData();
    qint64 left = bytes.size();
    while (left > 0)
    {
        const auto n = mcp_write(outFd, p, (unsigned) std::min<qint64>(left, 1 << 20));
        if (n <= 0)
            return; // the client is gone; stdin ends soon as well
        p += n;
        left -= n;
    }
}

void McpServer::send(const QJsonObject& message)
{
    // (compact JSON escapes the line breaks inside strings)
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    QMutexLocker lock(&writeMutex);
    writeRaw(bytes);
}

void McpServer::progress(McpCall *call, double done, double total, const QString& message)
{
    if (!call || call->progressToken.isNull() || call->progressToken.isUndefined())
        return;
    QMutexLocker lock(&writeMutex);
    if (call->done || call->cancel)
        return;
    if (!(done > call->lastProgress))
        return; // the values must increase
    const qint64 now = call->clock.elapsed();
    if (now - call->lastProgressMs < 250 && !(total > 0 && done >= total))
        return;
    call->lastProgress = done;
    call->lastProgressMs = now;
    QJsonObject params;
    params["progressToken"] = call->progressToken;
    params["progress"] = done;
    if (total > 0)
        params["total"] = total;
    if (!message.isEmpty())
        params["message"] = message;
    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["method"] = "notifications/progress";
    msg["params"] = params;
    writeRaw(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');
}

void McpServer::reply(const QJsonValue& id, const QJsonObject& result)
{
    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = id;
    msg["result"] = result;
    send(msg);
}

void McpServer::error(const QJsonValue& id, int code, const QString& message,
    const QJsonValue& data)
{
    QJsonObject err;
    err["code"] = code;
    err["message"] = message;
    if (!data.isUndefined() && !data.isNull())
        err["data"] = data;
    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    // (null where the id could not be read, as JSON-RPC asks)
    msg["id"] = id.isUndefined() ? QJsonValue() : id;
    msg["error"] = err;
    send(msg);
}

void McpServer::onLine(const QByteArray& line)
{
    if (line.trimmed().isEmpty())
        return;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
    if (perr.error != QJsonParseError::NoError)
    {
        error(QJsonValue(), PARSE_ERROR, "Parse error: " + perr.errorString());
        return;
    }
    if (doc.isArray())
    {   // batches were removed from the protocol in 2025-06-18
        error(QJsonValue(), INVALID_REQUEST, "Batches are not supported");
        return;
    }
    if (!doc.isObject())
    {
        error(QJsonValue(), INVALID_REQUEST, "A message must be a JSON object");
        return;
    }
    handle(doc.object());
}

void McpServer::handle(const QJsonObject& msg)
{
    const QJsonValue id = msg.value("id");
    const bool hasId = msg.contains("id");
    if (msg.value("jsonrpc").toString() != QLatin1String("2.0"))
    {
        if (hasId)
            error(validId(id) ? id : QJsonValue(), INVALID_REQUEST, "jsonrpc must be \"2.0\"");
        return;
    }
    if (!msg.contains("method"))
        return; // a response of the client (the server sends no requests)
    if (!msg.value("method").isString())
    {
        error(validId(id) ? id : QJsonValue(), INVALID_REQUEST, "method must be a string");
        return;
    }
    const QString method = msg.value("method").toString();
    const QJsonValue paramsValue = msg.value("params");
    if (!paramsValue.isUndefined() && !paramsValue.isObject())
    {
        if (hasId)
            error(validId(id) ? id : QJsonValue(), INVALID_PARAMS, "params must be an object");
        return;
    }
    const QJsonObject params = paramsValue.toObject();

    if (!hasId)
    {   // notifications get no answer
        if (method == QLatin1String("notifications/initialized"))
            initialized = true;
        else if (method == QLatin1String("notifications/cancelled"))
        {
            const QJsonValue rid = params.value("requestId");
            if (!validId(rid))
                return;
            QMutexLocker lock(&callsMutex);
            auto it = calls.find(idKey(rid));
            if (it != calls.end())
                it.value()->cancel = true;
        }
        return;
    }
    if (!validId(id))
    {
        error(QJsonValue(), INVALID_REQUEST, "id must be a string or an integer");
        return;
    }

    if (method == QLatin1String("initialize"))
    {
        const QString asked = params.value("protocolVersion").toString();
        QString version = PROTOCOL_VERSION;
        for (const char *known : KNOWN_VERSIONS)
            if (asked == QLatin1String(known))
                version = asked;
        QJsonObject tools;
        tools["listChanged"] = false;
        QJsonObject caps;
        caps["tools"] = tools;
        QJsonObject info;
        info["name"] = "seed-atlas";
        info["title"] = "Seed Atlas";
        info["version"] = getVersStr();
        QJsonObject result;
        result["protocolVersion"] = version;
        result["capabilities"] = caps;
        result["serverInfo"] = info;
        result["instructions"] = QString(
            "Seed Atlas computes Minecraft worlds from their seed: biomes, heights, structures, "
            "spawn, strongholds and End portal eyes, statistics of areas, and seed searches. "
            "Seeds are passed and returned as decimal strings (64-bit values do not fit a JSON "
            "number). Coordinates are block coordinates. Versions are given like \"1.21.1\" "
            "(see server_info). Searches run in the background: start_search, then "
            "search_status / search_results, stop_search. Datapack worlds: load_datapacks gives "
            "a world handle for the other tools. Clients may %1.")
            .arg(conf.rights == McpSettings::WRITE_FILES
                ? "also write files with export_search"
                : "not write files (the app's MCP settings allow it)");
        reply(id, result);
        return;
    }
    if (method == QLatin1String("ping"))
    {
        reply(id, QJsonObject());
        return;
    }
    if (method == QLatin1String("tools/list"))
    {
        QJsonObject result;
        result["tools"] = reg->toolList();
        reply(id, result);
        return;
    }
    if (method == QLatin1String("tools/call"))
    {
        callTool(id, params);
        return;
    }
    error(id, METHOD_NOT_FOUND, "Method not found: " + method);
}

void McpServer::callTool(const QJsonValue& id, const QJsonObject& params)
{
    if (!params.value("name").isString())
    {
        error(id, INVALID_PARAMS, "tools/call needs the name of a tool");
        return;
    }
    const QString name = params.value("name").toString();
    if (!reg->hasTool(name))
    {
        error(id, INVALID_PARAMS, "Unknown tool: " + name);
        return;
    }
    const QJsonValue argsValue = params.value("arguments");
    if (!argsValue.isUndefined() && !argsValue.isNull() && !argsValue.isObject())
    {
        error(id, INVALID_PARAMS, "arguments must be an object");
        return;
    }
    if (ending)
    {
        error(id, INTERNAL_ERROR, "The server is shutting down");
        return;
    }
    std::shared_ptr<McpCall> call = std::make_shared<McpCall>();
    call->id = id;
    call->server = this;
    call->clock.start();
    const QJsonValue token = params.value("_meta").toObject().value("progressToken");
    if (token.isString() || validId(token))
        call->progressToken = token;
    {
        QMutexLocker lock(&callsMutex);
        const QString key = idKey(id);
        if (calls.contains(key))
        {
            lock.unlock();
            error(id, INVALID_REQUEST, "A request with this id is still running");
            return;
        }
        calls.insert(key, call);
    }
    const QJsonObject args = argsValue.toObject();
    pool.start([this, call, name, args]() {
        QJsonObject result;
        if (call->cancel)
            result = mcp::errorResult("Cancelled");
        else
            result = reg->call(name, args, call.get());
        // the thread's datapack sampler holds the world; drop it
        releaseDatapackSampler();
        finishCall(call, result);
    });
}

void McpServer::finishCall(const std::shared_ptr<McpCall>& call, const QJsonObject& result)
{
    {
        QMutexLocker lock(&writeMutex);
        call->done = true;
        // a cancelled request gets no response
        if (!call->cancel)
        {
            QJsonObject msg;
            msg["jsonrpc"] = "2.0";
            msg["id"] = call->id;
            msg["result"] = result;
            writeRaw(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');
        }
    }
    QMutexLocker lock(&callsMutex);
    calls.remove(idKey(call->id));
}

int runMcpServer(int argc, char *argv[])
{
    // Keep the protocol's stdout for the protocol: the messages go to a
    // private copy of it, and file descriptor 1 becomes stderr, so that
    // whatever else prints to stdout (messages of other code, libraries)
    // ends up in the log. stdin is kept for the reader alone the same way.
    fflush(stdout);
    const int outFd = mcp_dup(mcp_fileno(stdout));
    const int inFd = mcp_dup(mcp_fileno(stdin));
    if (outFd < 0 || inFd < 0)
    {
        fprintf(stderr, "seed-atlas: cannot set up the standard streams for MCP\n");
        return 1;
    }
    mcp_dup2(mcp_fileno(stderr), mcp_fileno(stdout));
#if defined(_WIN32)
    _setmode(outFd, _O_BINARY);
    _setmode(inFd, _O_BINARY);
    {
        const int nul = _open("NUL", _O_RDONLY);
        if (nul >= 0)
        {
            _dup2(nul, _fileno(stdin));
            _close(nul);
        }
    }
#else
    // a client that goes away must not kill the process mid-write
    signal(SIGPIPE, SIG_IGN);
    {
        const int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
        {
            dup2(nul, STDIN_FILENO);
            close(nul);
        }
    }
#endif
    qInstallMessageHandler(messageToStderr);

    QCoreApplication app(argc, argv);

    // The settings are only read. (QSettings writes back only values that
    // were set, and nothing here sets any.)
    McpSettings mcpSettings;
    mcpSettings.loadReadOnly();
    {
        QSettings settings(appSettingsId(), appSettingsId());
        g_extgen.load(settings);
        publishExtGen();
        setConfiguredCpuUsagePercent(settings.value("config/cpuUsagePercent", 80).toInt());
        GpuClimateFilter::setAllowed(
            settings.value("config/useGpu", GpuClimateFilter::allowed()).toBool());
    }
    QThreadPool::globalInstance()->setMaxThreadCount(configuredThreadCount());

    logLine(QString("seed-atlas: MCP server ready (protocol %1, rights: %2, search threads %3, "
        "longest search %4 min)").arg(PROTOCOL_VERSION)
        .arg(mcpSettings.rights == McpSettings::WRITE_FILES ? "read, compute, write files"
            : "read and compute")
        .arg(mcpSettings.searchThreads).arg(mcpSettings.searchMinutes));

    McpServer server(mcpSettings, inFd, outFd);
    server.start();
    return app.exec();
}
