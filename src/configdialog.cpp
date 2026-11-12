#include "configdialog.h"
#include "ui_configdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QPushButton>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

#include <limits>


ConfigDialog::ConfigDialog(QWidget *parent, Config *config)
    : QDialog(parent)
    , ui(new Ui::ConfigDialog)
    , checkGpu(nullptr)
{
    ui->setupUi(this);

#if !WITH_UPDATER
    int miscidx = ui->gridLayout->indexOf(ui->groupMisc);
    if (miscidx >= 0)
    {
        delete ui->gridLayout->takeAt(miscidx);
        ui->groupMisc->hide();
    }
#endif

    ui->lineMatching->setValidator(new QIntValidator(1, 99999999, ui->lineMatching));
    ui->lineGridSpacing->setValidator(new QIntValidator(
        1, std::numeric_limits<int>::max(), ui->lineGridSpacing));
    ui->label_9->setText(tr("CPU usage limit:"));
    ui->spinThreads->setRange(1, 100);
    ui->spinThreads->setSingleStep(5);
    ui->spinThreads->setSuffix(tr(" %"));
    ui->spinThreads->setToolTip(tr("Limits CPU-heavy work by restricting the number of parallel workers. The actual CPU load can vary by task."));
    ui->lineIconScale->setValidator(new QDoubleValidator(1.0/8, 16.0, 3, ui->lineIconScale));

    QSize size = sizeHint();
    int hsa = ui->scrollArea->sizeHint().height();
    int hsc = ui->scrollAreaWidgetContents->sizeHint().height();
    int hpa = parent ? parent->size().height() : size.height();
    int h = size.height();
    int m1, m2;
    layout()->getContentsMargins(0, &m1, 0, &m2);
    h += hsc - hsa + m1 + m2;
    if (h > hpa) h = hpa;
    size.setHeight(h);
    resize(size);

    setupCategories();
    initConfig(config);
    trackChanges();
    const int parentWidth = parent ? parent->width() : 1200;
    resize(qBound(720, parentWidth - 80, 1000), height());
}

ConfigDialog::~ConfigDialog()
{
    delete ui;
}

void ConfigDialog::setupCategories()
{
    QTabWidget *tabs = new QTabWidget(this);
    const auto addCategory = [tabs](const QString& title) {
        QScrollArea *scroll = new QScrollArea(tabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        QWidget *content = new QWidget(scroll);
        QVBoxLayout *layout = new QVBoxLayout(content);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(10);
        layout->addStretch();
        scroll->setWidget(content);
        tabs->addTab(scroll, title);
        return layout;
    };
    const auto moveGroup = [this](QVBoxLayout *layout, QWidget *group) {
        ui->gridLayout->removeWidget(group);
        layout->insertWidget(layout->count()-1, group);
    };

    // Split the former monolithic map section into mapping, visuals and performance.
    QGroupBox *performance = new QGroupBox(tr("Performance"), this);
    QFormLayout *performanceLayout = new QFormLayout(performance);
    ui->gridLayout_7->removeWidget(ui->label_7);
    ui->gridLayout_7->removeWidget(ui->spinCacheSize);
    ui->gridLayout_7->removeWidget(ui->label_9);
    ui->gridLayout_7->removeWidget(ui->spinThreads);
    performanceLayout->addRow(ui->label_7, ui->spinCacheSize);
    performanceLayout->addRow(ui->label_9, ui->spinThreads);
    checkGpu = new QCheckBox(tr("Use the GPU for seed searches"), performance);
    checkGpu->setToolTip(tr("Long searches with climate or biome sample conditions let the "
        "graphics card rule out seeds first, where that is faster than the CPU. The results "
        "are the same either way."));
    performanceLayout->addRow(checkGpu);

    QGroupBox *mapVisuals = new QGroupBox(tr("Map appearance"), this);
    QVBoxLayout *mapVisualsLayout = new QVBoxLayout(mapVisuals);
    ui->gridLayout_7->removeWidget(ui->label_14);
    ui->gridLayout_7->removeWidget(ui->lineIconScale);
    ui->gridLayout_7->removeWidget(ui->checkSmooth);
    ui->gridLayout_7->removeWidget(ui->checkBBoxes);
    QFormLayout *iconLayout = new QFormLayout;
    iconLayout->addRow(ui->label_14, ui->lineIconScale);
    mapVisualsLayout->addLayout(iconLayout);
    mapVisualsLayout->addWidget(ui->checkBBoxes);
    mapVisualsLayout->addWidget(ui->checkSmooth);
    QPushButton *editTools = new QPushButton(tr("Toolbar tools..."), mapVisuals);
    connect(editTools, &QPushButton::clicked, this, &ConfigDialog::openMapTools);
    mapVisualsLayout->addWidget(editTools);

    ui->groupBox_2->setTitle(tr("Grid and map coordinates"));
    QVBoxLayout *application = addCategory(tr("General"));
    moveGroup(application, ui->groupSession);
    moveGroup(application, ui->groupBox);
    moveGroup(application, ui->groupMisc);

    QVBoxLayout *finding = addCategory(tr("Seed Finder"));
    moveGroup(finding, ui->groupSearch);

    QVBoxLayout *mapping = addCategory(tr("Seed Mapping"));
    moveGroup(mapping, ui->groupBox_2);

    QVBoxLayout *perf = addCategory(tr("Performance"));
    perf->insertWidget(perf->count()-1, performance);

    QVBoxLayout *visual = addCategory(tr("Visuals"));
    moveGroup(visual, ui->groupInterface);
    visual->insertWidget(visual->count()-1, mapVisuals);

    ui->gridLayout_8->replaceWidget(ui->scrollArea, tabs);
    ui->scrollArea->deleteLater();
}

// Apply is only enabled while there are changes that it would save.
void ConfigDialog::trackChanges()
{
    const auto changed = [this]() { setModified(true); };
    for (QCheckBox *w : findChildren<QCheckBox*>())
        connect(w, &QCheckBox::toggled, this, changed);
    for (QComboBox *w : findChildren<QComboBox*>())
        connect(w, &QComboBox::currentIndexChanged, this, changed);
    for (QLineEdit *w : findChildren<QLineEdit*>())
        connect(w, &QLineEdit::textEdited, this, changed);
    for (QAbstractSpinBox *w : findChildren<QAbstractSpinBox*>())
    {
        if (QSpinBox *spin = qobject_cast<QSpinBox*>(w))
            connect(spin, &QSpinBox::valueChanged, this, changed);
    }
    setModified(false);
}

void ConfigDialog::setModified(bool modified)
{
    if (QPushButton *apply = ui->buttonBox->button(QDialogButtonBox::Apply))
        apply->setEnabled(modified);
}

void ConfigDialog::initConfig(Config *config)
{
    // Keep the values that have no widget here (e.g. the biome colormap),
    // otherwise Apply would save them as empty.
    if (config != &conf)
        conf = *config;
    ui->checkSmooth->setChecked(config->smoothMotion);
    ui->checkBBoxes->setChecked(config->showBBoxes);
    ui->checkRestore->setChecked(config->restoreSession);
    ui->checkWindowPos->setChecked(config->restoreWindow);
    ui->checkUpdates->setChecked(config->checkForUpdates);
    ui->checkAutosave->setChecked(config->autosaveCycle != 0);
    if (config->autosaveCycle)
        ui->spinAutosave->setValue(config->autosaveCycle);
    ui->comboStyle->setCurrentIndex(config->uistyle);
    ui->lineMatching->setText(QString::number(config->maxMatching));
    ui->lineGridSpacing->setText(config->gridSpacing ? QString::number(config->gridSpacing) : "");
    ui->comboGridMult->setCurrentText(config->gridMultiplier ? QString::number(config->gridMultiplier) : tr("None"));
    ui->spinCacheSize->setValue(config->mapCacheSize);
    ui->spinThreads->setValue(config->cpuUsagePercent);
    checkGpu->setChecked(config->useGpu);
    ui->lineSep->setText(config->separator);
    int idx = config->quote == "\'" ? 1 : config->quote== "\"" ? 2 : 0;
    ui->comboQuote->setCurrentIndex(idx);
    ui->fontComboNorm->setCurrentFont(config->fontNorm);
    ui->fontComboMono->setCurrentFont(config->fontMono);
    ui->spinFontSizeNorm->setValue(config->fontNorm.pointSize());
    ui->spinFontSizeMono->setValue(config->fontMono.pointSize());
    ui->lineIconScale->setText(QString::number(config->iconScale));
}

Config ConfigDialog::getConfig()
{
    // values without a widget here come from what is saved right now, so
    // Apply does not undo a change made outside the dialog meanwhile
    Config saved;
    saved.load();
    conf.biomeColorPath = saved.biomeColorPath;
    conf.advancedSearch = saved.advancedSearch;

    conf.smoothMotion = ui->checkSmooth->isChecked();
    conf.showBBoxes = ui->checkBBoxes->isChecked();
    conf.restoreSession = ui->checkRestore->isChecked();
    conf.restoreWindow = ui->checkWindowPos->isChecked();
    conf.checkForUpdates = ui->checkUpdates->isChecked();
    conf.autosaveCycle = ui->checkAutosave->isChecked() ? ui->spinAutosave->value() : 0;
    conf.uistyle = ui->comboStyle->currentIndex();
    conf.maxMatching = ui->lineMatching->text().toInt();
    conf.gridSpacing = ui->lineGridSpacing->text().toInt();
    conf.gridMultiplier = ui->comboGridMult->currentText().toInt();
    conf.mapCacheSize = ui->spinCacheSize->value();
    conf.cpuUsagePercent = ui->spinThreads->value();
    conf.mapThreads = threadCountForCpuUsage(conf.cpuUsagePercent);
    conf.useGpu = checkGpu->isChecked();
    conf.separator = ui->lineSep->text();
    int idx = ui->comboQuote->currentIndex();
    conf.quote = idx == 1 ? "\'" : idx == 2 ? "\"" : "";

    conf.fontNorm = ui->fontComboNorm->currentFont();
    conf.fontMono = ui->fontComboMono->currentFont();
    conf.fontNorm.setPointSize(ui->spinFontSizeNorm->value());
    conf.fontMono.setPointSize(ui->spinFontSizeMono->value());
    conf.fontNorm.setStyleHint(QFont::AnyStyle);
    conf.fontMono.setStyleHint(QFont::Monospace);

    bool ok = false;
    conf.iconScale = ui->lineIconScale->text().toDouble(&ok);
    if (!ok || conf.iconScale <= 0)
        conf.iconScale = 1.0;

    if (!conf.maxMatching) conf.maxMatching = 65536;

    return conf;
}

void ConfigDialog::onUpdateMapConfig()
{
    emit updateMapConfig();
}

void ConfigDialog::on_buttonBox_clicked(QAbstractButton *button)
{
    int role = ui->buttonBox->buttonRole(button);
    if (role == QDialogButtonBox::ResetRole)
    {
        const QString biomeColorPath = conf.biomeColorPath;
        conf.reset();
        conf.biomeColorPath = biomeColorPath; // edited in its own dialog
        initConfig(&conf);
        setModified(true); // the defaults are not saved until applied
    }
    else if (role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::ApplyRole)
    {
        getConfig().save();
        emit updateConfig();
        setModified(false);
    }
}

void ConfigDialog::on_lineGridSpacing_textChanged(const QString &text)
{
    ui->comboGridMult->setEnabled(!text.isEmpty());
}
