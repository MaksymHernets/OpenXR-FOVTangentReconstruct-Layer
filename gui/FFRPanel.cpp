#include "FFRPanel.h"
#include "Localization.h"
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QLabel>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QImage>
using ui::translate;
class FFRPreview : public QWidget {
  public:
    std::array<fov::FFRPlan, 2> plans;
    FFRPreview() {
        setMinimumSize(440, 180);
        setObjectName("ffrPreview");
        setLayoutDirection(Qt::LeftToRight);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#101825"));
        for (int eye = 0; eye < 2; ++eye) {
            auto &p = plans[eye];
            QImage image(240, 160, QImage::Format_RGB32);
            for (int y = 0; y < 160; ++y)
                for (int x = 0; x < 240; ++x) {
                    float u = (x + .5f) / 240, v = (y + .5f) / 160;
                    auto r = p.renderedRect;
                    bool crop = u < r[0] || u >= r[0] + r[2] || v < r[1] || v >= r[1] + r[3];
                    QColor color;
                    if (crop)
                        color = ((x / 8 + y / 8) % 2) ? QColor("#d044c6") : QColor("#68309d");
                    else {
                        int zone = fov::ffrZone(p, (u - r[0]) / r[2], (v - r[1]) / r[3]);
                        color = zone == 1   ? QColor("#27cfa7")
                                : zone == 2 ? QColor("#f5b64c")
                                            : QColor("#e66058");
                        if (p.settings.debug && (x % 20 == 0 || y % 20 == 0))
                            color = Qt::white;
                    }
                    image.setPixelColor(x, y, color);
                }
            QRect dst(10 + eye * width() / 2, 12, width() / 2 - 20, height() - 24);
            painter.drawImage(dst, image);
            painter.setPen(Qt::white);
            painter.drawRect(dst);
        }
    }
};
FFRPanel::FFRPanel(QWidget *parent) : QWidget(parent) {
    auto layout = new QVBoxLayout(this);
    auto form = new QFormLayout;
    layout->addLayout(form);
    enabled = new QCheckBox("Enable FFR");
    enabled->setObjectName("ffrEnabled");
    form->addRow(enabled);
    effective = new QLabel;
    effective->setObjectName("ffrEffective");
    form->addRow(effective);
    mode = new QLabel;
    mode->setObjectName("ffrMode");
    form->addRow(mode);
    auto radii = [&](const char *name, double lo, double hi) {
        auto box = new QDoubleSpinBox;
        box->setObjectName(name);
        box->setRange(lo, hi);
        box->setDecimals(2);
        box->setSingleStep(.01);
        return box;
    };
    centerX = radii("ffrCenterX", .02, .49);
    centerY = radii("ffrCenterY", .02, .49);
    middleX = radii("ffrMiddleX", .02, .5);
    middleY = radii("ffrMiddleY", .02, .5);
    offsetX = radii("ffrOffsetX", -.4, .4);
    offsetY = radii("ffrOffsetY", -.4, .4);
    transition = radii("ffrTransition", 0, .15);
    auto pair = [](QWidget *a, QWidget *b) {
        auto w = new QWidget;
        auto h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(a);
        h->addWidget(b);
        return w;
    };
    form->addRow("Center radii X / Y", pair(centerX, centerY));
    form->addRow("Middle radii X / Y", pair(middleX, middleY));
    form->addRow("Center offset X / Y", pair(offsetX, offsetY));
    form->addRow("Transition band", transition);
    auto quality = [](const char *name, int lo, int hi) {
        auto w = new QSpinBox;
        w->setObjectName(name);
        w->setRange(lo, hi);
        w->setSuffix("%");
        return w;
    };
    fullQuality = quality("ffrFullMiddle", 50, 75);
    reducedQuality = quality("ffrReducedMiddle", 50, 75);
    outerQuality = quality("ffrOuter", 10, 20);
    fullLabel = new QLabel("Middle quality — full FOV");
    reducedLabel = new QLabel("Middle quality — reduced FOV");
    outerLabel = new QLabel("Outer quality");
    form->addRow(fullLabel, fullQuality);
    form->addRow(reducedLabel, reducedQuality);
    form->addRow(outerLabel, outerQuality);
    geometry = new QComboBox;
    geometry->setObjectName("ffrGeometry");
    geometry->addItems({"Elliptical", "Rectangular"});
    form->addRow("Zone geometry", geometry);
    policy = new QComboBox;
    policy->setObjectName("ffrPolicy");
    policy->addItems({"Conservative", "Balanced", "Aggressive"});
    form->addRow("Rate mapping policy", policy);
    debug = new QCheckBox("Colored peripheral test grid");
    debug->setObjectName("ffrDebug");
    form->addRow(debug);
    reconstruction = new QLabel;
    reconstruction->setObjectName("ffrReconstruction");
    form->addRow(reconstruction);
    support = new QLabel;
    support->setObjectName("ffrSupport");
    support->setWordWrap(true);
    form->addRow(support);
    layout->addWidget(new QLabel(
        "Green: center 100% · Amber: middle · Red: rendered outer zone · Purple: cropped reconstruction"));
    preview = new FFRPreview;
    layout->addWidget(preview);
    layout->addWidget(new QLabel("Configured map preview; hardware application is not confirmed by this "
                                 "preview. Quality targets are not literal shading-rate percentages."));
    for (auto spin : {centerX, centerY, middleX, middleY, offsetX, offsetY, transition})
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { updateView(); });
    for (auto spin : {fullQuality, reducedQuality, outerQuality})
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { updateView(); });
    for (auto combo : {geometry, policy})
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateView(); });
    for (auto box : {enabled, debug})
        connect(box, &QCheckBox::toggled, this, [this] { updateView(); });
    setSettings({});
    for (auto label : {effective, mode, support, reconstruction})
        label->setText({});
}
fov::FFRSettings FFRPanel::settings() const {
    fov::FFRSettings s;
    s.enabled = enabled->isChecked();
    s.debug = debug->isChecked();
    s.centerX = float(centerX->value());
    s.centerY = float(centerY->value());
    s.middleX = std::max(s.centerX, float(middleX->value()));
    s.middleY = std::max(s.centerY, float(middleY->value()));
    s.offsetX = float(offsetX->value());
    s.offsetY = float(offsetY->value());
    s.transition = float(transition->value());
    s.fullMiddleQuality = fullQuality->value();
    s.reducedMiddleQuality = reducedQuality->value();
    s.outerQuality = outerQuality->value();
    s.geometry = geometry->currentIndex() == 0 ? "ellipse" : "rectangle";
    const char *policies[]{"conservative", "balanced", "aggressive"};
    s.policy = policies[std::max(0, policy->currentIndex())];
    return s;
}
void FFRPanel::setSettings(const fov::FFRSettings &s) {
    updating = true;
    middleX->setMinimum(.02);
    middleY->setMinimum(.02);
    enabled->setChecked(s.enabled);
    debug->setChecked(s.debug);
    centerX->setValue(s.centerX);
    centerY->setValue(s.centerY);
    middleX->setValue(s.middleX);
    middleY->setValue(s.middleY);
    offsetX->setValue(s.offsetX);
    offsetY->setValue(s.offsetY);
    transition->setValue(s.transition);
    fullQuality->setValue(s.fullMiddleQuality);
    reducedQuality->setValue(s.reducedMiddleQuality);
    outerQuality->setValue(s.outerQuality);
    geometry->setCurrentIndex(s.geometry == "rectangle" ? 1 : 0);
    policy->setCurrentIndex(s.policy == "conservative" ? 0 : s.policy == "aggressive" ? 2 : 1);
    updating = false;
    updateView();
}
void FFRPanel::setRequested(float h, float v, bool cropping, const QString &method) {
    requestedH = h;
    requestedV = v;
    cropEnabled = cropping;
    algorithm = method;
    updateView();
}
void FFRPanel::setObserved(const fov::Json &j) {
    observed = j.is_object() ? j : fov::Json::object();
    updateView();
}
void FFRPanel::updateView() {
    if (updating)
        return;
    auto s = settings();
    middleX->setMinimum(s.centerX);
    middleY->setMinimum(s.centerY);
    XrFovf baseline{-std::atan(1.f), std::atan(1.f), std::atan(1.f), -std::atan(1.f)};
    auto plan = fov::makeFFRPlan(
        baseline, fov::reduce(baseline, cropEnabled ? requestedH : 1, cropEnabled ? requestedV : 1), s);
    preview->plans = {plan, plan};
    bool live = false;
    try {
        if (observed.contains("eyes") && observed["eyes"].is_array() && observed["eyes"].size() == 2) {
            auto angles = [](const fov::Json &a) {
                if (!a.is_array() || a.size() != 4)
                    throw std::runtime_error("Bad FOV");
                return XrFovf{a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a[3].get<float>()};
            };
            for (int eye = 0; eye < 2; ++eye) {
                auto &e = observed["eyes"][eye];
                preview->plans[eye] =
                    fov::makeFFRPlan(angles(e["runtimeBaselineFov"]), angles(e["activeFov"]), s);
            }
            live = preview->plans[0].valid && preview->plans[1].valid;
        }
    } catch (...) {
        live = false;
    }
    if (!live)
        preview->plans = {plan, plan};
    bool full = false, reduced = false;
    for (auto &p : preview->plans) {
        full |= p.mode == fov::FFRZoneMode::ThreeRenderedZones;
        reduced |= p.mode == fov::FFRZoneMode::TwoRenderedZonesWithCrop;
    }
    outerQuality->setVisible(full);
    outerLabel->setVisible(full);
    fullQuality->setVisible(full);
    fullLabel->setVisible(full);
    reducedQuality->setVisible(reduced);
    reducedLabel->setVisible(reduced);
    reconstruction->setVisible(reduced);
    mode->setText(translate(full && reduced ? "Per-eye mixed zone modes"
                            : full          ? "Three rendered zones"
                                            : "Two rendered zones + cropped reconstruction"));
    if (live) {
        auto a = preview->plans[0], b = preview->plans[1];
        effective->setText(translate("Observed effective FOV — L %1 / %2 · R %3 / %4")
                               .arg(a.horizontal * 100, 0, 'f', 1)
                               .arg(a.vertical * 100, 0, 'f', 1)
                               .arg(b.horizontal * 100, 0, 'f', 1)
                               .arg(b.vertical * 100, 0, 'f', 1) +
                           "%");
    } else
        effective->setText(translate("Effective FOV unavailable; showing configured preview"));
    reconstruction->setText(
        translate("Cropped region → existing reconstruction: %1")
            .arg(live ? QString::fromStdString(observed.value("reconstructionMode", std::string("unknown")))
                      : algorithm));
    if (live && !observed.value("reconstructionAvailable", false))
        reconstruction->setText(translate("Reconstruction unavailable; native projection is preserved"));
    auto c = observed.value("capabilities", fov::Json::object());
    auto hardware = c.contains("hardwareSupported") && c["hardwareSupported"].is_boolean() &&
                    c["hardwareSupported"].get<bool>();
    QString rates = translate("Actual applied rates: unconfirmed");
    if (observed.contains("scene"))
        for (auto &item : observed["scene"])
            if (item.contains("recordedRates")) {
                rates += "\n" + translate("Recorded rates (eye %1): %2")
                                    .arg(item.value("eye", 0))
                                    .arg(QString::fromStdString(item["recordedRates"].dump()));
            }
    support->setText(translate(!live      ? "Backend support unavailable without a live session"
                               : hardware ? "D3D12 Tier 2 available; explicit scene integration required"
                                          : "Hardware FFR unsupported for the current integration") +
                     "\n" + rates);
    preview->update();
}
