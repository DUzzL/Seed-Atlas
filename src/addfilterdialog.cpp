#include "addfilterdialog.h"

#include "util.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cstring>

AddFilterDialog::AddFilterDialog(QWidget *parent, WorldInfo wi)
    : QDialog(parent)
    , wi(wi)
    , addButton()
{
    setWindowTitle(tr("Add Filter"));
    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *intro = new QLabel(tr(
        "Tick what your world should have and how close it has to be. "
        "Distances are measured from the world center (0, 0); the spawn "
        "point is usually close to it.\n"
        "Each extra filter makes matching seeds rarer, so start with one or two."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QGroupBox *structures = new QGroupBox(tr("Structures"), this);
    QGridLayout *sgrid = new QGridLayout(structures);
    addGoal(sgrid, F_VILLAGE,       -1, tr("Village"), 500);
    addGoal(sgrid, F_PORTAL,        -1, tr("Ruined portal"), 300);
    addGoal(sgrid, F_SHIPWRECK,     -1, tr("Shipwreck"), 500);
    addGoal(sgrid, F_TREASURE,      -1, tr("Buried treasure"), 500);
    addGoal(sgrid, F_OUTPOST,       -1, tr("Pillager outpost"), 1000);
    addGoal(sgrid, F_DESERT,        -1, tr("Desert pyramid"), 1000);
    addGoal(sgrid, F_JUNGLE,        -1, tr("Jungle temple"), 1000);
    addGoal(sgrid, F_HUT,           -1, tr("Swamp hut"), 1000);
    addGoal(sgrid, F_IGLOO,         -1, tr("Igloo"), 1000);
    addGoal(sgrid, F_MONUMENT,      -1, tr("Ocean monument"), 1000);
    addGoal(sgrid, F_TRAILS,        -1, tr("Trail ruins"), 1000);
    addGoal(sgrid, F_CHAMBERS,      -1, tr("Trial chambers"), 1000);
    addGoal(sgrid, F_ANCIENT_CITY,  -1, tr("Ancient city"), 1500);
    addGoal(sgrid, F_STRONGHOLD,    -1, tr("Stronghold"), 1500);
    addGoal(sgrid, F_MANSION,       -1, tr("Woodland mansion"), 5000);
    layout->addWidget(structures);

    QGroupBox *biomes = new QGroupBox(tr("Biomes"), this);
    QGridLayout *bgrid = new QGridLayout(biomes);
    addGoal(bgrid, F_BIOME, mushroom_fields, tr("Mushroom fields"), 1500);
    addGoal(bgrid, F_BIOME, cherry_grove,    tr("Cherry grove"), 1000);
    addGoal(bgrid, F_BIOME, pale_garden,     tr("Pale garden"), 1000);
    addGoal(bgrid, F_BIOME, flower_forest,   tr("Flower forest"), 1000);
    addGoal(bgrid, F_BIOME, jungle,          tr("Jungle"), 1000);
    addGoal(bgrid, F_BIOME, badlands,        tr("Badlands"), 1500);
    addGoal(bgrid, F_BIOME, ice_spikes,      tr("Ice spikes"), 1500);
    layout->addWidget(biomes);

    QLabel *note = new QLabel(tr(
        "The ticked filters are added to the list; then press \"Start search\". "
        "For anything not listed here, use \"Custom filter...\"."), this);
    note->setWordWrap(true);
    layout->addWidget(note);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton *custom = box->addButton(tr("Custom filter..."), QDialogButtonBox::ResetRole);
    custom->setToolTip(tr("Open the full condition editor with all filter types and options"));
    connect(custom, &QPushButton::clicked, this, [this]() { done(Custom); });
    addButton = box->addButton(tr("Add"), QDialogButtonBox::AcceptRole);
    addButton->setDefault(true);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    updateButtons();

    // center on the main window and stay on its screen; the default
    // placement used the size before the layout was done
    adjustSize();
    if (QWidget *top = parent ? parent->window() : nullptr)
    {
        QRect r = rect();
        r.moveCenter(top->geometry().center());
        if (const QScreen *screen = top->screen())
        {
            const QRect avail = screen->availableGeometry();
            r.moveLeft(qBound(avail.left(), r.left(), avail.right() - r.width()));
            r.moveTop(qBound(avail.top(), r.top(), avail.bottom() - r.height()));
        }
        move(r.topLeft());
    }
}

void AddFilterDialog::addGoal(QGridLayout *grid, int type, int biome, const QString& label, int radius)
{
    const FilterInfo& ft = g_filterinfo.list[type];
    bool available = wi.mc >= ft.mcmin && wi.mc <= ft.mcmax;
    if (biome >= 0)
        available = available && isOverworld(wi.mc, biome);
    if (!available)
        return; // not generated in this Minecraft version

    // two goals per row keep the dialog short enough for small screens
    int n = grid->property("goals").toInt();
    grid->setProperty("goals", n + 1);
    int row = n / 2, col = (n % 2) * 4;
    QCheckBox *check = new QCheckBox(label, grid->parentWidget());
    if (biome >= 0)
    {
        QPixmap pix(14, 14);
        pix.fill(QColor(g_biomeColors[biome][0], g_biomeColors[biome][1], g_biomeColors[biome][2]));
        check->setIcon(QIcon(pix));
    }
    else if (ft.icon)
    {
        check->setIcon(QIcon(getPix(ft.icon, 20)));
    }
    QLabel *within = new QLabel(tr("within"), grid->parentWidget());
    QSpinBox *spin = new QSpinBox(grid->parentWidget());
    spin->setRange(16, 30000);
    spin->setSingleStep(100);
    spin->setValue(radius);
    spin->setSuffix(tr(" blocks"));
    spin->setEnabled(false);
    connect(check, &QCheckBox::toggled, spin, &QSpinBox::setEnabled);
    connect(check, &QCheckBox::toggled, this, &AddFilterDialog::updateButtons);
    grid->addWidget(check, row, col + 0);
    grid->addWidget(within, row, col + 1);
    grid->addWidget(spin, row, col + 2);
    grid->setColumnStretch(col, 1);
    if (col == 0)
        grid->setColumnMinimumWidth(3, 24); // gap between the two columns
    goals.push_back(Goal{type, biome, label, check, spin});
}

void AddFilterDialog::updateButtons()
{
    bool any = false;
    for (const Goal& g : goals)
        any = any || g.check->isChecked();
    if (addButton)
        addButton->setEnabled(any);
}

std::vector<Condition> AddFilterDialog::getConditions() const
{
    std::vector<Condition> structs, biomes;
    for (const Goal& g : goals)
    {
        if (!g.check->isChecked())
            continue;
        int r = g.spin->value();
        Condition c;
        memset(&c, 0, sizeof(c));
        c.version = Condition::VER_CURRENT;
        c.type = g.type;
        c.relative = 0; // world origin
        c.count = 1;
        QByteArray text = g.label.toUtf8().left(sizeof(c.text) - 1);
        memcpy(c.text, text.constData(), text.size());
        if (g.biome < 0)
        {   // structures are located within a radius
            c.rmax = r + 1;
            structs.push_back(c);
        }
        else
        {   // biomes are checked in a square, sampled at 1:16 for speed
            c.x1 = c.z1 = -r;
            c.x2 = c.z2 = r - 1;
            c.y = 256;
            c.step = 16;
            if (g.biome < 128)
                c.biomeToFind = 1ULL << g.biome;
            else
                c.biomeToFindM = 1ULL << (g.biome - 128);
            c.converage = 0.5f;
            c.confidence = 0.95f;
            biomes.push_back(c);
        }
    }
    // structure positions are tested cheaply on the lower 48 bits first
    std::vector<Condition> conds = structs;
    conds.insert(conds.end(), biomes.begin(), biomes.end());
    for (Condition& c : conds)
        c.save = 0; // the conditions list assigns the next free IDs
    return conds;
}
