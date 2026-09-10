#ifndef UPDATER_H
#define UPDATER_H

class QWidget;
struct Config;

// Checks the manually maintained GitHub release for an available build.
void searchForUpdates(QWidget *parent, Config *config, bool quiet);

#endif // UPDATER_H
