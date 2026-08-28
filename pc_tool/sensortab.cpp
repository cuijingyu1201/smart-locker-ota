// sensortab.cpp - Sensor monitor tab implementation
#include "sensortab.h"
#include <QRect>
#include <QFont>
#include <QPainterPath>
#include <cstdlib>
#include <ctime>

SensorTab::SensorTab(QWidget *parent)
    : QWidget(parent)
    , m_writeIdx(0)
    , m_count(0)
    , m_curTemp(25.0)
    , m_curHumi(60.0)
    , m_simPhase(0.0)
{
    srand(time(nullptr));
    // Initialize ring buffers with default values
    for (int i = 0; i < BUF_SIZE; ++i) {
        m_temp[i] = m_curTemp;
        m_humi[i] = m_curHumi;
    }
    m_count = BUF_SIZE;  // start full so the curve fills immediately

    // Timer: generate a new data point every 500ms (simulated sensor)
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &SensorTab::onTimer);
    m_timer->start(500);
}

// ------------------------------------------------------------------
// Timer slot: called every 500ms
// ------------------------------------------------------------------
void SensorTab::onTimer()
{
    // Simulate real sensor data: sine wave + small noise
    // This mimics DHT11 readings from the STM32 board
    m_simPhase += 0.15;
    m_curTemp = 25.0 + 3.0 * qSin(m_simPhase) + (rand() % 100) / 50.0;   // 22~28 deg C
    m_curHumi = 60.0 + 8.0 * qSin(m_simPhase + 0.5) + (rand() % 100) / 30.0; // 50~70 %RH

    // Write into ring buffer
    m_temp[m_writeIdx] = m_curTemp;
    m_humi[m_writeIdx] = m_curHumi;
    m_writeIdx = (m_writeIdx + 1) % BUF_SIZE;
    if (m_count < BUF_SIZE) m_count++;

    // Tell Qt to redraw (triggers paintEvent)
    update();
}

// ------------------------------------------------------------------
// Main drawing entry point - called by Qt when widget needs repaint
// ------------------------------------------------------------------
void SensorTab::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);  // smooth curves

    // Background
    p.fillRect(rect(), QColor(245, 245, 245));

    // Three layers: header (top) -> curve area (center)
    drawHeader(p);
    drawCurveArea(p);
}

void SensorTab::resizeEvent(QResizeEvent *)
{
    update();  // force redraw on resize
}

// ------------------------------------------------------------------
// Top header: big number display + online status
// ------------------------------------------------------------------
void SensorTab::drawHeader(QPainter &p)
{
    const int PAD = 20;
    const int H = 100;   // header height

    // Header background
    p.fillRect(PAD, PAD, width() - 2 * PAD, H, Qt::white);

    QFont labelFont("Arial", 10, QFont::Bold);
    QFont bigFont("Consolas", 28, QFont::Bold);

    // --- Temperature block ---
    // Label
    p.setFont(labelFont);
    p.setPen(QColor(100, 100, 100));
    p.drawText(PAD + 20, PAD + 28, "Temperature");
    // Big number
    p.setFont(bigFont);
    p.setPen(QColor(220, 60, 40));
    QString tStr = QString("%1 C").arg(m_curTemp, 0, 'f', 1);
    p.drawText(PAD + 20, PAD + 80, tStr);

    // --- Humidity block ---
    const int blockW = 250;
    int hx = PAD + blockW;
    p.setFont(labelFont);
    p.setPen(QColor(100, 100, 100));
    p.drawText(hx + 20, PAD + 28, "Humidity");
    p.setFont(bigFont);
    p.setPen(QColor(40, 120, 200));
    QString hStr = QString("%1 %RH").arg(m_curHumi, 0, 'f', 1);
    p.drawText(hx + 20, PAD + 80, hStr);

    // --- Firmware version + online status ---
    int infoX = hx + blockW;
    p.setFont(labelFont);
    p.setPen(QColor(80, 80, 80));
    p.drawText(infoX + 20, PAD + 28, "Firmware");
    p.setFont(QFont("Consolas", 14));
    p.setPen(QColor(60, 60, 60));
    p.drawText(infoX + 20, PAD + 55, "v1.0.5 (build 12)");

    // Online dot + text
    const int dotY = PAD + 75;
    const int dotX = infoX + 25;
    p.setBrush(QColor(50, 200, 80));  // green = online
    p.setPen(Qt::NoPen);
    p.drawEllipse(dotX, dotY - 6, 12, 12);
    p.setFont(QFont("Arial", 10));
    p.setPen(QColor(50, 200, 80));
    p.drawText(dotX + 18, dotY + 3, "Online");
}

// ------------------------------------------------------------------
// Curve area: border + grid + two curves (temp + humi)
// ------------------------------------------------------------------
void SensorTab::drawCurveArea(QPainter &p)
{
    const int PAD = 20;
    const int HEADER_H = 100;
    const int GAP = 15;

    // Two chart areas (temp on top, humi on bottom)
    // Each: top-left corner, width, height
    int totalH = height() - 2 * PAD - HEADER_H - GAP;
    int chartH = (totalH - GAP) / 2;
    int chartW = width() - 2 * PAD;
    int top1 = PAD + HEADER_H + GAP;
    int top2 = top1 + chartH + GAP;

    QRect tempArea(PAD, top1, chartW, chartH);
    QRect humiArea(PAD, top2, chartW, chartH);

    // Draw temp chart
    drawGrid(p, tempArea);
    drawTempCurve(p, tempArea);

    // Draw humi chart
    drawGrid(p, humiArea);
    drawHumiCurve(p, humiArea);
}

void SensorTab::drawGrid(QPainter &p, const QRect &area)
{
    // Chart border
    p.setPen(QPen(QColor(200, 200, 200), 1));
    p.setBrush(Qt::white);
    p.drawRect(area);

    // Horizontal grid lines (5 lines)
    p.setPen(QPen(QColor(230, 230, 230), 1, Qt::DashLine));
    for (int i = 1; i < 5; ++i) {
        int y = area.top() + area.height() * i / 5;
        p.drawLine(area.left(), y, area.right(), y);
    }

    // Vertical grid lines
    for (int i = 1; i < 10; ++i) {
        int x = area.left() + area.width() * i / 10;
        p.drawLine(x, area.top(), x, area.bottom());
    }
}

// ------------------------------------------------------------------
// Temperature curve (red, scale 15~35 deg C)
// ------------------------------------------------------------------
void SensorTab::drawTempCurve(QPainter &p, const QRect &area)
{
    // Y-axis labels on the left
    p.setPen(QColor(120, 120, 120));
    p.setFont(QFont("Consolas", 8));
    p.drawText(area.left() + 5, area.top() + 12, "35 C");
    p.drawText(area.left() + 5, area.bottom() - 5, "15 C");

    // Curve pen: red, 2px
    QPen pen(QColor(220, 60, 40), 2);
    p.setPen(pen);

    if (m_count < 2) return;

    // Start index (most recent data, oldest data wraps around)
    int start = (m_writeIdx - m_count + BUF_SIZE) % BUF_SIZE;

    QPainterPath path;
    bool firstPoint = true;

    for (int i = 0; i < m_count; ++i) {
        int bufIdx = (start + i) % BUF_SIZE;

        // X: evenly space across chart area
        double x = area.left() + (double)i / (m_count - 1) * area.width();

        // Y: map value to pixel (inverted)
        double y = valueToY(m_temp[bufIdx], 15.0, 35.0,
                            area.top(), area.height());

        if (firstPoint) {
            path.moveTo(x, y);
            firstPoint = false;
        } else {
            path.lineTo(x, y);
        }
    }
    p.drawPath(path);

    // Filled area under curve (light red)
    path.lineTo(area.right(), area.bottom());
    path.lineTo(area.left(), area.bottom());
    path.closeSubpath();
    p.setBrush(QColor(220, 60, 40, 30));   // alpha=30 -> very transparent red
    p.setPen(Qt::NoPen);
    p.drawPath(path);
}

// ------------------------------------------------------------------
// Humidity curve (blue, scale 30~90 %RH)
// ------------------------------------------------------------------
void SensorTab::drawHumiCurve(QPainter &p, const QRect &area)
{
    p.setPen(QColor(120, 120, 120));
    p.setFont(QFont("Consolas", 8));
    p.drawText(area.left() + 5, area.top() + 12, "90 %");
    p.drawText(area.left() + 5, area.bottom() - 5, "30 %");

    QPen pen(QColor(40, 120, 200), 2);
    p.setPen(pen);

    if (m_count < 2) return;

    int start = (m_writeIdx - m_count + BUF_SIZE) % BUF_SIZE;

    QPainterPath path;
    bool firstPoint = true;

    for (int i = 0; i < m_count; ++i) {
        int bufIdx = (start + i) % BUF_SIZE;
        double x = area.left() + (double)i / (m_count - 1) * area.width();
        double y = valueToY(m_humi[bufIdx], 30.0, 90.0,
                            area.top(), area.height());

        if (firstPoint) {
            path.moveTo(x, y);
            firstPoint = false;
        } else {
            path.lineTo(x, y);
        }
    }
    p.drawPath(path);

    path.lineTo(area.right(), area.bottom());
    path.lineTo(area.left(), area.bottom());
    path.closeSubpath();
    p.setBrush(QColor(40, 120, 200, 30));
    p.setPen(Qt::NoPen);
    p.drawPath(path);
}

// ------------------------------------------------------------------
// Helper: value -> Y pixel
// value in [minVal, maxVal] -> pixel in [areaTop, areaTop+areaHeight]
// NOTE: screen Y grows downward, so invert
// ------------------------------------------------------------------
double SensorTab::valueToY(double value, double minVal, double maxVal,
                           int areaTop, int areaHeight) const
{
    double ratio = (value - minVal) / (maxVal - minVal);  // 0.0 ~ 1.0
    // Clamp to [0, 1] so out-of-range values don't crash drawing
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    return areaTop + (1.0 - ratio) * areaHeight;  // invert!
}