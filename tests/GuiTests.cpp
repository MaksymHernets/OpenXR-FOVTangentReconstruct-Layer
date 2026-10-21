#include "gui/MainWindow.h"
#include "gui/FFRPanel.h"
#include "gui/Localization.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QPlainTextEdit>
#include <QFormLayout>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
QStringList placeholders(const QString &s) {
    QStringList result;
    auto matches = QRegularExpression("%[1-9]").globalMatch(s);
    while (matches.hasNext())
        result << matches.next().captured();
    result.sort();
    return result;
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir data;
        require(data.isValid(), "temporary directory");
        qputenv("FOV_DATA_DIR", data.path().toUtf8());
        auto path = fov::dataDirectory();
        fov::updateJson(path / L"history.json", [](fov::Json &j) {
            j["c:/test.exe"] = {{"applicationName", "Test"}, {"firstLaunch", "2026-10-21T12:34:56Z"}};
        });
        fov::updateJson(path / L"config.json", [](fov::Json &j) {
            j["sentinel"] = 123;
            j["defaults"]["vulkan"] = {{"enabled", false}, {"processing", "passthrough"}};
        });
        MainWindow window;
        {
            FFRPanel panel;
            fov::Json eye = {{"runtimeBaselineFov", {-.8, .8, .8, -.8}}, {"activeFov", {-.8, .8, .8, -.8}}};
            panel.setObserved({{"eyes", {eye, eye}},
                               {"capabilities",
                                {{"hardwareSupported", true},
                                 {"integrationRequired", false},
                                 {"backend", "Automatic D3D12 Tier 2 VRS"}}},
                               {"automaticScene",
                                {{"installed", true},
                                 {"drawsRecorded", 12},
                                 {"drawsExecuted", 5},
                                 {"reason", "Queue completion observed"}}}});
            auto support = panel.findChild<QLabel *>("ffrSupport")->text();
            require(support.contains("Automatic D3D12 scene interception installed") &&
                        support.contains("12 recorded draws; 5 GPU-executed draws"),
                    "UI distinguishes automatic execution evidence from hardware capability");
        }

        auto tabs = window.findChild<QTabWidget *>();
        auto language = window.findChild<QComboBox *>("languageChoice");
        auto locale = window.findChild<QComboBox *>("localeChoice");
        auto apply = window.findChild<QPushButton *>("applySettings");
        require(tabs && tabs->count() == 7 && language && locale && apply, "settings controls");
        require(language->count() == 11 && locale->count() == 12, "supported languages and locales");
        require(ui::language() == "en" && ui::locale() == "en_US", "default preferences");
        auto algorithm = window.findChild<QComboBox *>("edgeAlgorithm");
        require(algorithm != nullptr, "algorithm selector");
        auto edgeForm = qobject_cast<QFormLayout *>(algorithm->parentWidget()->layout());
        require(edgeForm && edgeForm->rowCount() == 9, "algorithm parameter form");
        auto stretch = qobject_cast<QDoubleSpinBox *>(edgeForm->itemAt(3, QFormLayout::FieldRole)->widget());
        auto spin = window.findChild<QDoubleSpinBox *>("horizontalFov");
        spin->setValue(.73);
        stretch->setValue(.23);
        auto value = spin->value();
        auto ffrEnabled = window.findChild<QCheckBox *>("ffrEnabled");
        auto ffrFull = window.findChild<QSpinBox *>("ffrFullMiddle");
        auto ffrReduced = window.findChild<QSpinBox *>("ffrReducedMiddle");
        auto ffrOutside = window.findChild<QLabel *>("ffrReconstruction");
        auto ffrDebugView = window.findChild<QComboBox *>("ffrDebugView");
        auto ffrMode = window.findChild<QLabel *>("ffrMode");
        require(ffrEnabled && ffrFull && ffrReduced && ffrOutside && ffrDebugView && ffrMode,
                "independent FFR panel");
        ffrEnabled->setChecked(true);
        ffrFull->setValue(50);
        ffrReduced->setValue(75);
        ffrDebugView->setCurrentIndex(3);
        std::array<QDoubleSpinBox *, 2> fovSpins{spin, window.findChild<QDoubleSpinBox *>("verticalFov")};
        fovSpins[0]->setValue(1);
        fovSpins[1]->setValue(1);
        require(!ffrOutside->isHidden() && ffrReduced->isHidden(), "full-FOV three-zone controls");
        fovSpins[0]->setValue(.85);
        require(!ffrOutside->isHidden() && !ffrReduced->isHidden(),
                "anisotropic reduction selects two-zone controls");
        fovSpins[0]->setValue(1);
        fovSpins[1]->setValue(.8);
        require(!ffrOutside->isHidden(), "vertical reduction selects two-zone controls");
        fovSpins[1]->setValue(1);
        require(ffrFull->value() == 50 && ffrReduced->value() == 75 && ffrDebugView->currentIndex() == 3,
                "mode switches preserve independent quality");
        fovSpins[0]->setValue(value);
        fovSpins[1]->setValue(.8);
        require(algorithm->currentIndex() == 1, "FFR does not change reconstruction algorithm");
        QFile file(":/i18n/gui/translations.json");
        require(file.open(QIODevice::ReadOnly), "embedded catalog");
        const auto catalog = QJsonDocument::fromJson(file.readAll()).object();
        require(catalog.size() >= 98, "catalog parsed");
        const QStringList localeIds{"en_US", "de_DE", "fr_FR", "es_ES", "zh_CN", "ja_JP",
                                    "ru_RU", "hi_IN", "ar_SA", "bn_BD", "ko_KR"};
        for (int i = 0; i < language->count(); ++i) {
            language->setCurrentIndex(i);
            locale->setCurrentIndex(locale->findData(localeIds[i]));
            apply->click();
            require(ui::language() == language->currentData(), "live language switch");
            require(tabs->tabText(5) == ui::translate("Settings"), "translated tab");
            require(spin->value() == value, "unsaved editor value preserved");
            require(spin->locale().name() == localeIds[i], "editor locale applied");
            for (int m = 0; m < algorithm->count(); ++m) {
                algorithm->setCurrentIndex(m);
                const bool expected[]{true,   true,   m == 4, m == 1 || m == 2, m == 1, m == 1,
                                      m == 2, m == 2, m == 5};
                for (int row = 0; row < edgeForm->rowCount(); ++row) {
                    require(edgeForm->isRowVisible(row) == expected[row], "algorithm-specific rows");
                    auto field = edgeForm->itemAt(row, QFormLayout::FieldRole);
                    if (!field)
                        field = edgeForm->itemAt(row, QFormLayout::SpanningRole);
                    require(field && field->widget()->isHidden() == !expected[row], "parameter visibility");
                    if (auto label = edgeForm->itemAt(row, QFormLayout::LabelRole))
                        require(label->widget()->isHidden() == !expected[row], "parameter label visibility");
                }
            }
            require(stretch->value() == .23, "hidden parameter values survive algorithm switching");
            const auto direction = ui::language() == "ar" ? Qt::RightToLeft : Qt::LeftToRight;
            require(window.layoutDirection() == direction && QApplication::layoutDirection() == direction,
                    "language controls layout direction");
            for (auto pane : window.findChildren<QPlainTextEdit *>())
                require(pane->layoutDirection() == Qt::LeftToRight,
                        "technical diagnostics remain left to right");
            auto date = window.findChild<QTableWidget *>()->item(0, 3)->text();
            require(date == QLocale(localeIds[i])
                                .toString(QDateTime::fromString("2026-10-21T12:34:56Z", Qt::ISODate).toUTC(),
                                          QLocale::ShortFormat),
                    "localized UTC date");
            if (i > 0) {
                for (auto entry = catalog.begin(); entry != catalog.end(); ++entry) {
                    auto translated = entry.value().toObject().value(ui::language()).toString();
                    require(!translated.isEmpty(), "missing translation");
                    require(placeholders(entry.key()) == placeholders(translated),
                            "translation placeholders");
                }
                // Every captured English label must have an entry, except the product name.
                for (auto widget : window.findChildren<QWidget *>()) {
                    for (const auto key :
                         {"sourceText", "sourceTitle", "sourcePlaceholder", "sourceSuffix"}) {
                        auto source = widget->property(key).toString();
                        require(source.isEmpty() || source == "OpenXR FOV Manager" ||
                                    catalog.contains(source),
                                "uncatalogued UI text");
                    }
                    for (const auto key : {"sourceItems", "sourceTabs", "sourceHeaders"}) {
                        for (const auto &source : widget->property(key).toStringList())
                            require(catalog.contains(source), "uncatalogued UI item");
                    }
                }
            }
            ui::setPreferences("en", "en_US");
            ui::loadPreferences();
            require(ui::language() == language->currentData() && ui::locale() == localeIds[i],
                    "preferences survive reload");
        }
        MainWindow reopened;
        require(reopened.findChild<QTabWidget *>()->tabText(5) == ui::translate("Settings"),
                "startup translation");
        require(fov::loadJson(path / L"config.json")["sentinel"] == 123,
                "UI preferences do not modify profiles");
        for (auto button : window.findChildren<QPushButton *>())
            if (button->property("sourceText").toString() == "Save profile")
                button->click();
        auto savedFfr = fov::parseProfile(fov::loadJson(path / L"config.json")["defaults"]).ffr;
        require(savedFfr.enabled && savedFfr.fullMiddleQuality == 50 && savedFfr.reducedMiddleQuality == 75 &&
                    savedFfr.debugView == 3,
                "FFR editor persistence");
        auto savedVulkan = fov::loadJson(path / L"config.json")["defaults"]["vulkan"];
        require(savedVulkan["enabled"] == false && savedVulkan["processing"] == "passthrough",
                "Editing a profile preserves Vulkan-specific policy");
        fov::updateJson(path / L"history.json", [](fov::Json &j) {
            j["c:/second.exe"] = {{"applicationName", "Second"}, {"firstLaunch", "2025-11-30T12:34:56Z"}};
        });
        apply->click();
        auto table = window.findChild<QTableWidget *>();
        table->sortItems(3, Qt::AscendingOrder);
        require(table->item(0, 1)->text() == "c:/second.exe", "localized dates sort chronologically");
        auto saved = fov::loadJson(path / L"manager.json");
        bool rejected = false;
        try {
            ui::savePreferences("invalid", "en_US");
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected && fov::loadJson(path / L"manager.json") == saved,
                "invalid selections cannot replace preferences");
        ui::setPreferences("en", "de_DE");
        require(ui::translate("Settings") == "Settings" && QLocale().decimalPoint() == ",",
                "independent language and locale");
        ui::setPreferences("en", "ar_SA");
        require(QApplication::layoutDirection() == Qt::LeftToRight && ui::locale() == "ar_SA",
                "Arabic formatting does not change English layout");
        ui::setPreferences("ar", "en_US");
        require(QApplication::layoutDirection() == Qt::RightToLeft && ui::locale() == "en_US",
                "Arabic layout does not require Arabic formatting");
        fov::updateJson(path / L"manager.json",
                        [](fov::Json &j) { j = {{"language", 42}, {"locale", "invalid"}}; });
        ui::loadPreferences();
        require(ui::language() == "en" && ui::locale() == "en_US", "invalid preference fallback");
        QFile corrupt(QString::fromStdWString((path / L"manager.json").wstring()));
        require(corrupt.open(QIODevice::WriteOnly), "open corrupt preference fixture");
        corrupt.write("{");
        corrupt.close();
        ui::loadPreferences();
        require(ui::language() == "en" && ui::locale() == "en_US", "malformed preference fallback");
        std::cout << "GUI localization and preference tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
