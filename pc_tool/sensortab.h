// sensortab.h - Sensor monitor tab (QPainter real-time curve)
#ifndef SENSORTAB_H
#define SENSORTAB_H

#include <QWidget>
#include <QTimer>
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QtMath>

class SensorTab : public QWidget
{
    Q_OBJECT

public:
    explicit SensorTab(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;   // Qt drawing entry point
    void resizeEvent(QResizeEvent *) override; // window resize -> redraw

private slots:
    void onTimer();   // called every 500ms: add data point + trigger repaint

private:
    // Ring buffer (circular buffer) for 100 data points
    static constexpr int BUF_SIZE = 100;
    double m_temp[BUF_SIZE];   // temperature history (deg C)
    double m_humi[BUF_SIZE];   // humidity history (%RH)
    int    m_writeIdx;         // next write position
    int    m_count;            // valid data count (max 100)

    // Current values (displayed as big numbers at top-left)
    double m_curTemp;
    double m_curHumi;

    // Timer for simulated data updates
    QTimer *m_timer;
    double m_simPhase;   // for generating smooth sine-wave-like fake data

    // Drawing helpers
    void drawHeader(QPainter &p);           // big numbers + labels + online status
    void drawCurveArea(QPainter &p);        // chart border + grid + axis labels
    void drawGrid(QPainter &p, const QRect &area);
    void drawTempCurve(QPainter &p, const QRect &area);
    void drawHumiCurve(QPainter &p, const QRect &area);

    // Map data value -> Y pixel (invert because screen Y grows downward)
    double valueToY(double value, double minVal, double maxVal,
                    int areaTop, int areaHeight) const;
};

#endif // SENSORTAB_H