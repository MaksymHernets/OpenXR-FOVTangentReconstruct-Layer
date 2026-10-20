#pragma once
#include <QString>
#include <QStringList>
class QWidget;
namespace ui {
QStringList languages();
QStringList locales();
QString language();
QString locale();
void loadPreferences();
void savePreferences(const QString &language, const QString &locale);
void setPreferences(const QString &language, const QString &locale);
QString translate(const char *source);
QString translate(const QString &source);
// Capture English source text once, then reapply it without rebuilding editor widgets.
void capture(QWidget *root);
void retranslate(QWidget *root);
} // namespace ui
