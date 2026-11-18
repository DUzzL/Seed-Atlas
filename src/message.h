#ifndef MESSAGE_H
#define MESSAGE_H

#include <QMessageBox>
#include <QStringList>

int warn(QWidget *parent, const QString& title, const QString& text, const QString& prompt, QMessageBox::StandardButtons buttons);
void warn(QWidget *parent, const QString& title, const QString& text);
void warn(QWidget *parent, const QString& text);
void info(QWidget *parent, const QString& text);

// While an instance exists, the messages of the calling thread are collected
// in it instead of being shown or printed, and prompts get their cautious
// answer (abort, cancel or no). For callers without a window or terminal,
// such as the MCP server.
class MessageCapture
{
public:
    MessageCapture();
    ~MessageCapture();
    MessageCapture(const MessageCapture&) = delete;
    MessageCapture& operator=(const MessageCapture&) = delete;
    QStringList texts;
private:
    MessageCapture *outer;
};

#endif // MESSAGE_H
