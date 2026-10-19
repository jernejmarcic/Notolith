#pragma once

#include <QList>
#include <QPointF>
#include <QVector>

#include <algorithm>
#include <utility>

namespace noteAppC {

/// Force-directed layout: Barnes-Hut quadtree repulsion (O(n log n) per step)
/// plus spring edges and a weak pull to the centre. Knows nothing about
/// notes, painting or widgets - GraphView drives it with a QTimer and reads
/// positions back out, which is also what makes it testable on its own.
///
/// Stepped rather than solved in one shot: a 5 000-node graph never needs to
/// finish before the first frame, and it settles into stillness on its own,
/// at which point the caller stops paying for it (see isSettled()).
class ForceLayout {
public:
    /// Field names match Obsidian's `graph.json` sliders (GraphOptions in
    /// GraphView.h maps straight onto these four); theta/damping/timeStep are
    /// this engine's own tuning and are not exposed to the user.
    struct Options {
        double repelStrength = 10.0;
        double linkStrength = 1.0;
        double linkDistance = 250.0;
        double centerStrength = 0.5;
        double theta = 0.85;   ///< Barnes-Hut opening angle: smaller = more exact, slower
        double damping = 0.6; ///< d3's velocityDecay 0.4: 60% of the velocity survives a step
        double timeStep = 1.0;
        bool operator==(const Options &) const = default;
    };

    /// Rebuilds for `nodeCount` bodies, scattered in a disc so two runs do
    /// not start stacked on top of each other, with `edges` as (from, to)
    /// indices into that range.
    /// `alpha` is the starting temperature: 1 for a graph laid out from
    /// scratch, lower when most nodes keep their place (see wake()).
    void reset(int nodeCount, const QList<std::pair<int, int>> &edges, double alpha = 1.0);

    void setOptions(const Options &options) { m_options = options; }
    const Options &options() const { return m_options; }

    int count() const { return m_position.size(); }
    QPointF position(int index) const { return m_position.value(index); }
    /// Every position at once - what GraphView snapshots after a step so the
    /// UI thread never reads the layout while a worker is stepping it.
    const QVector<QPointF> &positions() const { return m_position; }
    void setPosition(int index, QPointF position);
    void setPinned(int index, bool pinned);
    bool isPinned(int index) const { return m_pinned.value(index, false); }

    /// One Euler step. Returns the mean kinetic energy per node, which is
    /// what settling is judged against - a caller that wants its own
    /// convergence signal can watch this fall instead of polling isSettled().
    double step();

    /// True once recent steps have all reported energy under the settle
    /// threshold - a short streak rather than one step, so a single noisy
    /// step does not stop-then-restart the timer.
    /// Still, or cooled to the floor with nothing holding it warm (d3 stops
    /// at alphaMin too): a large graph under extreme sliders can stay above
    /// the energy threshold, and stepping on at the floor turns it slowly.
    bool isSettled() const
    {
        return m_settledStreak >= kSettleStreak || (m_alphaTarget <= 0.0 && m_alpha <= kMinAlpha);
    }

    /// Un-settles the layout: call when a node is dragged or the data
    /// changes, so motion resumes without a full reset() (which would also
    /// scatter every position). Also reheats the cooling schedule (see
    /// m_alpha) so a drag on an otherwise-cooled graph still has real force
    /// behind it instead of the near-zero pull a fully cooled step would
    /// apply.
    /// Reheats only to d3's 0.3, never back to 1: a full reheat sends every
    /// node flying again, which is the "jiggly graph". It never cools a
    /// layout that is hotter already.
    void wake(double alpha = 0.3)
    {
        m_settledStreak = 0;
        m_alpha = std::max(m_alpha, alpha);
    }
    /// The temperature alpha cools toward (d3's alphaTarget): 0.3 while a
    /// node is held, so the rest of the graph keeps flowing around it, 0 after.
    void setAlphaTarget(double target) { m_alphaTarget = target; }
    double alpha() const { return m_alpha; }

private:
    struct QuadNode {
        double cx = 0, cy = 0, halfSize = 0; // bounds: a square centred at (cx, cy)
        double mass = 0;
        double comX = 0, comY = 0; // sum of positions beneath this node; divide by mass for the centre of mass
        int body = -1;             // >= 0: the one body this leaf holds directly
        int firstChild = -1;       // >= 0: index of 4 consecutive children; -1 means this is a leaf
    };

    void buildTree();
    int newNode(double cx, double cy, double halfSize);
    void subdivide(int nodeIndex);
    int childIndexFor(int nodeIndex, QPointF point) const;
    void insert(int nodeIndex, int bodyIndex, int depth);
    void accumulateForce(int nodeIndex, int bodyIndex, double &fx, double &fy) const;

    Options m_options;
    QVector<QPointF> m_position;
    QVector<QPointF> m_velocity;
    QVector<QPointF> m_force; // scratch buffer, reused every step so step() never allocates
    QVector<bool> m_pinned;
    QVector<std::pair<int, int>> m_edges;
    QVector<QuadNode> m_tree;
    int m_settledStreak = 0;
    // Cooling schedule (the same idea as D3's forceSimulation alpha, or
    // simulated annealing's temperature): every practical force-directed
    // layout needs one, because a graph dense enough for its bulk repulsion
    // to keep outrunning damping - which the postmortem in kMaxTreeDepth's
    // comment measured happening from step 0 on a 5000-node graph - would
    // otherwise oscillate at the speed cap forever rather than settle.
    // Scaling the force by a factor that decays toward ~0 over a few
    // hundred steps guarantees convergence regardless of how strong
    // repulsion is relative to damping; the speed cap in step() is what
    // keeps the hot early steps, where alpha is still ~1, from diverging.
    double m_alpha = 1.0;
    double m_alphaTarget = 0.0;
    QVector<int> m_degree;
    static constexpr double kAlphaDecay = 0.977; // ~0.001 by step 300
    static constexpr double kMinAlpha = 0.001;   // never fully zero - wake() still has something to push with

    // A cell this small is treated as one point rather than subdivided
    // further - true Barnes-Hut would recurse forever for two coincident
    // bodies, and nothing is gained by resolving positions closer than this.
    static constexpr double kMinCell = 1e-3;
    // Postmortem (2026-09-23, see GraphLayout.cpp step()): a halfSize check
    // alone is not a safe recursion bound. Two things defeated it in
    // practice - (1) `x < kMinCell` is false for a NaN x, so it silently
    // never fires; (2) once a coordinate overflows to +-inf, halving it
    // forever produces inf again (IEEE 754: inf / 2 == inf), so the size
    // never crosses the threshold at all. Both let insert() recurse without
    // bound, each level appending 4 more QuadNode - not a stack overflow
    // (the tail call was optimised into a loop), a slow, silent multi-GB
    // heap climb until the OOM killer stepped in. A hard depth cap bounds
    // the recursion unconditionally, independent of what the arithmetic
    // does; kMinCell now also uses a NaN-safe comparison as a second layer.
    static constexpr int kMaxTreeDepth = 48;
    // Repulsion's softening floor on distance^2. This used to be 1e-6, which
    // does essentially nothing at the scale positions live on (tens to
    // thousands of units): two clustered nodes half a unit apart still got
    // their true, enormous 1/d^2 force. That force, applied every step under
    // plain Euler integration with nothing capping the resulting velocity,
    // compounded geometrically - see the note above. 1.0 keeps repulsion
    // finite for any encounter closer than one unit, which is the regime a
    // real layout actually passes through while settling.
    static constexpr double kSoftening = 1.0;
    // A hard ceiling on how far a node may move in one step, independent of
    // how large the computed force was - the classic Fruchterman-Reingold
    // "temperature" cap. Scaled to linkDistance so it bounds the worst early
    // steps (a dense initial scatter) without limiting normal settling once
    // forces are back in a sane range.
    static constexpr double kMaxSpeedFactor = 4.0;
    // Obsidian's slider values (repel 10, centre 0.5) to force units: d3's
    // default charge is 30 per node, and its forceX/forceY default is 0.1.
    // A centre spring at the slider's raw 0.5 outweighed springs 250 px long
    // and pulled every leaf of a star onto one side.
    static constexpr double kRepelScale = 30.0;
    static constexpr double kCenterScale = 0.1;
    static constexpr double kSettleEnergy = 0.02;
    static constexpr int kSettleStreak = 8;
};

} // namespace noteAppC
