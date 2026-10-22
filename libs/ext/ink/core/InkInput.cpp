#include "InkInput.h"

#include <algorithm>
#include <cmath>

#include <ink_stroke_modeler/params.h>
#include <ink_stroke_modeler/stroke_modeler.h>
#include <ink_stroke_modeler/types.h>

namespace noteAppC::ink {

// The pinned modeler's types live in ::ink::stroke_model (not ::ink): write it in full, or ink
// resolves to our own namespace.
struct InkInput::Modeler {
    ::ink::stroke_model::StrokeModeler modeler;
    std::vector<::ink::stroke_model::Result> results;
};

namespace {

// Rnote's parameters (rnote-engine/src/pens/pensconfig/brushconfig.rs and
// rnote-study.md section 1.4), in seconds and document px. The wobble
// timeout is Rnote's 0.04 s preset, not derived from kInputRateHz (which only
// sets min_output_rate). The upstream
// defaults for the position modeler stay. `stylus_state_modeler_params.
// max_input_samples`, which Rnote overrides, does not exist at the pinned
// modeler commit; use_stroke_normal_projection is the current knob.
::ink::stroke_model::StrokeModelParams rnoteParams()
{
    ::ink::stroke_model::StrokeModelParams params;
    params.wobble_smoother_params.is_enabled = true;
    params.wobble_smoother_params.timeout = ::ink::stroke_model::Duration(0.04);
    params.wobble_smoother_params.speed_floor = 1.31;
    params.wobble_smoother_params.speed_ceiling = 1.44;
    params.sampling_params.min_output_rate = std::max(120.0, kInputRateHz);
    params.sampling_params.end_of_stroke_stopping_distance = 0.01;
    params.sampling_params.end_of_stroke_max_iterations = 20;
    params.sampling_params.max_outputs_per_call = 200;
    params.stylus_state_modeler_params.use_stroke_normal_projection = true;
    params.prediction_params = ::ink::stroke_model::StrokeEndPredictorParams{};
    return params;
}

} // namespace

double PressureNormaliser::normalise(double raw)
{
    if (std::isnan(raw) || raw < 0.0)
        return raw; // unknown: left for InkInput::cleanPressure
    m_peak = std::max(m_peak, raw);
    if (raw > kStrongFraction * m_learnedMax)
        ++m_strongSamples;
    if (!m_calibrated && m_strongSamples >= kCalibrationSamples) {
        m_learnedMax = std::max(kMinimumMax, m_peak);
        m_calibrated = true;
        m_changed = true;
    } else if (m_calibrated && m_peak > m_learnedMax) {
        m_learnedMax = m_peak; // a harder press than ever: raise, never lower
        m_changed = true;
    }
    return std::min(1.0, raw / m_learnedMax);
}

void PressureNormaliser::setLearnedMax(double max)
{
    m_learnedMax = std::max(kMinimumMax, max);
    m_calibrated = max < 1.0; // 1.0 is the default, not a learned value: such a device still calibrates
    m_peak = m_calibrated ? m_learnedMax : 0.0;
    m_strongSamples = 0;
}

bool PressureNormaliser::takeChanged()
{
    const bool changed = m_changed;
    m_changed = false;
    return changed;
}

InkInput::InkInput() : m_modeler(std::make_unique<Modeler>()) {}
InkInput::~InkInput() = default;

double InkInput::cleanPressure(double pressure)
{
    if (std::isnan(pressure) || pressure < 0.0)
        return 0.5; // "unknown" from a device without pressure: a mid-width line, not a hairline
    return std::min(pressure, 1.0);
}

void InkInput::appendPoint(const QPointF &position, double pressure)
{
    m_stroke.points.push_back({position.x(), position.y(), cleanPressure(pressure)});
    ++m_newPoints;
}

// The input boundary: the modeler may assert on NaN or infinity, and the file writer would
// turn it into null. Positions are checked as floats, the modeler's type: 1e300 is finite as
// a double and infinite as a float.
bool InkInput::usable(const InkSample &sample)
{
    return std::isfinite(float(sample.position.x())) && std::isfinite(float(sample.position.y())) && std::isfinite(sample.timeSeconds);
}

void InkInput::begin(const InkSample &sample, const StrokeStyle &style)
{
    m_stroke = Stroke{};
    m_stroke.style = style;
    m_active = false;
    m_prediction.clear();
    m_newPoints = 0;
    m_dropped = 0;
    m_lastTime = -1;
    m_hasPending = false;
    if (!usable(sample)) {
        ++m_dropped;
        return; // nothing started: finish() has no stroke to hand out
    }
    m_active = true;
    m_last = sample;
    if (!m_raw) {
        // Reset returns a status only for bad params; ours are constants.
        (void)m_modeler->modeler.Reset(rnoteParams());
    }
    feed(sample, 0);
}

// A move is held back until the next sample arrives (one event, about 5 ms at
// 200 Hz, covered by the predicted tail): a sample that shares its
// predecessor's timestamp - a compositor batching events - must replace it,
// and the modeler cannot take a sample back. A strictly backwards timestamp is
// skipped: the modeler rejects time going backwards.
// ponytail: one event of latency on every move; feed at once and drop the newer duplicate if the bench says 5 ms matters.
void InkInput::extend(const InkSample &sample)
{
    if (!m_active)
        return;
    if (!usable(sample) || sample.timeSeconds < m_lastTime) {
        ++m_dropped;
        return;
    }
    if (sample.timeSeconds == m_lastTime) {
        if (m_hasPending) {
            m_pending = sample; // the newer replaces the older
            m_last = sample;
        } else
            ++m_dropped;        // the same instant as the pen-down sample, which is already in the stroke
        return;
    }
    flushPending();
    m_pending = sample;
    m_hasPending = true;
    m_lastTime = sample.timeSeconds;
    m_last = sample;
}

void InkInput::end(const InkSample &sample)
{
    if (!m_active)
        return;
    InkSample up = sample;
    if (!usable(up)) {
        ++m_dropped; // a pen-up that is not a number still ends the stroke, where it last was
        up = m_last;
    } else if (up.timeSeconds < m_lastTime) {
        ++m_dropped; // a stale clock on the pen-up: it still ends the stroke, at the last time
        up.timeSeconds = m_lastTime;
    }
    if (m_hasPending && up.timeSeconds == m_pending.timeSeconds)
        m_hasPending = false; // the pen-up replaces a move of the same instant
    else
        flushPending();
    feed(up, 2);
    if (m_stroke.points.empty())
        appendPoint(up.position, up.pressure); // a tap the modeler turned into nothing is still a dot
    m_prediction.clear();
    m_active = false;
}

void InkInput::flushPending()
{
    if (!m_hasPending)
        return;
    m_hasPending = false;
    feed(m_pending, 1);
}

void InkInput::feed(const InkSample &sample, int eventType)
{
    if (m_raw) {
        appendPoint(sample.position, sample.pressure);
        m_lastTime = sample.timeSeconds;
        return;
    }
    ::ink::stroke_model::Input input;
    input.event_type = eventType == 0 ? ::ink::stroke_model::Input::EventType::kDown
                     : eventType == 1 ? ::ink::stroke_model::Input::EventType::kMove
                                      : ::ink::stroke_model::Input::EventType::kUp;
    input.position = {float(sample.position.x()), float(sample.position.y())};
    input.time = ::ink::stroke_model::Time(sample.timeSeconds);
    input.pressure = float(cleanPressure(sample.pressure));
    m_modeler->results.clear();
    const absl::Status status = m_modeler->modeler.Update(input, m_modeler->results);
    if (!status.ok()) {
        // A duplicate kDown, or a sample the modeler cannot place: keep the raw
        // point so the stroke never silently loses ink.
        appendPoint(sample.position, sample.pressure);
        ++m_dropped;
        m_lastTime = sample.timeSeconds;
        return;
    }
    for (const ::ink::stroke_model::Result &r : m_modeler->results)
        if (std::isfinite(r.position.x) && std::isfinite(r.position.y))
            appendPoint(QPointF(r.position.x, r.position.y), r.pressure);
    if (eventType == 0 && m_stroke.points.empty())
        appendPoint(sample.position, sample.pressure); // the modeler emits the down point only with the first move
    m_lastTime = sample.timeSeconds;
    m_prediction.clear();
    if (eventType != 2) {
        m_modeler->results.clear();
        if (m_modeler->modeler.Predict(m_modeler->results).ok())
            for (const ::ink::stroke_model::Result &r : m_modeler->results)
                m_prediction.push_back({double(r.position.x), double(r.position.y), cleanPressure(r.pressure)});
    }
}

size_t InkInput::takeNewPoints()
{
    const size_t n = m_newPoints;
    m_newPoints = 0;
    return n;
}

std::shared_ptr<const Stroke> InkInput::finish()
{
    if (m_active || m_stroke.points.empty())
        return nullptr;
    auto done = std::make_shared<Stroke>(std::move(m_stroke));
    m_stroke = Stroke{};
    m_newPoints = 0;
    return done;
}

} // namespace noteAppC::ink
