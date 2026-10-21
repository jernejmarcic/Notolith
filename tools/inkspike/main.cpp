// Pen spike: logs every tablet event (device, pointer type, capabilities,
// pressure, tilt, buttons, timestamp, rate) and scribbles raw polylines.
// Run by hand on PADD (GNOME) and shuttlepod-1 (Hyprland); results go to the
// session NOTES.md. No ink code depends on this file.
#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointingDevice>
#include <QSplitter>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

class Scribble : public QWidget {
public:
    QPlainTextEdit *log = nullptr;
    Scribble() { setTabletTracking(true); setAttribute(Qt::WA_AcceptTouchEvents); setMinimumSize(600, 400); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        p.setRenderHint(QPainter::Antialiasing);
        for (const QPolygonF &line : m_lines)
            p.drawPolyline(line);
        p.drawPolyline(m_live);
        p.setPen(Qt::red);
        p.drawText(8, 16, QStringLiteral("rate %1 Hz  events %2").arg(m_rateHz, 0, 'f', 0).arg(m_events));
    }
    void tabletEvent(QTabletEvent *e) override
    {
        const QPointingDevice *dev = e->pointingDevice();
        const QPoint cursor = mapFromGlobal(QCursor::pos());
        const QString line = QStringLiteral("%1 dev=%2 type=%3 ptr=%4 caps=0x%5 pos=%6,%7 cursor=%8,%9 p=%10 tilt=%11,%12 btn=%13 btns=0x%14 t=%15")
            .arg(typeName(e->type()), dev ? dev->name() : QStringLiteral("?"))
            .arg(dev ? int(dev->type()) : -1).arg(int(e->pointerType()))
            .arg(dev ? int(dev->capabilities()) : 0, 0, 16)
            .arg(e->position().x(), 0, 'f', 2).arg(e->position().y(), 0, 'f', 2)
            .arg(cursor.x()).arg(cursor.y())
            .arg(e->pressure(), 0, 'f', 3).arg(e->xTilt(), 0, 'f', 1).arg(e->yTilt(), 0, 'f', 1)
            .arg(int(e->button())).arg(int(e->buttons()), 0, 16).arg(e->timestamp());
        emitLine(line);
        rate(e->timestamp());
        if (e->type() == QEvent::TabletPress) { m_live.clear(); m_live << e->position(); }
        else if (e->type() == QEvent::TabletMove && (e->buttons() & Qt::LeftButton)) m_live << e->position();
        else if (e->type() == QEvent::TabletRelease) { m_lines << m_live; m_live.clear(); }
        e->accept();
        update();
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchUpdate || e->type() == QEvent::TouchEnd) {
            auto *t = static_cast<QTouchEvent *>(e);
            emitLine(QStringLiteral("%1 points=%2").arg(typeName(e->type())).arg(t->points().size()));
            e->accept();
            return true;
        }
        return QWidget::event(e);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        emitLine(QStringLiteral("MousePress source=%1 pos=%2,%3 (synthesised from tablet: %4)")
                     .arg(int(e->source())).arg(e->position().x()).arg(e->position().y())
                     .arg(e->source() == Qt::MouseEventSynthesizedByQt ? "yes" : "no"));
    }

private:
    static QString typeName(QEvent::Type t)
    {
        switch (t) {
        case QEvent::TabletPress: return QStringLiteral("TabletPress");
        case QEvent::TabletMove: return QStringLiteral("TabletMove");
        case QEvent::TabletRelease: return QStringLiteral("TabletRelease");
        case QEvent::TouchBegin: return QStringLiteral("TouchBegin");
        case QEvent::TouchUpdate: return QStringLiteral("TouchUpdate");
        case QEvent::TouchEnd: return QStringLiteral("TouchEnd");
        default: return QString::number(int(t));
        }
    }
    void emitLine(const QString &line)
    {
        std::fprintf(stderr, "%s\n", qPrintable(line));
        if (log) log->appendPlainText(line);
    }
    void rate(quint64 timestampMs)
    {
        ++m_events;
        if (m_windowStart == 0) m_windowStart = timestampMs;
        ++m_windowEvents;
        if (timestampMs - m_windowStart >= 1000) {
            m_rateHz = double(m_windowEvents) * 1000.0 / double(timestampMs - m_windowStart);
            m_windowStart = timestampMs;
            m_windowEvents = 0;
        }
    }
    QList<QPolygonF> m_lines;
    QPolygonF m_live;
    quint64 m_windowStart = 0;
    int m_windowEvents = 0;
    int m_events = 0;
    double m_rateHz = 0;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QSplitter window;
    auto *scribble = new Scribble;
    auto *log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setMaximumBlockCount(2000);
    scribble->log = log;
    window.addWidget(scribble);
    window.addWidget(log);
    window.setWindowTitle(QStringLiteral("inkspike - devicePixelRatio %1").arg(scribble->devicePixelRatioF()));
    window.resize(1200, 700);
    window.show();
    return app.exec();
}
