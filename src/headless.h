#ifndef HEADLESS_H
#define HEADLESS_H

#include "searchthread.h"
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include <QFile>

#include <unordered_set>

class Headless : public QThread
{
    Q_OBJECT

public:
    Headless(QString sessionpath, QString resultspath, bool reset, QObject *parent = 0);
    virtual ~Headless();

    bool loadSession(QString sessionpath, bool reset);

public slots:
    void run();
    void searchResults(QVector<uint64_t> seeds);
    void searchFinish(bool done);
    void progressTimeout();

signals:
    void finished();

public:
    SearchMaster sthread;
    QString sessionpath;
    Session session;
    std::vector<uint64_t> results;
    // the results so far: a resumed search finds the seeds again that
    // were reported after its resume position
    std::unordered_set<uint64_t> resultset;
    QFile resultfile;
    QTextStream resultstream;
    FILE *progressfp;
    QTimer timer;
    QElapsedTimer elapsed;
};

#endif // HEADLESS_H
