#include "updater.h"

#include <QApplication>
#include <QMessageBox>

void searchForUpdates(bool quiet)
{
    if (!quiet)
    {
        QMessageBox::information(
            nullptr,
            QApplication::translate("UpdaterDialog", "Updates"),
            QApplication::translate(
                "UpdaterDialog",
                "Automatic updates are not configured for this Seed Atlas build."));
    }
}
