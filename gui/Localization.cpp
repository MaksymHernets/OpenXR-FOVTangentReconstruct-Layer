#include "Localization.h"
#include "common/ConfigManager.h"
#include <QAbstractButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QTabWidget>
#include <QTableWidget>
#include <QVariant>
#include <QCoreApplication>
#include <QTranslator>
#include <QApplication>
namespace ui {
namespace {
QString currentLanguage = "en", currentLocale = "en_US";
const QJsonObject &catalog() {
    static const auto data = [] {
        QFile file(":/i18n/gui/translations.json");
        if (!file.open(QIODevice::ReadOnly))
            return QJsonObject{};
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return data;
}
class DialogTranslator : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char *, const char *source, const char *, int) const override {
        return catalog().value(QString::fromUtf8(source)).toObject().value(currentLanguage).toString();
    }
};
QList<QWidget *> widgets(QWidget *root) {
    auto list = root->findChildren<QWidget *>();
    list.prepend(root);
    return list;
}
} // namespace
QStringList languages() {
    return {"en", "de", "fr", "es", "zh", "ja", "ru", "hi", "ar", "bn", "ko"};
}
QStringList locales() {
    return {"en_US", "en_GB", "de_DE", "fr_FR", "es_ES", "zh_CN",
            "ja_JP", "ru_RU", "hi_IN", "ar_SA", "bn_BD", "ko_KR"};
}
QString language() {
    return currentLanguage;
}
QString locale() {
    return currentLocale;
}
void setPreferences(const QString &language, const QString &locale) {
    currentLanguage = languages().contains(language) ? language : "en";
    currentLocale = locales().contains(locale) ? locale : "en_US";
    QLocale::setDefault(QLocale(currentLocale));
    // Layout follows the interface language, independently of numeric/date formatting.
    QApplication::setLayoutDirection(currentLanguage == "ar" ? Qt::RightToLeft : Qt::LeftToRight);
}
void loadPreferences() {
    static DialogTranslator translator;
    QCoreApplication::installTranslator(&translator);
    auto j = fov::loadJson(fov::dataDirectory() / L"manager.json");
    auto get = [&](const char *key) {
        return j.contains(key) && j[key].is_string() ? QString::fromStdString(j[key].get<std::string>())
                                                     : QString();
    };
    setPreferences(get("language"), get("locale"));
}
void savePreferences(const QString &language, const QString &locale) {
    if (!languages().contains(language) || !locales().contains(locale))
        throw std::invalid_argument("Unsupported language or locale");
    fov::updateJson(fov::dataDirectory() / L"manager.json", [&](fov::Json &j) {
        j["language"] = language.toStdString();
        j["locale"] = locale.toStdString();
    });
}
QString translate(const char *source) {
    return translate(QString::fromUtf8(source));
}
QString translate(const QString &source) {
    if (currentLanguage == "en")
        return source;
    auto translated = catalog().value(source).toObject().value(currentLanguage).toString();
    return translated.isEmpty() ? source : translated;
}
void capture(QWidget *root) {
    for (auto w : widgets(root)) {
        if (auto label = qobject_cast<QLabel *>(w)) {
            w->setProperty("sourceText", label->text());
            label->setWordWrap(true);
        }
        if (auto button = qobject_cast<QAbstractButton *>(w))
            w->setProperty("sourceText", button->text());
        if (auto group = qobject_cast<QGroupBox *>(w))
            w->setProperty("sourceTitle", group->title());
        if (auto edit = qobject_cast<QLineEdit *>(w))
            w->setProperty("sourcePlaceholder", edit->placeholderText());
        if (auto spin = qobject_cast<QDoubleSpinBox *>(w))
            w->setProperty("sourceSuffix", spin->suffix());
        if (auto combo = qobject_cast<QComboBox *>(w); combo && !combo->property("nativeNames").toBool()) {
            QStringList items;
            for (int i = 0; i < combo->count(); ++i)
                items << combo->itemText(i);
            w->setProperty("sourceItems", items);
        }
        if (auto tabs = qobject_cast<QTabWidget *>(w)) {
            QStringList items;
            for (int i = 0; i < tabs->count(); ++i)
                items << tabs->tabText(i);
            w->setProperty("sourceTabs", items);
        }
        if (auto table = qobject_cast<QTableWidget *>(w)) {
            QStringList items;
            for (int i = 0; i < table->columnCount(); ++i)
                items << table->horizontalHeaderItem(i)->text();
            w->setProperty("sourceHeaders", items);
        }
    }
}
void retranslate(QWidget *root) {
    root->setLayoutDirection(currentLanguage == "ar" ? Qt::RightToLeft : Qt::LeftToRight);
    for (auto w : widgets(root)) {
        w->setLocale(QLocale(currentLocale));
        auto source = w->property("sourceText").toString();
        if (!source.isEmpty()) {
            if (auto label = qobject_cast<QLabel *>(w))
                label->setText(translate(source));
            if (auto button = qobject_cast<QAbstractButton *>(w))
                button->setText(translate(source));
        }
        if (auto group = qobject_cast<QGroupBox *>(w))
            group->setTitle(translate(w->property("sourceTitle").toString()));
        if (auto edit = qobject_cast<QLineEdit *>(w))
            edit->setPlaceholderText(translate(w->property("sourcePlaceholder").toString()));
        if (auto spin = qobject_cast<QDoubleSpinBox *>(w))
            spin->setSuffix(translate(w->property("sourceSuffix").toString()));
        if (auto combo = qobject_cast<QComboBox *>(w); combo && w->property("sourceItems").isValid()) {
            auto items = w->property("sourceItems").toStringList();
            for (int i = 0; i < items.size(); ++i)
                combo->setItemText(i, translate(items[i]));
        }
        if (auto tabs = qobject_cast<QTabWidget *>(w)) {
            auto items = w->property("sourceTabs").toStringList();
            for (int i = 0; i < items.size(); ++i)
                tabs->setTabText(i, translate(items[i]));
        }
        if (auto table = qobject_cast<QTableWidget *>(w)) {
            auto items = w->property("sourceHeaders").toStringList();
            for (int i = 0; i < items.size(); ++i)
                table->horizontalHeaderItem(i)->setText(translate(items[i]));
        }
    }
}
} // namespace ui
