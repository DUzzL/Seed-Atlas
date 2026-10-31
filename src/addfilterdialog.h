#ifndef ADDFILTERDIALOG_H
#define ADDFILTERDIALOG_H

#include "config.h"
#include "search.h"

#include <QDialog>
#include <vector>

class QCheckBox;
class QSpinBox;

// Adds filters from a plain list: tick what the world should have near the
// world center. "Custom filter..." leads to the full condition editor.
class AddFilterDialog : public QDialog
{
    Q_OBJECT

public:
    enum { Custom = 2 }; // exec() result: open the condition editor instead

    AddFilterDialog(QWidget *parent, WorldInfo wi);

    // Conditions for the ticked goals; cheap structure checks come first.
    std::vector<Condition> getConditions() const;

private:
    struct Goal
    {
        int type;       // filter type (F_VILLAGE, F_BIOME, ...)
        int biome;      // biome id for F_BIOME, otherwise -1
        QString label;
        QCheckBox *check;
        QSpinBox *spin;
    };

    void addGoal(class QGridLayout *grid, int type, int biome, const QString& label, int radius);
    void updateButtons();

    WorldInfo wi;
    std::vector<Goal> goals;
    class QPushButton *addButton;
};

#endif // ADDFILTERDIALOG_H
