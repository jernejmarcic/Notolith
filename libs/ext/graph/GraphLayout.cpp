#include "GraphLayout.h"

#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

namespace noteAppC {

void ForceLayout::reset(int nodeCount, const QList<std::pair<int, int>> &edges)
{
    nodeCount = std::max(0, nodeCount);
    m_position.fill(QPointF(0, 0), nodeCount);
    m_velocity.fill(QPointF(0, 0), nodeCount);
    m_force.fill(QPointF(0, 0), nodeCount);
    m_pinned.fill(false, nodeCount);
    m_edges = QVector<std::pair<int, int>>(edges.begin(), edges.end());

    // A disc rather than a grid or all-at-the-origin: a grid biases the first
    // few steps toward its axes, and stacking everyone at (0,0) makes the
    // very first repulsion step divide by ~0 for every pair. Seeded, so the
    // same graph lays out the same way every time: a global generator made
    // the settling test pass or fail by the draw.
    QRandomGenerator seeded(quint32(nodeCount) * 2654435761u + 1);
    QRandomGenerator *rng = &seeded;
    const double spread = std::sqrt(double(std::max(1, nodeCount))) * 40.0;
    for (int i = 0; i < nodeCount; ++i) {
        const double angle = rng->generateDouble() * 2.0 * M_PI;
        const double radius = std::sqrt(rng->generateDouble()) * spread;
        m_position[i] = QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
    }

    m_tree.clear();
    m_tree.reserve(nodeCount * 3 + 4);
    m_settledStreak = 0;
    m_alpha = 1.0;
}

void ForceLayout::setPosition(int index, QPointF position)
{
    if (index >= 0 && index < m_position.size())
        m_position[index] = position;
}

void ForceLayout::setPinned(int index, bool pinned)
{
    if (index >= 0 && index < m_pinned.size())
        m_pinned[index] = pinned;
}

int ForceLayout::newNode(double cx, double cy, double halfSize)
{
    QuadNode node;
    node.cx = cx;
    node.cy = cy;
    node.halfSize = halfSize;
    m_tree.append(node);
    return m_tree.size() - 1;
}

void ForceLayout::subdivide(int nodeIndex)
{
    // Read bounds before appending: append() may reallocate m_tree, which
    // would invalidate a reference held across it, so nothing here keeps one
    // - every access below re-indexes m_tree fresh.
    const double cx = m_tree[nodeIndex].cx;
    const double cy = m_tree[nodeIndex].cy;
    const double half = m_tree[nodeIndex].halfSize / 2.0;

    const int first = newNode(cx - half, cy - half, half);
    newNode(cx + half, cy - half, half);
    newNode(cx - half, cy + half, half);
    newNode(cx + half, cy + half, half);
    m_tree[nodeIndex].firstChild = first;
}

int ForceLayout::childIndexFor(int nodeIndex, QPointF point) const
{
    const QuadNode &node = m_tree[nodeIndex];
    const int right = point.x() >= node.cx ? 1 : 0;
    const int bottom = point.y() >= node.cy ? 1 : 0;
    return node.firstChild + bottom * 2 + right;
}

void ForceLayout::insert(int nodeIndex, int bodyIndex, int depth)
{
    // The aggregate is updated on the way down for every node this body
    // passes through, whether it ends up a leaf or turns internal, since
    // accumulateForce() reads mass/comX/comY the same way for both.
    m_tree[nodeIndex].mass += 1.0;
    m_tree[nodeIndex].comX += m_position[bodyIndex].x();
    m_tree[nodeIndex].comY += m_position[bodyIndex].y();

    if (m_tree[nodeIndex].firstChild == -1 && m_tree[nodeIndex].body == -1) {
        m_tree[nodeIndex].body = bodyIndex; // empty leaf: this body lands here
        return;
    }

    if (m_tree[nodeIndex].firstChild == -1) {
        // A leaf already holding one body. Below kMinCell - or once depth
        // has run away, which is the only thing that actually bounds this
        // for a non-finite halfSize, see kMaxTreeDepth's comment - stop
        // subdividing and let the leaf hold both. Two notes effectively on
        // top of each other then repel everything else as one combined
        // mass, which is a harmless approximation nobody will see at that
        // resolution.
        //
        // Written as !(halfSize >= kMinCell) rather than halfSize < kMinCell
        // on purpose: the two are not the same when halfSize is NaN. `<`
        // against NaN is always false, so that spelling would silently
        // never stop; `>=` is also always false, but negating it flips that
        // into the safe "stop" answer instead.
        if (depth >= kMaxTreeDepth || !(m_tree[nodeIndex].halfSize >= kMinCell))
            return;

        const int existingBody = m_tree[nodeIndex].body;
        m_tree[nodeIndex].body = -1;
        subdivide(nodeIndex);
        insert(childIndexFor(nodeIndex, m_position[existingBody]), existingBody, depth + 1);
        insert(childIndexFor(nodeIndex, m_position[bodyIndex]), bodyIndex, depth + 1);
        return;
    }

    insert(childIndexFor(nodeIndex, m_position[bodyIndex]), bodyIndex, depth + 1);
}

void ForceLayout::buildTree()
{
    m_tree.clear();
    const int n = m_position.size();
    if (n == 0)
        return;

    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    bool haveBounds = false;
    for (const QPointF &p : std::as_const(m_position)) {
        // A non-finite position should not happen any more (step() clamps
        // speed and sanitises the result below), but the bounding box is
        // cheap insurance: one bad coordinate here would otherwise poison
        // every subsequent halfSize with inf or NaN, which is exactly the
        // failure this file's other comments describe.
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
            continue;
        if (!haveBounds) {
            minX = maxX = p.x();
            minY = maxY = p.y();
            haveBounds = true;
            continue;
        }
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }
    if (!haveBounds) // every position was non-finite - nothing sane to build
        return;

    const double cx = (minX + maxX) / 2.0;
    const double cy = (minY + maxY) / 2.0;
    // Square, with a margin so a body sitting exactly on the boundary still
    // has room, and a 1.0 floor so an all-coincident graph does not produce
    // a zero-size root.
    const double half = std::max({(maxX - minX) / 2.0, (maxY - minY) / 2.0, 1.0}) * 1.05;

    newNode(cx, cy, half);
    for (int i = 0; i < n; ++i) {
        if (std::isfinite(m_position[i].x()) && std::isfinite(m_position[i].y()))
            insert(0, i, 0);
    }
}

void ForceLayout::accumulateForce(int nodeIndex, int bodyIndex, double &fx, double &fy) const
{
    const QuadNode &node = m_tree[nodeIndex];
    if (node.mass <= 0.0)
        return;

    const bool isLeaf = node.firstChild == -1;
    if (isLeaf && node.body == bodyIndex)
        return; // a body does not repel itself

    const double comX = node.comX / node.mass;
    const double comY = node.comY / node.mass;
    const double dx = m_position[bodyIndex].x() - comX;
    const double dy = m_position[bodyIndex].y() - comY;
    double distSq = dx * dx + dy * dy;

    // A leaf is always resolved directly - it is either exactly one body or
    // an unsplittable cluster (see insert()), never an approximation that
    // opening it further would improve. An internal node only stands in for
    // its children once it is far enough away, the classic Barnes-Hut s/d
    // opening-angle test.
    const bool resolveDirectly =
        isLeaf || (node.halfSize * 2.0) / std::sqrt(std::max(distSq, 1e-12)) < m_options.theta;
    if (resolveDirectly) {
        distSq = std::max(distSq, kSoftening);
        const double dist = std::sqrt(distSq);
        // d3-force's many-body law, the engine Obsidian's graph runs on:
        // strength / d, not / d^2. At 10 / d^2 a leaf 150 px from its
        // neighbour felt 0.0004 against springs in the hundreds, so a local
        // graph's leaves never spread before the layout cooled and sat
        // stacked on one spot.
        const double force = m_options.repelStrength * kRepelScale * node.mass / dist;
        fx += dx / dist * force;
        fy += dy / dist * force;
        return;
    }

    for (int c = 0; c < 4; ++c)
        accumulateForce(node.firstChild + c, bodyIndex, fx, fy);
}

double ForceLayout::step()
{
    const int n = m_position.size();
    if (n == 0)
        return 0.0;

    buildTree();

    for (int i = 0; i < n; ++i) {
        double fx = 0, fy = 0;
        accumulateForce(0, i, fx, fy);
        // Centre force: a spring to the origin rather than a constant pull,
        // so it stays weak near the middle and never overwhelms repulsion
        // for a node that has drifted far out - matches Obsidian's slider,
        // which behaves the same way.
        fx -= m_position[i].x() * m_options.centerStrength * kCenterScale;
        fy -= m_position[i].y() * m_options.centerStrength * kCenterScale;
        m_force[i] = QPointF(fx, fy);
    }

    for (const auto &edge : std::as_const(m_edges)) {
        const QPointF delta = m_position[edge.second] - m_position[edge.first];
        const double dist = std::max(1e-6, std::hypot(delta.x(), delta.y()));
        const double magnitude = m_options.linkStrength * (dist - m_options.linkDistance) / dist;
        const QPointF pull = delta * magnitude;
        m_force[edge.first] += pull;
        m_force[edge.second] -= pull;
    }

    // Fruchterman-Reingold's classic move: cap how far a node may travel in
    // one step, independent of how large the force behind it was. Repulsion
    // between two nodes that end up close (a dense initial scatter, or a
    // spring pulling two hubs together) is a stiff 1/d^2 potential, and
    // plain Euler integration can overshoot such a potential into a bigger
    // separation next step than it started with - then an even bigger
    // force, then an even bigger overshoot. Uncapped, that compounds every
    // step: a real 5000-node run measured here went from a starting energy
    // already in the tens of millions to literal floating-point infinity
    // within 200 steps, and once a coordinate is infinite, halving it in
    // the quadtree never makes it smaller (inf / 2 == inf), so buildTree()
    // stopped terminating and the process was OOM-killed. The cap keeps
    // every step's displacement bounded no matter what the force computed
    // to, which is what actually stops the runaway - kSoftening (raised
    // from 1e-6 to 1.0) then keeps that worst case from being reached often
    // in the first place, so this is a backstop, not the primary fix.
    const double maxSpeed = m_options.linkDistance * kMaxSpeedFactor;
    double totalEnergy = 0.0;
    for (int i = 0; i < n; ++i) {
        if (m_pinned[i]) {
            m_velocity[i] = QPointF(0, 0);
            continue;
        }
        QPointF velocity = (m_velocity[i] + m_force[i] * m_alpha * m_options.timeStep) * m_options.damping;
        const double speed = std::hypot(velocity.x(), velocity.y());
        if (speed > maxSpeed)
            velocity *= maxSpeed / speed;

        QPointF position = m_position[i] + velocity * m_options.timeStep;
        if (!std::isfinite(position.x()) || !std::isfinite(position.y())
            || !std::isfinite(velocity.x()) || !std::isfinite(velocity.y())) {
            // Should be unreachable with the cap above, but a node that got
            // here any other way (a future option letting linkDistance or
            // repelStrength go extreme, say) is better teleported back to
            // the centre than left to poison every step after it.
            velocity = QPointF(0, 0);
            position = QPointF(0, 0);
        }

        m_velocity[i] = velocity;
        m_position[i] = position;
        totalEnergy += QPointF::dotProduct(velocity, velocity);
    }

    m_alpha = std::max(kMinAlpha, m_alpha * kAlphaDecay);

    const double meanEnergy = totalEnergy / n;
    if (meanEnergy < kSettleEnergy)
        ++m_settledStreak;
    else
        m_settledStreak = 0;
    return meanEnergy;
}

} // namespace noteAppC
