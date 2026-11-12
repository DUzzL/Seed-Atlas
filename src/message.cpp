#include "message.h"

#include <QApplication>
#include <QPointer>
#include <QStyle>
#include <QTextStream>

#include <stdio.h>
#if defined(_WIN32)
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif


static
int term_prompt(const QString& title, const QString& text, QMessageBox::StandardButtons buttons)
{
    // translations for QGnomeTheme as they may override QPlatformThemes
    QApplication::translate("QGnomeTheme", "&OK");
    QApplication::translate("QGnomeTheme", "&Save");
    QApplication::translate("QGnomeTheme", "&Cancel");
    QApplication::translate("QGnomeTheme", "&Close");
    QApplication::translate("QGnomeTheme", "Close without Saving");

    QTextStream out (stdout);
    out << QString("[%1]\n%2\n---\n").arg(title).arg(text);
    std::vector<std::pair<QString, int>> opts;
    if (buttons & QMessageBox::Ok)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "OK"), QMessageBox::Ok));
    if (buttons & QMessageBox::Yes)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Yes"), QMessageBox::Yes));
    if (buttons & QMessageBox::No)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "No"), QMessageBox::No));
    if (buttons & QMessageBox::Abort)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Abort"), QMessageBox::Abort));
    if (buttons & QMessageBox::Cancel)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Cancel"), QMessageBox::Cancel));
    if (buttons & QMessageBox::Ignore)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Ignore"), QMessageBox::Ignore));
    if (buttons & QMessageBox::Reset)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Reset"), QMessageBox::Reset));
    if (buttons & QMessageBox::RestoreDefaults)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Restore Defaults"), QMessageBox::RestoreDefaults));
    if (buttons & QMessageBox::Save)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Save"), QMessageBox::Save));
    if (buttons & QMessageBox::Discard)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Discard"), QMessageBox::Discard));
    if (buttons & QMessageBox::Apply)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Apply"), QMessageBox::Apply));
    if (buttons & QMessageBox::Close)
        opts.push_back(std::make_pair(QApplication::translate("QPlatformTheme", "Close"), QMessageBox::Close));
    if (opts.size() <= 1)
        return buttons;
    // Without an answer (no terminal, or the input ends) the prompt takes
    // the cautious option: abort, cancel or no where there is one.
    int fallback = opts[0].second;
    for (int b : { QMessageBox::Abort, QMessageBox::Cancel, QMessageBox::No, QMessageBox::Close })
    {
        if (buttons & b)
        {
            fallback = b;
            break;
        }
    }
    for (size_t i = 0; i < opts.size(); i++)
        out << QString("%1%2 [%3]").arg(i==0 ? "" : ", ").arg(opts[i].first).arg(i+1);
    out << ":";
    out.flush();
    if (!isatty(fileno(stdin)))
    {
        out << " (no terminal input)\n";
        out.flush();
        return fallback;
    }
    QTextStream in (stdin);
    uint num = 0;
    while (true)
    {
        QString line;
        if (!in.readLineInto(&line))
        {   // end of input
            out << "\n";
            out.flush();
            return fallback;
        }
        num = line.toUInt();
        if (num >= 1 && num <= opts.size())
            break;
        out << "Invalid option, try again: ";
        out.flush();
    }
    return opts[num-1].second;
}

// Same icon QMessageBox::setIcon() would show, without the system sound.
static QPixmap messageIcon(QMessageBox *box, QMessageBox::Icon icon)
{
    QStyle::StandardPixmap sp;
    switch (icon)
    {
    case QMessageBox::Information:  sp = QStyle::SP_MessageBoxInformation; break;
    case QMessageBox::Warning:      sp = QStyle::SP_MessageBoxWarning; break;
    case QMessageBox::Critical:     sp = QStyle::SP_MessageBoxCritical; break;
    case QMessageBox::Question:     sp = QStyle::SP_MessageBoxQuestion; break;
    default:                        return QPixmap();
    }
    QStyle *style = box->style();
    int size = style->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, box);
    return style->standardIcon(sp, nullptr, box).pixmap(QSize(size, size), box->devicePixelRatio());
}

static
int message(
    QWidget *parent, QMessageBox::Icon icon, const QString& title, const QString& text,
    const QString& prompt, QMessageBox::StandardButtons buttons)
{
    QString s = text;
    if (!prompt.isEmpty())
        s += "\n\n" + prompt;
    if (!parent)
        return term_prompt(title, s, buttons);

    // emulate the behaviour of QMessageBox::warning(...), but raise as active window
    QMessageBox *w = new QMessageBox(parent);
    w->setWindowTitle(title);
    w->setIconPixmap(messageIcon(w, icon)); // setIcon() plays a sound on windows
    // raise the parent, but keep it maximized or full screen
    parent->setWindowState((parent->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
#if WASM
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->setText(text);
    w->addButton(QMessageBox::Ok);
    w->show();
    return QMessageBox::Cancel;
#else
    w->setText(s);
    for (uint mask = QMessageBox::FirstButton; mask <= QMessageBox::LastButton; mask <<= 1)
        if (mask & buttons)
            w->addButton((QMessageBox::StandardButton)mask);
    QPointer<QMessageBox> guard(w); // the parent may go away during exec()
    const int result = w->exec();
    if (!guard)
        return QMessageBox::Cancel;
    const int button = result == -1 ? int(QMessageBox::Cancel) : int(w->standardButton(w->clickedButton()));
    delete w;
    return button;
#endif
}

int warn(QWidget *parent, const QString& title, const QString& text, const QString& prompt, QMessageBox::StandardButtons buttons)
{
    return message(parent, QMessageBox::Warning, title, text, prompt, buttons);
}

void warn(QWidget *parent, const QString& title, const QString& text)
{
    message(parent, QMessageBox::Warning, title, text, "", QMessageBox::Ok);
}

void warn(QWidget *parent, const QString& text)
{
    message(parent, QMessageBox::Warning, QApplication::tr("Warning"), text, "", QMessageBox::Ok);
}

void info(QWidget *parent, const QString& text)
{
    message(parent, QMessageBox::Information, QApplication::tr("Information"), text, "", QMessageBox::Ok);
}
