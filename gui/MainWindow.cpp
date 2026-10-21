#include "common/Platform.h"
#include "MainWindow.h"
#include "FFRPanel.h"
#include "Localization.h"
#include "common/RegistryManager.h"
#include <QTabWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QLocalSocket>
#include <QTimer>
#include <QMessageBox>
#include <QColorDialog>
#include <QSignalBlocker>
#include <QDateTime>
#include <QLocale>
#include <QStandardPaths>
#include <algorithm>
#include <QGroupBox>
#include <QScrollArea>
namespace {
using ui::translate;
QDoubleSpinBox *number(double min, double max, double step) {
    auto s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setDecimals(2);
    s->setSingleStep(step);
    return s;
}
QString localizedDate(const std::string &value) {
    auto date = QDateTime::fromString(QString::fromStdString(value), Qt::ISODate);
    return date.isValid() ? QLocale().toString(date.toUTC(), QLocale::ShortFormat)
                          : QString::fromStdString(value);
}
class HistoryDateItem : public QTableWidgetItem {
  public:
    HistoryDateItem(const QString &display, const std::string &isoDate) : QTableWidgetItem(display) {
        setData(Qt::UserRole, QDateTime::fromString(QString::fromStdString(isoDate), Qt::ISODate));
    }
    bool operator<(const QTableWidgetItem &other) const override {
        return data(Qt::UserRole).toDateTime() < other.data(Qt::UserRole).toDateTime();
    }
};
QString text(const std::string &s) {
    return QString::fromUtf8(s);
}
} // namespace
MainWindow::MainWindow() {
    setWindowTitle("OpenXR FOV Manager " FOV_VERSION);
    resize(1080, 760);
    auto central = new QWidget;
    auto outer = new QVBoxLayout(central);
    setCentralWidget(central);
    auto heading = new QLabel("OpenXR FOV Manager");
    heading->setStyleSheet("font-size:24px;font-weight:600;padding:8px");
    outer->addWidget(heading);
    targetLabel = new QLabel;
    outer->addWidget(targetLabel);
    auto tabs = new QTabWidget;
    outer->addWidget(tabs);
    auto dashboard = new QWidget;
    auto d = new QVBoxLayout(dashboard);
    registrationLabel = new QLabel;
    runtimeLabel = new QLabel;
    runtimeLabel->setWordWrap(true);
    liveLabel = new QLabel;
    liveLabel->setWordWrap(true);
    d->addWidget(registrationLabel);
    d->addWidget(runtimeLabel);
    d->addWidget(liveLabel);
    auto controls = new QHBoxLayout;
    for (auto item : {std::pair{"Enable layer", L"--enable"}, std::pair{"Disable layer", L"--disable"},
                      std::pair{"Unregister", L"--uninstall"}}) {
        auto b = new QPushButton(item.first);
        controls->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, action = std::wstring(item.second)]() {
            int r = fov::runRegistrationHelper(action, true);
            if (r)
                QMessageBox::warning(
                    this, translate("Registration"),
                    translate("Registry operation failed (code %1). Administrator approval is required.")
                        .arg(r));
            refresh();
        });
    }
    d->addLayout(controls);
    d->addWidget(new QLabel("Registration changes take effect when the VR application restarts. Windows may "
                            "request administrator approval."));
    d->addStretch();
    tabs->addTab(dashboard, "Dashboard");
    auto gamePage = new QWidget;
    auto g = new QVBoxLayout(gamePage);
    search = new QLineEdit;
    search->setPlaceholderText("Search name, path, OpenXR name, or graphics API");
    g->addWidget(search);
    games = new QTableWidget(0, 8);
    games->setHorizontalHeaderLabels({"Application", "Executable path", "OpenXR name", "First launch (UTC)",
                                      "Last launch (UTC)", "Launches", "Graphics API", "Profile"});
    games->setSelectionBehavior(QAbstractItemView::SelectRows);
    games->setSelectionMode(QAbstractItemView::SingleSelection);
    games->setEditTriggers(QAbstractItemView::NoEditTriggers);
    games->setSortingEnabled(true);
    games->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    g->addWidget(games);
    auto actions = new QHBoxLayout;
    auto edit = new QPushButton("Edit selected profile");
    auto remove = new QPushButton("Remove from history");
    auto defaults = new QPushButton("Edit global defaults");
    actions->addWidget(edit);
    actions->addWidget(remove);
    actions->addWidget(defaults);
    g->addLayout(actions);
    tabs->addTab(gamePage, "Games");
    connect(search, &QLineEdit::textChanged, this, [this]() {
        for (int r = 0; r < games->rowCount(); r++) {
            bool match = false;
            for (int c = 0; c < games->columnCount(); c++)
                match |= games->item(r, c)->text().contains(search->text(), Qt::CaseInsensitive);
            games->setRowHidden(r, !match);
        }
    });
    connect(edit, &QPushButton::clicked, this, [this, tabs]() {
        auto r = games->currentRow();
        if (r < 0)
            return;
        selectedPath = games->item(r, 1)->text();
        loadEditor();
        tabs->setCurrentIndex(2);
    });
    connect(games, &QTableWidget::cellDoubleClicked, this, [edit](int, int) { edit->click(); });
    connect(defaults, &QPushButton::clicked, this, [this]() {
        selectedPath.clear();
        loadEditor();
    });
    connect(remove, &QPushButton::clicked, this, [this]() {
        auto r = games->currentRow();
        if (r < 0)
            return;
        auto p = games->item(r, 1)->text().toStdString();
        try {
            fov::updateJson(fov::dataDirectory() / L"history.json", [&](fov::Json &j) { j.erase(p); });
            refreshGames();
        } catch (const std::exception &e) {
            QMessageBox::warning(this, translate("History"), e.what());
        }
    });
    auto fovPage = new QWidget;
    auto f = new QFormLayout(fovPage);
    profileEnabled = new QCheckBox("Enable processing for this profile");
    f->addRow(profileEnabled);
    horizontal = number(.5, 1, .01);
    vertical = number(.5, 1, .01);
    horizontal->setObjectName("horizontalFov");
    vertical->setObjectName("verticalFov");
    horizontalSlider = new QSlider(Qt::Horizontal);
    verticalSlider = new QSlider(Qt::Horizontal);
    horizontalSlider->setRange(50, 100);
    verticalSlider->setRange(50, 100);
    auto row = [](QSlider *slider, QDoubleSpinBox *spin) {
        auto w = new QWidget;
        auto layout = new QHBoxLayout(w);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(slider);
        layout->addWidget(spin);
        return w;
    };
    f->addRow("Horizontal multiplier", row(horizontalSlider, horizontal));
    f->addRow("Vertical multiplier", row(verticalSlider, vertical));
    linked = new QCheckBox("Link both axes");
    f->addRow(linked);
    connect(horizontalSlider, &QSlider::valueChanged, this,
            [this](int v) { horizontal->setValue(v / 100.); });
    connect(verticalSlider, &QSlider::valueChanged, this, [this](int v) { vertical->setValue(v / 100.); });
    connect(horizontal, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        horizontalSlider->setValue(qRound(v * 100));
        if (linked->isChecked() && !loading)
            vertical->setValue(v);
    });
    connect(vertical, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        verticalSlider->setValue(qRound(v * 100));
        if (linked->isChecked() && !loading)
            horizontal->setValue(v);
    });
    auto reset = new QPushButton("Reset to global defaults");
    f->addRow(reset);
    f->addRow(
        new QLabel("FOV changes apply live. Render dimensions are chosen at application startup.\nRequested "
                   "and submitted FOV values appear on Diagnostics while a game is running."));
    tabs->addTab(fovPage, "FOV Settings");
    connect(reset, &QPushButton::clicked, this, [this]() {
        try {
            fov::updateJson(fov::dataDirectory() / L"config.json", [&](fov::Json &j) {
                if (selectedPath.isEmpty())
                    j["defaults"] = fov::profileJson({});
                else if (j.contains("games") && j["games"].is_object())
                    j["games"].erase(selectedPath.toStdString());
            });
            loadEditor();
            refreshGames();
        } catch (const std::exception &e) {
            QMessageBox::warning(this, translate("Reset"), e.what());
        }
    });
    auto edgePage = new QWidget;
    auto e = new QFormLayout(edgePage);
    edgeEnabled = new QCheckBox("Enable edge filling (clear for passthrough debugging)");
    method = new QComboBox;
    method->setObjectName("edgeAlgorithm");
    method->addItems({"Clamp / Edge Extend", "Gradient Edge Stretching", "Blur Edge Fill", "Mirror Edge Fill",
                      "Solid Color", "Temporal Depth Reconstruction"});
    colorButton = new QPushButton;
    connect(colorButton, &QPushButton::clicked, this, [this]() {
        auto chosen = QColorDialog::getColor(fillColor, this, translate("Edge fill color"),
                                             QColorDialog::DontUseNativeDialog);
        if (chosen.isValid()) {
            fillColor = chosen;
            colorButton->setText(translate("Choose color: ") + fillColor.name());
        }
    });
    width = number(.01, .5, .01);
    gradient = number(0, 1, .05);
    blurStrength = number(0, 1, .05);
    samples = new QSpinBox;
    samples->setRange(1, 16);
    radius = new QSpinBox;
    radius->setRange(0, 16);
    e->addRow(edgeEnabled);
    e->addRow("Algorithm", method);
    e->addRow("Fill color", colorButton);
    e->addRow("Stretch width", width);
    e->addRow("Gradient strength", gradient);
    e->addRow("Gradient edge samples", samples);
    e->addRow("Blur radius (pixels)", radius);
    e->addRow("Blur strength", blurStrength);
    temporalGroup = new QGroupBox("Temporal Depth Reconstruction");
    auto t = new QFormLayout(temporalGroup);
    temporalEnabled = new QCheckBox("Enable temporal reconstruction");
    temporalUseDepth = new QCheckBox("Use submitted OpenXR depth when compatible");
    historyFrames = new QSpinBox;
    historyFrames->setRange(2, 8);
    confidence = number(0, .99, .05);
    temporalBlend = number(0, 1, .05);
    feather = number(.001, .5, .01);
    feather->setDecimals(3);
    fallbackRadius = new QSpinBox;
    fallbackRadius->setRange(0, 32);
    fallbackStrength = number(0, 1, .05);
    fallbackWidth = number(.01, .5, .01);
    transition = number(.01, .5, .01);
    maxAge = number(.02, .5, .01);
    gpuBudget = number(0, 20, .1);
    gpuBudget->setSuffix(" ms (0 = off)");
    debugConfidence = new QCheckBox("Confidence mask");
    debugCoverage = new QCheckBox("Reprojection coverage");
    debugDepth = new QCheckBox("Reprojected depth");
    t->addRow(temporalEnabled);
    t->addRow(temporalUseDepth);
    t->addRow("History frames", historyFrames);
    t->addRow("Confidence threshold", confidence);
    t->addRow("Temporal blend strength", temporalBlend);
    t->addRow("Boundary feather width", feather);
    t->addRow("Fallback blur radius", fallbackRadius);
    t->addRow("Fallback blur strength", fallbackStrength);
    t->addRow("Fallback stretch width", fallbackWidth);
    t->addRow("Confidence transition width", transition);
    t->addRow("Maximum history age (seconds)", maxAge);
    t->addRow("GPU budget per eye pass", gpuBudget);
    t->addRow(debugConfidence);
    t->addRow(debugCoverage);
    t->addRow(debugDepth);
    e->addRow(temporalGroup);
    connect(method, &QComboBox::currentIndexChanged, this, [this, e](int m) {
        // Hide complete rows so unused labels and layout space disappear with their controls.
        e->setRowVisible(colorButton, m == 4);
        e->setRowVisible(width, m == 1 || m == 2);
        e->setRowVisible(gradient, m == 1);
        e->setRowVisible(samples, m == 1);
        e->setRowVisible(radius, m == 2);
        e->setRowVisible(blurStrength, m == 2);
        e->setRowVisible(temporalGroup, m == 5);
    });
    auto edgeScroll = new QScrollArea;
    edgeScroll->setWidgetResizable(true);
    edgeScroll->setWidget(edgePage);
    tabs->addTab(edgeScroll, "Edge Fill");
    auto diagnosticPage = new QWidget;
    auto di = new QVBoxLayout(diagnosticPage);
    temporalLabel = new QLabel;
    temporalLabel->setWordWrap(true);
    di->addWidget(temporalLabel);
    auto debugControls = new QHBoxLayout;
    logging = new QCheckBox("Write process logs");
    gpuTiming = new QCheckBox("GPU timestamps");
    fovBorders = new QCheckBox("Show FOV borders");
    debugControls->addWidget(logging);
    debugControls->addWidget(gpuTiming);
    debugControls->addWidget(fovBorders);
    auto saveDebug = new QPushButton("Save diagnostics settings");
    debugControls->addWidget(saveDebug);
    di->addLayout(debugControls);
    auto debug = fov::loadJson(fov::dataDirectory() / L"config.json").value("debug", fov::Json::object());
    if (!debug.is_object())
        debug = fov::Json::object();
    logging->setChecked(
        debug.contains("logging") && debug["logging"].is_boolean() ? debug["logging"].get<bool>() : true);
    gpuTiming->setChecked(debug.contains("gpuTiming") && debug["gpuTiming"].is_boolean()
                              ? debug["gpuTiming"].get<bool>()
                              : true);
    fovBorders->setChecked(debug.contains("showFovBorders") && debug["showFovBorders"].is_boolean()
                               ? debug["showFovBorders"].get<bool>()
                               : false);
    connect(saveDebug, &QPushButton::clicked, this, [this]() {
        try {
            fov::updateJson(fov::dataDirectory() / L"config.json", [&](fov::Json &j) {
                j["debug"] = {{"logging", logging->isChecked()},
                              {"gpuTiming", gpuTiming->isChecked()},
                              {"showFovBorders", fovBorders->isChecked()}};
            });
            saveLabel->setText(translate("Diagnostics settings saved."));
        } catch (const std::exception &e) {
            QMessageBox::warning(this, translate("Save diagnostics"), e.what());
        }
    });
    diagnostics = new QPlainTextEdit;
    diagnostics->setLayoutDirection(Qt::LeftToRight);
    diagnostics->setReadOnly(true);
    log = new QPlainTextEdit;
    log->setLayoutDirection(Qt::LeftToRight);
    log->setReadOnly(true);
    log->setMaximumBlockCount(300);
    di->addWidget(new QLabel("Live diagnostics (FOV angles in radians: left, right, up, down)"));
    di->addWidget(diagnostics, 2);
    di->addWidget(new QLabel("Status changes and errors"));
    di->addWidget(log, 1);
    tabs->addTab(diagnosticPage, "Diagnostics");
    auto settingsPage = new QWidget;
    auto settings = new QFormLayout(settingsPage);
    auto languageChoice = new QComboBox;
    languageChoice->setObjectName("languageChoice");
    languageChoice->setProperty("nativeNames", true);
    const QStringList names{"English", "Deutsch", "Français", "Español", "简体中文", "日本語",
                            "Русский", "हिन्दी",   "العربية",  "বাংলা",   "한국어"};
    auto languages = ui::languages();
    for (int i = 0; i < languages.size(); ++i)
        languageChoice->addItem(names[i], languages[i]);
    auto localeChoice = new QComboBox;
    localeChoice->setObjectName("localeChoice");
    localeChoice->setProperty("nativeNames", true);
    for (const auto &id : ui::locales()) {
        QLocale locale(id);
        localeChoice->addItem(
            locale.nativeLanguageName() + " — " + locale.nativeTerritoryName() + " (" + id + ")", id);
    }
    auto apply = new QPushButton("Apply settings");
    apply->setObjectName("applySettings");
    auto settingsStatus = new QLabel;
    settingsStatus->setObjectName("settingsStatus");
    auto explanation =
        new QLabel("Language changes apply immediately. Locale controls number and date formatting.");
    explanation->setWordWrap(true);
    auto technicalNote =
        new QLabel("Technical diagnostics and runtime messages retain their original format.");
    technicalNote->setWordWrap(true);
    settings->addRow("Language", languageChoice);
    settings->addRow("Locale", localeChoice);
    settings->addRow(explanation);
    settings->addRow(technicalNote);
    settings->addRow(apply);
    settings->addRow(settingsStatus);
    tabs->addTab(settingsPage, "Settings");
    ffrPanel = new FFRPanel;
    auto ffrScroll = new QScrollArea;
    ffrScroll->setWidgetResizable(true);
    ffrScroll->setWidget(ffrPanel);
    tabs->addTab(ffrScroll, "FFR");
    auto updateFFR = [this] {
        ffrPanel->setRequested(float(horizontal->value()), float(vertical->value()),
                               profileEnabled->isChecked() && edgeEnabled->isChecked(),
                               method->currentText());
    };
    connect(horizontal, qOverload<double>(&QDoubleSpinBox::valueChanged), this, updateFFR);
    connect(vertical, qOverload<double>(&QDoubleSpinBox::valueChanged), this, updateFFR);
    connect(profileEnabled, &QCheckBox::toggled, this, updateFFR);
    connect(edgeEnabled, &QCheckBox::toggled, this, updateFFR);
    connect(method, qOverload<int>(&QComboBox::currentIndexChanged), this, updateFFR);
    connect(method, &QComboBox::currentTextChanged, this, updateFFR);
    connect(apply, &QPushButton::clicked, this, [this, languageChoice, localeChoice, settingsStatus]() {
        const auto language = languageChoice->currentData().toString();
        const auto locale = localeChoice->currentData().toString();
        try {
            ui::savePreferences(language, locale);
            ui::setPreferences(language, locale);
            applyLanguage();
            settingsStatus->setText(translate("Settings saved."));
        } catch (const std::exception &e) {
            QMessageBox::warning(this, translate("Save failed"), e.what());
        }
    });
    auto footer = new QHBoxLayout;
    saveLabel = new QLabel;
    auto save = new QPushButton("Save profile");
    footer->addWidget(saveLabel, 1);
    footer->addWidget(save);
    outer->addLayout(footer);
    connect(save, &QPushButton::clicked, this, [this]() { saveEditor(); });
    ui::capture(this);
    ui::loadPreferences();
    languageChoice->setCurrentIndex(languageChoice->findData(ui::language()));
    localeChoice->setCurrentIndex(localeChoice->findData(ui::locale()));
    loadEditor();
    applyLanguage();
    auto timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this]() {
        refresh();
        discoverPipes();
    });
    timer->start(1000);
    auto historyTimer = new QTimer(this);
    connect(historyTimer, &QTimer::timeout, this, [this]() { refreshGames(); });
    historyTimer->start(5000);
}
void MainWindow::applyLanguage() {
    ui::retranslate(this);
    targetLabel->setText(selectedPath.isEmpty() ? translate("Editing global defaults")
                                                : translate("Editing profile: ") + selectedPath);
    colorButton->setText(translate("Choose color: ") + fillColor.name());
    saveLabel->clear();
    refresh();
    refreshGames();
}
void MainWindow::loadEditor() {
    loading = true;
    auto j = fov::loadJson(fov::dataDirectory() / L"config.json");
    auto p = fov::parseProfile(j.value("defaults", fov::Json::object()));
    if (!selectedPath.isEmpty() && j.contains("games") && j["games"].is_object())
        for (auto &[key, value] : j["games"].items())
            if (fov::normalizePath(key) == selectedPath.toStdString()) {
                p = fov::parseProfile(value, p);
                break;
            }
    ffrPanel->setObserved(fov::Json::object());
    ffrPanel->setSettings(p.ffr);
    profileEnabled->setChecked(p.enabled);
    editorVulkanEnabled = p.vulkanEnabled;
    editorVulkanProcessing = p.vulkanProcessing;
    edgeEnabled->setChecked(p.edgeEnabled);
    horizontal->setValue(p.horizontal);
    vertical->setValue(p.vertical);
    method->setCurrentIndex(p.method);
    auto &temporal = p.temporal;
    temporalEnabled->setChecked(temporal.enabled);
    temporalUseDepth->setChecked(temporal.useDepth);
    historyFrames->setValue(temporal.historyFrames);
    confidence->setValue(temporal.confidenceThreshold);
    temporalBlend->setValue(temporal.temporalBlendStrength);
    feather->setValue(temporal.boundaryFeatherWidth);
    fallbackRadius->setValue((int)temporal.fallbackBlurRadius);
    fallbackStrength->setValue(temporal.fallbackBlurStrength);
    fallbackWidth->setValue(temporal.fallbackStretchWidth);
    transition->setValue(temporal.transitionWidth);
    maxAge->setValue(temporal.maxHistoryAge);
    gpuBudget->setValue(temporal.gpuBudgetMs);
    debugConfidence->setChecked(temporal.showConfidenceMask);
    debugCoverage->setChecked(temporal.showReprojectionCoverage);
    debugDepth->setChecked(temporal.showDepth);
    fillColor = QColor::fromRgbF(p.color[0], p.color[1], p.color[2]);
    colorButton->setText(translate("Choose color: ") + fillColor.name());
    width->setValue(p.stretchWidth);
    gradient->setValue(p.gradientStrength);
    samples->setValue(p.edgeSamples);
    radius->setValue(p.blurRadius);
    blurStrength->setValue(p.blurStrength);
    targetLabel->setText(selectedPath.isEmpty() ? translate("Editing global defaults")
                                                : translate("Editing profile: ") + selectedPath);
    saveLabel->clear();
    loading = false;
    QMetaObject::invokeMethod(method, "currentIndexChanged", Qt::DirectConnection, Q_ARG(int, p.method));
}
void MainWindow::saveEditor() {
    fov::Profile p;
    p.ffr = ffrPanel->settings();
    p.vulkanEnabled = editorVulkanEnabled;
    p.vulkanProcessing = editorVulkanProcessing;
    p.enabled = profileEnabled->isChecked();
    p.edgeEnabled = edgeEnabled->isChecked();
    p.horizontal = (float)horizontal->value();
    p.vertical = (float)vertical->value();
    p.method = method->currentIndex();
    p.temporal.enabled = temporalEnabled->isChecked();
    p.temporal.useDepth = temporalUseDepth->isChecked();
    p.temporal.historyFrames = historyFrames->value();
    p.temporal.confidenceThreshold = (float)confidence->value();
    p.temporal.temporalBlendStrength = (float)temporalBlend->value();
    p.temporal.boundaryFeatherWidth = (float)feather->value();
    p.temporal.fallbackBlurRadius = (float)fallbackRadius->value();
    p.temporal.fallbackBlurStrength = (float)fallbackStrength->value();
    p.temporal.fallbackStretchWidth = (float)fallbackWidth->value();
    p.temporal.transitionWidth = (float)transition->value();
    p.temporal.maxHistoryAge = (float)maxAge->value();
    p.temporal.gpuBudgetMs = (float)gpuBudget->value();
    p.temporal.showConfidenceMask = debugConfidence->isChecked();
    p.temporal.showReprojectionCoverage = debugCoverage->isChecked();
    p.temporal.showDepth = debugDepth->isChecked();
    p.color = {fillColor.redF(), fillColor.greenF(), fillColor.blueF()};
    p.stretchWidth = (float)width->value();
    p.gradientStrength = (float)gradient->value();
    p.edgeSamples = samples->value();
    p.blurRadius = radius->value();
    p.blurStrength = (float)blurStrength->value();
    try {
        fov::updateJson(fov::dataDirectory() / L"config.json", [&](fov::Json &j) {
            if (!j.contains("enabled") || !j["enabled"].is_boolean())
                j["enabled"] = true;
            if (selectedPath.isEmpty())
                j["defaults"] = fov::profileJson(p);
            else {
                if (!j.contains("games") || !j["games"].is_object())
                    j["games"] = fov::Json::object();
                std::vector<std::string> duplicates;
                for (auto &[key, _] : j["games"].items())
                    if (fov::normalizePath(key) == selectedPath.toStdString())
                        duplicates.push_back(key);
                for (auto &key : duplicates)
                    j["games"].erase(key);
                j["games"][selectedPath.toStdString()] = fov::profileJson(p);
            }
        });
        saveLabel->setText(translate("Saved. Running applications reload within one second."));
        refreshGames();
    } catch (const std::exception &e) {
        QMessageBox::warning(this, translate("Save failed"), e.what());
    }
}
void MainWindow::refresh() {
    auto p = fov::moduleDirectory() / L"OpenXRFovExtension.json";
    auto r = fov::registration(p);
    QString status = r == fov::Registration::Enabled    ? translate("Enabled")
                     : r == fov::Registration::Disabled ? translate("Disabled")
                     : r == fov::Registration::Missing  ? translate("Unregistered")
                                                        : translate("Invalid registry value");
    try {
        fov::validateManifest(p);
    } catch (const std::exception &e) {
        status += " — " + QString::fromUtf8(e.what());
    }
    registrationLabel->setText(translate("Layer registration (64-bit HKLM): ") + status);
    runtimeLabel->setText(translate("Active OpenXR runtime: ") +
                          QString::fromStdWString(fov::activeRuntime()));
    QStringList lines;
    for (auto it = live.begin(); it != live.end(); ++it) {
        auto &j = it.value();
        lines << QString("%1 | %2 | %3 | H %4 / V %5 | %6\n%7")
                     .arg(text(j.value("application", "Unknown")), text(j.value("graphicsApi", "Unknown")),
                          text(j.value("status", "Unknown")))
                     .arg(QLocale().toString(
                         j.value("settings", fov::Json::object()).value("horizontalFovMultiplier", 1.0), 'f',
                         2))
                     .arg(QLocale().toString(
                         j.value("settings", fov::Json::object()).value("verticalFovMultiplier", 1.0), 'f',
                         2))
                     .arg(text(j.value("settings", fov::Json::object()).value("edgeFillMethod", "Unknown")),
                          text(j.value("lastError", "")));
    }
    fov::Json ffr = fov::Json::object();
    for (auto it = live.begin(); it != live.end(); ++it) {
        if ((!selectedPath.isEmpty() && text(it.value().value("executable", "")) == selectedPath) ||
            (selectedPath.isEmpty() && live.size() == 1)) {
            ffr = it.value().value("ffr", fov::Json::object());
            ffr["scene"] = it.value().value("ffrScene", fov::Json::array());
        }
    }
    ffrPanel->setObserved(ffr);
    liveLabel->setText(lines.empty() ? translate("No VR application connected") : lines.join("\n\n"));
    fov::Json all = fov::Json::object();
    for (auto it = live.begin(); it != live.end(); ++it)
        all[it.key().toStdString()] = it.value();
    diagnostics->setPlainText(text(all.dump(2)));
    QStringList temporalLines;
    for (auto it = live.begin(); it != live.end(); ++it) {
        auto eyes = it.value().value("temporalDepth", fov::Json::array());
        if (!eyes.is_array())
            continue;
        for (auto &eye : eyes) {
            if (!eye.is_object())
                continue;
            auto coverage = eye.value("reconstructedPercent", fov::Json());
            auto fallback = eye.value("fallbackPercent", fov::Json());
            temporalLines << translate("Eye %1: %2 · depth %3 · history %4 · reconstructed %5 / fallback %6 "
                                       "(sampled, delayed) · invalidations %7")
                                 .arg(eye.value("eye", 0))
                                 .arg(text(eye.value("mode", "Unknown")),
                                      eye.value("depthAvailable", false) ? translate("Yes") : translate("No"))
                                 .arg(eye.value("validHistoryFrames", 0))
                                 .arg(coverage.is_number()
                                          ? QLocale().toString(coverage.get<double>(), 'f', 1) + "%"
                                          : translate("pending"),
                                      fallback.is_number()
                                          ? QLocale().toString(fallback.get<double>(), 'f', 1) + "%"
                                          : translate("pending"))
                                 .arg(eye.value("historyInvalidations", uint64_t(0)));
        }
    }
    temporalLabel->setText(temporalLines.empty() ? translate("Temporal reconstruction: no measurements")
                                                 : temporalLines.join("\n"));
}
void MainWindow::refreshGames() {
    auto h = fov::loadJson(fov::dataDirectory() / L"history.json");
    auto config = fov::loadJson(fov::dataDirectory() / L"config.json");
    QString current;
    int row = games->currentRow();
    if (row >= 0)
        current = games->item(row, 1)->text();
    games->setSortingEnabled(false);
    games->setRowCount(0);
    for (auto &[key, value] : h.items()) {
        if (!value.is_object())
            continue;
        int r = games->rowCount();
        games->insertRow(r);
        bool custom =
            config.contains("games") && config["games"].is_object() && config["games"].contains(key);
        QStringList values{text(value.value("applicationName", "Unknown")),
                           text(key),
                           text(value.value("openxrApplicationName", "")),
                           localizedDate(value.value("firstLaunch", "")),
                           localizedDate(value.value("lastLaunch", "")),
                           QString::number(value.value("launchCount", uint64_t(0))),
                           text(value.value("graphicsApi", "Unknown")),
                           custom ? translate("Custom") : translate("Global defaults")};
        for (int c = 0; c < 8; c++) {
            QTableWidgetItem *item =
                c == 3 || c == 4
                    ? new HistoryDateItem(values[c], value.value(c == 3 ? "firstLaunch" : "lastLaunch", ""))
                    : new QTableWidgetItem(values[c]);
            if (c == 5)
                item->setData(Qt::DisplayRole,
                              QVariant::fromValue<qulonglong>(value.value("launchCount", uint64_t(0))));
            games->setItem(r, c, item);
        }
    }
    games->setSortingEnabled(true);
    for (int r = 0; r < games->rowCount(); r++) {
        bool match = search->text().isEmpty();
        for (int c = 0; c < 8; c++)
            match |= games->item(r, c)->text().contains(search->text(), Qt::CaseInsensitive);
        games->setRowHidden(r, !match);
        if (games->item(r, 1)->text() == current)
            games->selectRow(r);
    }
}
void MainWindow::discoverPipes() {
    WIN32_FIND_DATAW pipeData;
    HANDLE h = FindFirstFileW(L"\\\\.\\pipe\\*", &pipeData);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        QString name = QString::fromWCharArray(pipeData.cFileName);
        if (!name.startsWith("OpenXRFovExtension-") || sockets.contains(name))
            continue;
        auto socket = new QLocalSocket(this);
        sockets[name] = socket;
        auto cleanup = [this, socket, name]() {
            if (sockets.value(name) != socket)
                return;
            sockets.remove(name);
            buffers.remove(name);
            live.remove(name);
            socket->deleteLater();
            refresh();
        };
        connect(socket, &QLocalSocket::disconnected, this, cleanup);
        connect(socket, &QLocalSocket::errorOccurred, this,
                [cleanup](QLocalSocket::LocalSocketError) { cleanup(); });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, name]() {
            auto &buffer = buffers[name];
            buffer += socket->readAll();
            if (buffer.size() > 262144) {
                socket->abort();
                return;
            }
            int end;
            while ((end = buffer.indexOf('\n')) >= 0) {
                auto line = buffer.left(end);
                buffer.remove(0, end + 1);
                try {
                    auto j = fov::Json::parse(line.toStdString());
                    if (!j.is_object())
                        continue;
                    auto key = name;
                    auto previous = live.contains(key) ? live[key] : fov::Json::object();
                    if (j.value("status", "") != previous.value("status", "") ||
                        j.value("lastError", "") != previous.value("lastError", ""))
                        log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") +
                                             text(j.value("application", "")) + ": " +
                                             text(j.value("status", "")) + " " +
                                             text(j.value("lastError", "")));
                    live[key] = j;
                } catch (...) {
                }
            }
            refresh();
        });
        socket->connectToServer(name, QIODevice::ReadOnly);
    } while (FindNextFileW(h, &pipeData));
    FindClose(h);
}
