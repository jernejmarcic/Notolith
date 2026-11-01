#pragma once

#include <memory>
#include <vector>

#include <QPointF>
#include <Qt>

#include "InkTypes.h"

namespace noteAppC::ink {

// Pen input test results (tools/inkspike on a FydeTab Duo). Change only from a
// new measurement, never the names or shapes. The FydeTab Duo's pen has a tip only
// and its buttons do not work, so the held buttons are a bonus: every tool is also on the toolbar and on a key.
inline constexpr Qt::MouseButton kHeldEraserButton = Qt::MiddleButton; ///< BTN_STYLUS
inline constexpr Qt::MouseButton kHeldLassoButton = Qt::RightButton;   ///< BTN_STYLUS2
inline constexpr bool kTrustEraserPointerType = true;                  ///< no eraser end seen on the FydeTab Duo; harmless where one exists
inline constexpr double kInputRateHz = 120.0;                          ///< FydeTab Duo in contact; feeds only the modeler's min_output_rate
inline constexpr QPointF kFractionalScaleOffset{0.0, 0.0};             ///< device pixel ratio 2 showed no offset; subtracted from tablet widget positions by InkCanvas

/// The longest gap between two samples the modeler is shown, in seconds. It refuses a gap that needs
/// more than max_outputs_per_call (200) outputs at min_output_rate (120): about 1.67 s. A pen resting
/// still on the page needs no interpolation for the idle time, so InkInput cuts the excess out of the
/// clock it feeds the modeler (the stroke keeps no times; the real ones only order the samples).
inline constexpr double kMaxModelerGapS = 1.0;

struct InkSample {
    QPointF position; ///< document px
    double pressure = 0.5; ///< 0..1; anything else is clamped, a negative "unknown" becomes 0.5
    double timeSeconds = 0; ///< QInputEvent::timestamp() / 1000.0
};

/// (A FydeTab Duo's pen: pressure never exceeds 0.657, median 0.21.) Divides raw
/// pressure by a per-device learned maximum, so a pen that tops out low still
/// draws full-width lines. The canvas keeps one per device name.
/// - Starts at 1.0: no change.
/// - After kCalibrationSamples samples above kStrongFraction of the current
///   maximum it takes the peak seen as the maximum. That can LOWER it from the
///   1.0 default, which is the point.
/// - From then on it only rises, when a harder press exceeds it.
/// - Unknown pressure (negative, NaN) passes through; InkInput makes it 0.5.
class PressureNormaliser {
public:
    static constexpr int kCalibrationSamples = 200;
    static constexpr double kStrongFraction = 0.3;
    static constexpr double kMinimumMax = 0.1; ///< a broken device reporting ~0 must not scale everything up to 1

    double normalise(double raw); ///< min(1, raw / learnedMax()); learns as a side effect
    double learnedMax() const { return m_learnedMax; }
    /// A stored value (setting ink.pressureByDevice). Below 1.0 it counts as
    /// already calibrated; it is not reported by takeChanged().
    void setLearnedMax(double max);
    bool takeChanged(); ///< true once after learning changed the maximum: the canvas persists it

private:
    double m_learnedMax = 1.0;
    double m_peak = 0.0;
    int m_strongSamples = 0;
    bool m_calibrated = false;
    bool m_changed = false;
};

/// One stroke's samples through ink-stroke-modeler (spec section 7, Input):
/// wobble smoothing, the spring position model and a predicted tail, with
/// the tuned parameters of InkInput.cpp. Raw mode passes samples straight through.
class InkInput {
public:
    InkInput();
    ~InkInput();
    InkInput(const InkInput &) = delete;
    InkInput &operator=(const InkInput &) = delete;

    void setRawInput(bool raw) { m_raw = raw; }
    void begin(const InkSample &sample, const StrokeStyle &style);
    void extend(const InkSample &sample);
    void end(const InkSample &sample);
    bool active() const { return m_active; }

    const Stroke &stroke() const { return m_stroke; }
    const std::vector<StrokePoint> &prediction() const { return m_prediction; }
    size_t newPointsSinceLastTake() const { return m_newPoints; }
    size_t takeNewPoints();
    std::shared_ptr<const Stroke> finish(); ///< nullptr while active, or when there is no stroke (a pen-down that was not a number)
    int droppedSamples() const { return m_dropped; } ///< strictly backwards or non-finite samples skipped (a duplicate of the pen-down instant too)
    /// Times the modeler refused a sample and was restarted from it (or, on a pen-up, the raw point
    /// kept); a prediction that failed. Zero in normal use: the tests watch it.
    int modelerFailures() const { return m_failures; }

private:
    struct Modeler; // hides the ink_stroke_modeler headers from the canvas
    void feed(const InkSample &sample, int eventType);
    void flushPending();
    void restartModeler(const InkSample &sample, double modelTime);
    static bool usable(const InkSample &sample);
    void appendPoint(const QPointF &position, double pressure);
    static double cleanPressure(double pressure);

    std::unique_ptr<Modeler> m_modeler;
    Stroke m_stroke;
    std::vector<StrokePoint> m_prediction;
    size_t m_newPoints = 0;
    int m_dropped = 0;
    double m_lastTime = -1;   ///< the newest accepted sample's time, fed or pending
    InkSample m_last;         ///< the newest accepted sample: a pen-up that is not a number ends the stroke here
    InkSample m_pending;      ///< the newest move, held until the next sample shows it was not replaced
    bool m_hasPending = false;
    double m_timeOffset = 0;    ///< seconds of idle time cut out of the clock the modeler sees (kMaxModelerGapS)
    double m_lastModelTime = 0; ///< the modeler's clock at the last sample fed to it
    int m_failures = 0;
    bool m_degraded = false;    ///< the modeler could not be (re)started: the rest of this stroke is raw
    bool m_active = false;
    bool m_raw = false;
};

} // namespace noteAppC::ink
