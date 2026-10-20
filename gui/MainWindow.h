#pragma once
#include <QMainWindow>
#include <QMap>
#include <QColor>
#include "common/ConfigManager.h"
class QLabel;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QSlider;
class QTableWidget;
class QLineEdit;
class QPlainTextEdit;
class QLocalSocket;
class QPushButton;
class QGroupBox;
class MainWindow : public QMainWindow {
    QString selectedPath;
    QMap<QString, fov::Json> live;
    QLabel *registrationLabel, *runtimeLabel, *liveLabel, *targetLabel, *saveLabel;
    QCheckBox *profileEnabled, *edgeEnabled, *linked;
    QCheckBox *logging, *gpuTiming, *fovBorders;
    QDoubleSpinBox *horizontal, *vertical, *width, *gradient, *blurStrength;
    QSpinBox *samples, *radius;
    QSlider *horizontalSlider, *verticalSlider;
    QComboBox *method;
    QPushButton *colorButton;
    QColor fillColor{Qt::black};
    QGroupBox *temporalGroup;
    QCheckBox *temporalEnabled, *temporalUseDepth, *debugConfidence, *debugCoverage, *debugDepth;
    QSpinBox *historyFrames, *fallbackRadius;
    QDoubleSpinBox *confidence, *temporalBlend, *feather, *fallbackStrength, *fallbackWidth, *transition,
        *maxAge, *gpuBudget;
    QLabel *temporalLabel;
    QTableWidget *games;
    QLineEdit *search;
    QPlainTextEdit *diagnostics, *log;
    QMap<QString, QLocalSocket *> sockets;
    QMap<QString, QByteArray> buffers;
    bool loading = false;
    void refresh();
    void refreshGames();
    void loadEditor();
    void saveEditor();
    void discoverPipes();
    void applyLanguage();

  public:
    MainWindow();
};
