#ifndef ABOUTDIALOG_H
#define ABOUTDIALOG_H

#include <QDialog>
#include <QString>

// The version comes from the file VERSION.txt through the build (seed-atlas.pro).
// A negative patch number designates a development version (-1 - N: devN).
#if !defined(SEED_ATLAS_VERS_MAJOR) || !defined(SEED_ATLAS_VERS_MINOR) || !defined(SEED_ATLAS_VERS_PATCH)
#error "Build with seed-atlas.pro, which defines the version from the file VERSION.txt."
#endif
#define VERS_MAJOR (SEED_ATLAS_VERS_MAJOR)
#define VERS_MINOR (SEED_ATLAS_VERS_MINOR)
#define VERS_PATCH (SEED_ATLAS_VERS_PATCH)

// returns +1 if newer, -1 if older  and 0 if equal
inline int cmpVers(int major, int minor, int patch)
{
    int s;
    s = (major > VERS_MAJOR) - (major < VERS_MAJOR);
    if (s) return s;
    s = (minor > VERS_MINOR) - (minor < VERS_MINOR);
    if (s) return s;
    int p0 = VERS_PATCH >= 0 ? 1000+VERS_PATCH : -VERS_PATCH;
    int p1 = patch      >= 0 ? 1000+patch      : -patch;
    s = (p1 > p0) - (p1 < p0);
    return s;
}

inline QString getVersStr()
{
    QString s = QString("%1.%2.").arg(VERS_MAJOR).arg(VERS_MINOR);
    if (VERS_PATCH < 0)
        s += QString("dev%1").arg(-1-VERS_PATCH);
    else
        s += QString("%1").arg(VERS_PATCH);
    return s;
}


namespace Ui {
class AboutDialog;
}

class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);
    ~AboutDialog();

private:
    Ui::AboutDialog *ui;
};

#endif // ABOUTDIALOG_H
