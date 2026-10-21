#pragma once
#include "common/FFR.h"
#include <QWidget>
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QLabel;
class FFRPreview;
class FFRPanel : public QWidget {
    QCheckBox *enabled, *debug;
    QDoubleSpinBox *centerX, *centerY, *middleX, *middleY, *offsetX, *offsetY, *transition;
    QSpinBox *fullQuality, *reducedQuality;
    QComboBox *geometry, *policy, *debugView, *fallback;
    QLabel *effective, *mode, *support, *reconstruction, *fullLabel, *reducedLabel;
    FFRPreview *preview;
    fov::Json observed = fov::Json::object();
    float requestedH = 1, requestedV = 1;
    bool cropEnabled = true, updating = false;
    QString algorithm;
    void updateView();

  public:
    explicit FFRPanel(QWidget *parent = nullptr);
    fov::FFRSettings settings() const;
    void setSettings(const fov::FFRSettings &);
    void setRequested(float h, float v, bool cropping, const QString &method);
    void setObserved(const fov::Json &);
};
