// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 The FreeCAD Project Association AISBL               *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <unordered_map>

#include "SectionGeometry.h"

using namespace Gui::Section;

namespace
{
constexpr uint32_t invalidIndex = std::numeric_limits<uint32_t>::max();
}

// ----------------------------------------------------------------------------

CutPlane::CutPlane(const Base::Vector3d& base, const Base::Vector3d& dir)
    : normal(dir)
{
    normal.Normalize();
    distance = normal * base;
}

CutPlane CutPlane::flipped() const
{
    CutPlane plane;
    plane.normal = -normal;
    plane.distance = -distance;
    return plane;
}

void CutPlane::getAxes(Base::Vector3d& uDir, Base::Vector3d& vDir) const
{
    // u x v must point into the removed half-space
    const Base::Vector3d outward = -normal;
    const Base::Vector3d ref = std::abs(outward.z) < 0.9 ? Base::Vector3d(0.0, 0.0, 1.0)
                                                         : Base::Vector3d(1.0, 0.0, 0.0);
    uDir = ref % outward;
    uDir.Normalize();
    vDir = outward % uDir;
    vDir.Normalize();
}

int CutPlane::classify(const Base::BoundBox3d& box) const
{
    if (!box.IsValid()) {
        return -1;
    }
    double minDist = std::numeric_limits<double>::max();
    double maxDist = std::numeric_limits<double>::lowest();
    for (unsigned short i = 0; i < 8; ++i) {
        const double dist = signedDistance(box.CalcPoint(i));
        minDist = std::min(minDist, dist);
        maxDist = std::max(maxDist, dist);
    }
    if (minDist >= 0.0) {
        return 1;
    }
    if (maxDist < 0.0) {
        return -1;
    }
    return 0;
}

// ----------------------------------------------------------------------------

namespace
{
struct CellKey
{
    int64_t x;
    int64_t y;
    int64_t z;

    bool operator==(const CellKey& other) const = default;
};

struct CellKeyHash
{
    std::size_t operator()(const CellKey& key) const noexcept
    {
        auto mix = [](uint64_t value) {
            value ^= value >> 33;
            value *= 0xff51afd7ed558ccdULL;
            value ^= value >> 33;
            return value;
        };
        return static_cast<std::size_t>(
            mix(static_cast<uint64_t>(key.x)) ^ (mix(static_cast<uint64_t>(key.y)) << 1)
            ^ (mix(static_cast<uint64_t>(key.z)) << 2)
        );
    }
};

bool isFinite(const Base::Vector3d& pnt)
{
    return std::isfinite(pnt.x) && std::isfinite(pnt.y) && std::isfinite(pnt.z);
}
}  // namespace

IndexedMesh Gui::Section::weldTriangleSoup(const std::vector<Base::Vector3d>& soup, double tolerance)
{
    IndexedMesh mesh;
    const std::size_t numTriangles = soup.size() / 3;
    if (numTriangles == 0) {
        return mesh;
    }

    tolerance = std::max(tolerance, 1e-12);
    // Cells are much bigger than the tolerance so that most lookups only need to
    // check a single cell. Neighbouring cells are only checked for points close
    // to a cell border.
    const double cellSize = tolerance * 16.0;
    const double invCell = 1.0 / cellSize;
    const double border = tolerance * invCell;
    const double tol2 = tolerance * tolerance;

    std::unordered_map<CellKey, uint32_t, CellKeyHash> cells;
    cells.reserve(soup.size() / 3);
    // Points sharing a cell form a linked list
    std::vector<uint32_t> chain;
    chain.reserve(soup.size() / 3);
    mesh.points.reserve(soup.size() / 3);

    auto findInCell = [&](const CellKey& key, const Base::Vector3d& pnt) {
        auto it = cells.find(key);
        if (it == cells.end()) {
            return invalidIndex;
        }
        for (uint32_t idx = it->second; idx != invalidIndex; idx = chain[idx]) {
            if ((mesh.points[idx] - pnt).Sqr() <= tol2) {
                return idx;
            }
        }
        return invalidIndex;
    };

    auto addPoint = [&](const Base::Vector3d& pnt) {
        const std::array<double, 3> scaled {pnt.x * invCell, pnt.y * invCell, pnt.z * invCell};
        const CellKey key {
            static_cast<int64_t>(std::floor(scaled[0])),
            static_cast<int64_t>(std::floor(scaled[1])),
            static_cast<int64_t>(std::floor(scaled[2]))
        };

        uint32_t found = findInCell(key, pnt);
        if (found != invalidIndex) {
            return found;
        }

        const std::array<int64_t, 3> cell {key.x, key.y, key.z};
        std::array<int, 3> low {};
        std::array<int, 3> high {};
        for (int k = 0; k < 3; ++k) {
            const double frac = scaled[k] - static_cast<double>(cell[k]);
            low[k] = frac < border ? -1 : 0;
            high[k] = frac > 1.0 - border ? 1 : 0;
        }
        for (int dx = low[0]; dx <= high[0]; ++dx) {
            for (int dy = low[1]; dy <= high[1]; ++dy) {
                for (int dz = low[2]; dz <= high[2]; ++dz) {
                    if (dx == 0 && dy == 0 && dz == 0) {
                        continue;
                    }
                    found = findInCell({key.x + dx, key.y + dy, key.z + dz}, pnt);
                    if (found != invalidIndex) {
                        return found;
                    }
                }
            }
        }

        const auto idx = static_cast<uint32_t>(mesh.points.size());
        mesh.points.push_back(pnt);
        mesh.boundBox.Add(pnt);
        auto [it, inserted] = cells.try_emplace(key, idx);
        chain.push_back(inserted ? invalidIndex : it->second);
        it->second = idx;
        return idx;
    };

    mesh.triangles.reserve(numTriangles);
    for (std::size_t i = 0; i < numTriangles; ++i) {
        const Base::Vector3d& p0 = soup[3 * i];
        const Base::Vector3d& p1 = soup[3 * i + 1];
        const Base::Vector3d& p2 = soup[3 * i + 2];
        if (!isFinite(p0) || !isFinite(p1) || !isFinite(p2)) {
            continue;
        }
        const uint32_t a = addPoint(p0);
        const uint32_t b = addPoint(p1);
        const uint32_t c = addPoint(p2);
        if (a == b || b == c || c == a) {
            continue;
        }
        mesh.triangles.push_back({a, b, c});
    }
    mesh.buildEdges();
    return mesh;
}

void IndexedMesh::buildEdges()
{
    edges.clear();
    triangleEdges.clear();
    triangleEdges.reserve(triangles.size());
    edges.reserve(triangles.size() * 3 / 2 + 16);

    std::unordered_map<uint64_t, uint32_t> edgeIndex;
    edgeIndex.reserve(triangles.size() * 2);
    auto edgeOf = [&](uint32_t a, uint32_t b) {
        if (a > b) {
            std::swap(a, b);
        }
        const uint64_t key = (static_cast<uint64_t>(a) << 32) | b;
        auto [it, inserted] = edgeIndex.try_emplace(key, static_cast<uint32_t>(edges.size()));
        if (inserted) {
            edges.push_back({a, b});
        }
        return it->second;
    };
    for (const auto& tri : triangles) {
        triangleEdges.push_back(
            {edgeOf(tri[0], tri[1]), edgeOf(tri[1], tri[2]), edgeOf(tri[2], tri[0])}
        );
    }
}

double Gui::Section::defaultWeldTolerance(const Base::BoundBox3d& box)
{
    if (!box.IsValid()) {
        return 1e-9;
    }
    // The tessellation is rendered with single precision coordinates, so the
    // tolerance must be well above the float resolution at the model's scale.
    const double extent = std::max(
        {box.CalcDiagonalLength(),
         std::abs(box.MinX),
         std::abs(box.MaxX),
         std::abs(box.MinY),
         std::abs(box.MaxY),
         std::abs(box.MinZ),
         std::abs(box.MaxZ)}
    );
    return std::max(extent * 1e-6, 1e-9);
}

// ----------------------------------------------------------------------------

namespace
{
/// Splits a node sequence into simple loops wherever a node repeats
void splitAtRepeats(
    const std::vector<uint32_t>& sequence,
    bool closed,
    std::vector<std::vector<uint32_t>>& loops,
    std::vector<std::vector<uint32_t>>& openChains
)
{
    std::vector<uint32_t> stack;
    std::unordered_map<uint32_t, std::size_t> position;
    stack.reserve(sequence.size());

    for (uint32_t node : sequence) {
        auto it = position.find(node);
        if (it != position.end()) {
            const std::size_t first = it->second;
            std::vector<uint32_t> loop(stack.begin() + static_cast<std::ptrdiff_t>(first), stack.end());
            for (std::size_t i = first + 1; i < stack.size(); ++i) {
                position.erase(stack[i]);
            }
            stack.resize(first + 1);
            if (loop.size() >= 3) {
                loops.push_back(std::move(loop));
            }
            continue;
        }
        position.emplace(node, stack.size());
        stack.push_back(node);
    }

    if (closed) {
        if (stack.size() >= 3) {
            loops.push_back(std::move(stack));
        }
    }
    else if (stack.size() >= 2) {
        openChains.push_back(std::move(stack));
    }
}

void removeDuplicatePoints(Polyline& line, double tolerance)
{
    const double tol2 = tolerance * tolerance;
    auto& pts = line.points;
    if (pts.empty()) {
        return;
    }
    std::vector<Base::Vector3d> result;
    result.reserve(pts.size());
    for (const auto& pnt : pts) {
        if (result.empty() || (result.back() - pnt).Sqr() > tol2) {
            result.push_back(pnt);
        }
    }
    if (line.closed) {
        while (result.size() > 1 && (result.back() - result.front()).Sqr() <= tol2) {
            result.pop_back();
        }
    }
    pts = std::move(result);
}

/// Joins open polylines whose ends are within tolerance
void closeGaps(std::vector<Polyline>& open, std::vector<Polyline>& closedOut, double tolerance)
{
    const double tol2 = tolerance * tolerance;
    // Quadratic in the number of open contours, which is normally tiny.
    constexpr std::size_t maxOpen = 2000;
    if (open.size() > maxOpen) {
        return;
    }

    bool merged = true;
    while (merged) {
        merged = false;
        for (std::size_t i = 0; i < open.size(); ++i) {
            auto& line = open[i].points;
            if (line.size() >= 3 && (line.front() - line.back()).Sqr() <= tol2) {
                open[i].closed = true;
                closedOut.push_back(std::move(open[i]));
                open.erase(open.begin() + static_cast<std::ptrdiff_t>(i));
                merged = true;
                break;
            }
        }
        if (merged) {
            continue;
        }

        double best = tol2;
        std::size_t bestI = 0;
        std::size_t bestJ = 0;
        int bestCase = -1;
        for (std::size_t i = 0; i < open.size(); ++i) {
            const auto& a = open[i].points;
            for (std::size_t j = i + 1; j < open.size(); ++j) {
                const auto& b = open[j].points;
                const std::array<double, 4> dist {
                    (a.back() - b.front()).Sqr(),
                    (a.back() - b.back()).Sqr(),
                    (a.front() - b.back()).Sqr(),
                    (a.front() - b.front()).Sqr()
                };
                for (int k = 0; k < 4; ++k) {
                    if (dist[k] <= best) {
                        best = dist[k];
                        bestI = i;
                        bestJ = j;
                        bestCase = k;
                    }
                }
            }
        }
        if (bestCase < 0) {
            break;
        }

        auto& a = open[bestI].points;
        auto b = std::move(open[bestJ].points);
        switch (bestCase) {
            case 0:  // a.back -> b.front
                break;
            case 1:  // a.back -> b.back
                std::reverse(b.begin(), b.end());
                break;
            case 2:  // a.front -> b.back, prepend b
                std::swap(a, b);
                break;
            case 3:  // a.front -> b.front
                std::reverse(a.begin(), a.end());
                break;
            default:
                break;
        }
        a.insert(a.end(), b.begin(), b.end());
        open.erase(open.begin() + static_cast<std::ptrdiff_t>(bestJ));
        merged = true;
    }
}
}  // namespace

SliceResult Gui::Section::sliceMesh(const IndexedMesh& mesh, const CutPlane& plane, double gapTolerance)
{
    SliceResult result;
    const auto& pts = mesh.points;
    if (mesh.triangles.empty()) {
        return result;
    }

    std::vector<double> dist(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        dist[i] = plane.signedDistance(pts[i]);
    }

    // Every mesh edge crossing the plane yields one contour node. Sharing the node
    // between the adjacent triangles gives the contours their topology.
    const IndexedMesh* topology = &mesh;
    IndexedMesh withEdges;
    if (mesh.triangleEdges.size() != mesh.triangles.size()) {
        withEdges.triangles = mesh.triangles;
        withEdges.buildEdges();
        topology = &withEdges;
    }
    const auto& edges = topology->edges;
    const auto& triangleEdges = topology->triangleEdges;

    std::vector<Base::Vector3d> nodes;
    std::vector<uint32_t> edgeNodes(edges.size(), invalidIndex);
    std::vector<std::array<uint32_t, 2>> segments;

    auto nodeOnEdge = [&](uint32_t edge) {
        uint32_t& node = edgeNodes[edge];
        if (node == invalidIndex) {
            const uint32_t a = edges[edge][0];
            const uint32_t b = edges[edge][1];
            const double da = dist[a];
            const double db = dist[b];
            const double t = da / (da - db);
            node = static_cast<uint32_t>(nodes.size());
            nodes.push_back(pts[a] + (pts[b] - pts[a]) * t);
        }
        return node;
    };

    for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
        const auto& tri = mesh.triangles[t];
        const bool k0 = dist[tri[0]] >= 0.0;
        const bool k1 = dist[tri[1]] >= 0.0;
        const bool k2 = dist[tri[2]] >= 0.0;
        if (k0 == k1 && k1 == k2) {
            continue;
        }
        const auto& triEdges = triangleEdges[t];
        std::array<uint32_t, 2> seg {};
        int count = 0;
        if (k0 != k1) {
            seg[count++] = nodeOnEdge(triEdges[0]);
        }
        if (k1 != k2) {
            seg[count++] = nodeOnEdge(triEdges[1]);
        }
        if (k2 != k0) {
            seg[count++] = nodeOnEdge(triEdges[2]);
        }
        segments.push_back(seg);
    }

    if (segments.empty()) {
        return result;
    }

    // Node to segment incidence in compressed row storage
    std::vector<uint32_t> first(nodes.size() + 1, 0);
    for (const auto& seg : segments) {
        ++first[seg[0] + 1];
        ++first[seg[1] + 1];
    }
    std::partial_sum(first.begin(), first.end(), first.begin());
    std::vector<uint32_t> incident(first.back());
    std::vector<uint32_t> fill(first.begin(), first.end() - 1);
    for (uint32_t s = 0; s < segments.size(); ++s) {
        incident[fill[segments[s][0]]++] = s;
        incident[fill[segments[s][1]]++] = s;
    }

    std::vector<bool> used(segments.size(), false);
    auto nextSegment = [&](uint32_t node) {
        for (uint32_t k = first[node]; k < first[node + 1]; ++k) {
            if (!used[incident[k]]) {
                return incident[k];
            }
        }
        return invalidIndex;
    };

    std::vector<std::vector<uint32_t>> loops;
    std::vector<std::vector<uint32_t>> openChains;

    auto walk = [&](uint32_t node) {
        std::vector<uint32_t> sequence {node};
        uint32_t seg = nextSegment(node);
        while (seg != invalidIndex) {
            used[seg] = true;
            node = segments[seg][0] == node ? segments[seg][1] : segments[seg][0];
            sequence.push_back(node);
            seg = nextSegment(node);
        }
        const bool closed = sequence.size() > 2 && sequence.front() == sequence.back();
        if (closed) {
            sequence.pop_back();
        }
        splitAtRepeats(sequence, closed, loops, openChains);
    };

    // Nodes with an odd number of segments are ends of open chains. Start there
    // so open chains are traversed in one piece.
    for (uint32_t node = 0; node < nodes.size(); ++node) {
        if ((first[node + 1] - first[node]) % 2 == 1 && nextSegment(node) != invalidIndex) {
            walk(node);
        }
    }
    for (uint32_t s = 0; s < segments.size(); ++s) {
        if (!used[s]) {
            walk(segments[s][0]);
        }
    }

    const double dupTolerance = std::max(gapTolerance * 1e-3, 1e-15);
    auto toPolyline = [&](const std::vector<uint32_t>& sequence, bool closed) {
        Polyline line;
        line.closed = closed;
        line.points.reserve(sequence.size());
        for (uint32_t idx : sequence) {
            line.points.push_back(nodes[idx]);
        }
        removeDuplicatePoints(line, dupTolerance);
        return line;
    };

    for (const auto& loop : loops) {
        Polyline line = toPolyline(loop, true);
        if (line.points.size() >= 3) {
            result.contours.push_back(std::move(line));
        }
    }

    std::vector<Polyline> open;
    for (const auto& chain : openChains) {
        Polyline line = toPolyline(chain, false);
        if (line.points.size() >= 2) {
            open.push_back(std::move(line));
        }
    }
    if (!open.empty() && gapTolerance > 0.0) {
        closeGaps(open, result.contours, gapTolerance);
    }
    result.openContours = open.size();
    for (auto& line : open) {
        result.contours.push_back(std::move(line));
    }
    return result;
}

// ----------------------------------------------------------------------------
// Polygon triangulation
//
// Ear clipping with hole elimination and z-order hashing, following the
// algorithm of the earcut library by Mapbox (ISC license,
// https://github.com/mapbox/earcut). Holes are bridged to the outer ring with
// David Eberly's method, and degenerate input is handled by successively more
// aggressive passes.

namespace
{
class EarClipper
{
public:
    explicit EarClipper(std::vector<uint32_t>& output)
        : triangles(output)
    {}

    void run(const std::vector<std::vector<Point2d>>& rings);

private:
    struct Node
    {
        Node(uint32_t index, double xPos, double yPos)
            : i(index)
            , x(xPos)
            , y(yPos)
        {}

        uint32_t i;
        double x;
        double y;
        Node* prev {nullptr};
        Node* next {nullptr};
        int32_t z {0};
        Node* prevZ {nullptr};
        Node* nextZ {nullptr};
        bool steiner {false};
    };

    std::vector<uint32_t>& triangles;
    std::deque<Node> pool;
    bool hashing {false};
    double minX {0.0};
    double minY {0.0};
    double invSize {0.0};

    Node* linkedList(const std::vector<Point2d>& ring, uint32_t offset, bool clockwise);
    Node* filterPoints(Node* start, Node* end = nullptr);
    void earcutLinked(Node* ear, int pass = 0);
    bool isEar(Node* ear) const;
    bool isEarHashed(Node* ear) const;
    Node* cureLocalIntersections(Node* start);
    void splitEarcut(Node* start);
    Node* eliminateHoles(const std::vector<std::vector<Point2d>>& rings, Node* outerNode);
    Node* eliminateHole(Node* hole, Node* outerNode);
    static Node* findHoleBridge(Node* hole, Node* outerNode);
    static bool sectorContainsSector(const Node* m, const Node* p);
    void indexCurve(Node* start) const;
    static Node* sortLinked(Node* list);
    int32_t zOrder(double xPos, double yPos) const;
    static Node* getLeftmost(Node* start);
    static bool pointInTriangle(
        double ax,
        double ay,
        double bx,
        double by,
        double cx,
        double cy,
        double px,
        double py
    );
    static bool isValidDiagonal(Node* a, Node* b);
    static double area(const Node* p, const Node* q, const Node* r);
    static bool equals(const Node* p1, const Node* p2);
    static bool intersects(const Node* p1, const Node* q1, const Node* p2, const Node* q2);
    static bool onSegment(const Node* p, const Node* q, const Node* r);
    static int sign(double val);
    static bool intersectsPolygon(const Node* a, const Node* b);
    static bool locallyInside(const Node* a, const Node* b);
    static bool middleInside(const Node* a, const Node* b);
    Node* splitPolygon(Node* a, Node* b);
    Node* insertNode(uint32_t index, double xPos, double yPos, Node* last);
    static void removeNode(Node* p);
    void addTriangle(const Node* a, const Node* b, const Node* c);
};

void EarClipper::run(const std::vector<std::vector<Point2d>>& rings)
{
    if (rings.empty() || rings.front().size() < 3) {
        return;
    }

    std::size_t total = 0;
    for (const auto& ring : rings) {
        total += ring.size();
    }

    Node* outerNode = linkedList(rings.front(), 0, true);
    if (!outerNode || outerNode->next == outerNode->prev) {
        return;
    }

    if (rings.size() > 1) {
        outerNode = eliminateHoles(rings, outerNode);
    }

    // Use z-order hashing for bigger polygons
    if (total > 80) {
        double maxX = rings.front().front()[0];
        double maxY = rings.front().front()[1];
        minX = maxX;
        minY = maxY;
        for (const auto& pnt : rings.front()) {
            minX = std::min(minX, pnt[0]);
            minY = std::min(minY, pnt[1]);
            maxX = std::max(maxX, pnt[0]);
            maxY = std::max(maxY, pnt[1]);
        }
        invSize = std::max(maxX - minX, maxY - minY);
        invSize = invSize != 0.0 ? 32767.0 / invSize : 0.0;
        hashing = invSize != 0.0;
    }

    earcutLinked(outerNode);
}

EarClipper::Node* EarClipper::linkedList(const std::vector<Point2d>& ring, uint32_t offset, bool clockwise)
{
    double sum = 0.0;
    const std::size_t count = ring.size();
    for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
        sum += (ring[j][0] - ring[i][0]) * (ring[i][1] + ring[j][1]);
    }

    Node* last = nullptr;
    if (clockwise == (sum > 0.0)) {
        for (std::size_t i = 0; i < count; ++i) {
            last = insertNode(offset + static_cast<uint32_t>(i), ring[i][0], ring[i][1], last);
        }
    }
    else {
        for (std::size_t i = count; i-- > 0;) {
            last = insertNode(offset + static_cast<uint32_t>(i), ring[i][0], ring[i][1], last);
        }
    }

    if (last && equals(last, last->next)) {
        removeNode(last);
        last = last->next;
    }
    return last;
}

EarClipper::Node* EarClipper::filterPoints(Node* start, Node* end)
{
    if (!start) {
        return start;
    }
    if (!end) {
        end = start;
    }

    Node* p = start;
    bool again = false;
    do {
        again = false;
        if (!p->steiner && (equals(p, p->next) || area(p->prev, p, p->next) == 0.0)) {
            removeNode(p);
            p = end = p->prev;
            if (p == p->next) {
                break;
            }
            again = true;
        }
        else {
            p = p->next;
        }
    } while (again || p != end);

    return end;
}

void EarClipper::earcutLinked(Node* ear, int pass)
{
    if (!ear) {
        return;
    }

    if (pass == 0 && hashing) {
        indexCurve(ear);
    }

    Node* stop = ear;
    while (ear->prev != ear->next) {
        Node* prev = ear->prev;
        Node* next = ear->next;

        if (hashing ? isEarHashed(ear) : isEar(ear)) {
            addTriangle(prev, ear, next);
            removeNode(ear);
            // Skipping the next vertex leads to less sliver triangles
            ear = next->next;
            stop = next->next;
            continue;
        }

        ear = next;

        // Looped through the whole remaining polygon without finding an ear
        if (ear == stop) {
            if (pass == 0) {
                earcutLinked(filterPoints(ear), 1);
            }
            else if (pass == 1) {
                ear = cureLocalIntersections(filterPoints(ear));
                earcutLinked(ear, 2);
            }
            else if (pass == 2) {
                splitEarcut(ear);
            }
            break;
        }
    }
}

bool EarClipper::isEar(Node* ear) const
{
    const Node* a = ear->prev;
    const Node* b = ear;
    const Node* c = ear->next;

    if (area(a, b, c) >= 0.0) {
        return false;  // reflex
    }

    const double x0 = std::min({a->x, b->x, c->x});
    const double y0 = std::min({a->y, b->y, c->y});
    const double x1 = std::max({a->x, b->x, c->x});
    const double y1 = std::max({a->y, b->y, c->y});

    const Node* p = c->next;
    while (p != a) {
        if (p->x >= x0 && p->x <= x1 && p->y >= y0 && p->y <= y1
            && pointInTriangle(a->x, a->y, b->x, b->y, c->x, c->y, p->x, p->y)
            && area(p->prev, p, p->next) >= 0.0) {
            return false;
        }
        p = p->next;
    }
    return true;
}

bool EarClipper::isEarHashed(Node* ear) const
{
    const Node* a = ear->prev;
    const Node* b = ear;
    const Node* c = ear->next;

    if (area(a, b, c) >= 0.0) {
        return false;  // reflex
    }

    const double x0 = std::min({a->x, b->x, c->x});
    const double y0 = std::min({a->y, b->y, c->y});
    const double x1 = std::max({a->x, b->x, c->x});
    const double y1 = std::max({a->y, b->y, c->y});

    const int32_t minZ = zOrder(x0, y0);
    const int32_t maxZ = zOrder(x1, y1);

    auto blocks = [&](const Node* n) {
        return n->x >= x0 && n->x <= x1 && n->y >= y0 && n->y <= y1 && n != a && n != c
            && pointInTriangle(a->x, a->y, b->x, b->y, c->x, c->y, n->x, n->y)
            && area(n->prev, n, n->next) >= 0.0;
    };

    const Node* p = ear->prevZ;
    const Node* n = ear->nextZ;

    while (p && p->z >= minZ && n && n->z <= maxZ) {
        if (blocks(p)) {
            return false;
        }
        p = p->prevZ;
        if (blocks(n)) {
            return false;
        }
        n = n->nextZ;
    }
    while (p && p->z >= minZ) {
        if (blocks(p)) {
            return false;
        }
        p = p->prevZ;
    }
    while (n && n->z <= maxZ) {
        if (blocks(n)) {
            return false;
        }
        n = n->nextZ;
    }
    return true;
}

EarClipper::Node* EarClipper::cureLocalIntersections(Node* start)
{
    Node* p = start;
    do {
        Node* a = p->prev;
        Node* b = p->next->next;

        if (!equals(a, b) && intersects(a, p, p->next, b) && locallyInside(a, b)
            && locallyInside(b, a)) {
            addTriangle(a, p, b);
            removeNode(p);
            removeNode(p->next);
            p = start = b;
        }
        p = p->next;
    } while (p != start);

    return filterPoints(p);
}

void EarClipper::splitEarcut(Node* start)
{
    Node* a = start;
    do {
        Node* b = a->next->next;
        while (b != a->prev) {
            if (a->i != b->i && isValidDiagonal(a, b)) {
                Node* c = splitPolygon(a, b);
                a = filterPoints(a, a->next);
                c = filterPoints(c, c->next);
                earcutLinked(a);
                earcutLinked(c);
                return;
            }
            b = b->next;
        }
        a = a->next;
    } while (a != start);
}

EarClipper::Node* EarClipper::eliminateHoles(
    const std::vector<std::vector<Point2d>>& rings,
    Node* outerNode
)
{
    std::vector<Node*> queue;
    uint32_t offset = static_cast<uint32_t>(rings.front().size());
    for (std::size_t r = 1; r < rings.size(); ++r) {
        const auto& ring = rings[r];
        if (ring.size() >= 3) {
            Node* list = linkedList(ring, offset, false);
            if (list) {
                if (list == list->next) {
                    list->steiner = true;
                }
                queue.push_back(getLeftmost(list));
            }
        }
        offset += static_cast<uint32_t>(ring.size());
    }

    std::sort(queue.begin(), queue.end(), [](const Node* a, const Node* b) {
        if (a->x != b->x) {
            return a->x < b->x;
        }
        if (a->y != b->y) {
            return a->y < b->y;
        }
        // Holes touching each other in one point: the one with the smaller slope first
        const double aSlope = (a->next->y - a->y) / (a->next->x - a->x);
        const double bSlope = (b->next->y - b->y) / (b->next->x - b->x);
        return aSlope < bSlope;
    });

    for (Node* hole : queue) {
        outerNode = eliminateHole(hole, outerNode);
    }
    return outerNode;
}

EarClipper::Node* EarClipper::eliminateHole(Node* hole, Node* outerNode)
{
    Node* bridge = findHoleBridge(hole, outerNode);
    if (!bridge) {
        return outerNode;
    }

    Node* bridgeReverse = splitPolygon(bridge, hole);
    filterPoints(bridgeReverse, bridgeReverse->next);
    return filterPoints(bridge, bridge->next);
}

EarClipper::Node* EarClipper::findHoleBridge(Node* hole, Node* outerNode)
{
    Node* p = outerNode;
    const double hx = hole->x;
    const double hy = hole->y;
    double qx = -std::numeric_limits<double>::infinity();
    Node* m = nullptr;

    // Find a segment intersected by a ray from the hole's leftmost point to the
    // left. The segment's endpoint with lesser x is a potential connection point.
    if (equals(hole, p)) {
        return p;
    }
    do {
        if (equals(hole, p->next)) {
            return p->next;
        }
        if (hy <= p->y && hy >= p->next->y && p->next->y != p->y) {
            const double x = p->x + (hy - p->y) * (p->next->x - p->x) / (p->next->y - p->y);
            if (x <= hx && x > qx) {
                qx = x;
                m = p->x < p->next->x ? p : p->next;
                if (x == hx) {
                    // The hole touches the outer segment
                    return m;
                }
            }
        }
        p = p->next;
    } while (p != outerNode);

    if (!m) {
        return nullptr;
    }

    // Look for points inside the triangle of hole point, segment intersection and
    // endpoint. If there are none, the connection is valid. Otherwise choose the
    // point with the minimum angle with the ray as connection point.
    const Node* stop = m;
    const double mx = m->x;
    const double my = m->y;
    double tanMin = std::numeric_limits<double>::infinity();

    p = m;
    do {
        if (hx >= p->x && p->x >= mx && hx != p->x
            && pointInTriangle(hy < my ? hx : qx, hy, mx, my, hy < my ? qx : hx, hy, p->x, p->y)) {
            const double tan = std::abs(hy - p->y) / (hx - p->x);
            if (locallyInside(p, hole)
                && (tan < tanMin
                    || (tan == tanMin
                        && (p->x > m->x || (p->x == m->x && sectorContainsSector(m, p)))))) {
                m = p;
                tanMin = tan;
            }
        }
        p = p->next;
    } while (p != stop);

    return m;
}

bool EarClipper::sectorContainsSector(const Node* m, const Node* p)
{
    return area(m->prev, m, p->prev) < 0.0 && area(p->next, m, m->next) < 0.0;
}

void EarClipper::indexCurve(Node* start) const
{
    Node* p = start;
    do {
        if (p->z == 0) {
            p->z = zOrder(p->x, p->y);
        }
        p->prevZ = p->prev;
        p->nextZ = p->next;
        p = p->next;
    } while (p != start);

    p->prevZ->nextZ = nullptr;
    p->prevZ = nullptr;

    sortLinked(p);
}

EarClipper::Node* EarClipper::sortLinked(Node* list)
{
    // Simon Tatham's linked list merge sort
    int inSize = 1;
    int numMerges = 0;
    do {
        Node* p = list;
        list = nullptr;
        Node* tail = nullptr;
        numMerges = 0;

        while (p) {
            ++numMerges;
            Node* q = p;
            int pSize = 0;
            for (int i = 0; i < inSize; ++i) {
                ++pSize;
                q = q->nextZ;
                if (!q) {
                    break;
                }
            }
            int qSize = inSize;

            while (pSize > 0 || (qSize > 0 && q)) {
                Node* e = nullptr;
                if (pSize != 0 && (qSize == 0 || !q || p->z <= q->z)) {
                    e = p;
                    p = p->nextZ;
                    --pSize;
                }
                else {
                    e = q;
                    q = q->nextZ;
                    --qSize;
                }

                if (tail) {
                    tail->nextZ = e;
                }
                else {
                    list = e;
                }
                e->prevZ = tail;
                tail = e;
            }
            p = q;
        }

        if (tail) {
            tail->nextZ = nullptr;
        }
        inSize *= 2;
    } while (numMerges > 1);

    return list;
}

int32_t EarClipper::zOrder(double xPos, double yPos) const
{
    // Coordinates are mapped to a non-negative 15 bit integer range
    auto x = static_cast<int32_t>((xPos - minX) * invSize);
    auto y = static_cast<int32_t>((yPos - minY) * invSize);

    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;

    y = (y | (y << 8)) & 0x00FF00FF;
    y = (y | (y << 4)) & 0x0F0F0F0F;
    y = (y | (y << 2)) & 0x33333333;
    y = (y | (y << 1)) & 0x55555555;

    return x | (y << 1);
}

EarClipper::Node* EarClipper::getLeftmost(Node* start)
{
    Node* p = start;
    Node* leftmost = start;
    do {
        if (p->x < leftmost->x || (p->x == leftmost->x && p->y < leftmost->y)) {
            leftmost = p;
        }
        p = p->next;
    } while (p != start);
    return leftmost;
}

bool EarClipper::pointInTriangle(
    double ax,
    double ay,
    double bx,
    double by,
    double cx,
    double cy,
    double px,
    double py
)
{
    return (cx - px) * (ay - py) >= (ax - px) * (cy - py)
        && (ax - px) * (by - py) >= (bx - px) * (ay - py)
        && (bx - px) * (cy - py) >= (cx - px) * (by - py);
}

bool EarClipper::isValidDiagonal(Node* a, Node* b)
{
    // The diagonal must not intersect other edges, must be locally visible, and
    // must not create opposite facing sectors. Zero length diagonals are a special case.
    return a->next->i != b->i && a->prev->i != b->i && !intersectsPolygon(a, b)
        && ((locallyInside(a, b) && locallyInside(b, a) && middleInside(a, b)
             && (area(a->prev, a, b->prev) != 0.0 || area(a, b->prev, b) != 0.0))
            || (equals(a, b) && area(a->prev, a, a->next) > 0.0 && area(b->prev, b, b->next) > 0.0));
}

double EarClipper::area(const Node* p, const Node* q, const Node* r)
{
    return (q->y - p->y) * (r->x - q->x) - (q->x - p->x) * (r->y - q->y);
}

bool EarClipper::equals(const Node* p1, const Node* p2)
{
    return p1->x == p2->x && p1->y == p2->y;
}

bool EarClipper::intersects(const Node* p1, const Node* q1, const Node* p2, const Node* q2)
{
    const int o1 = sign(area(p1, q1, p2));
    const int o2 = sign(area(p1, q1, q2));
    const int o3 = sign(area(p2, q2, p1));
    const int o4 = sign(area(p2, q2, q1));

    if (o1 != o2 && o3 != o4) {
        return true;
    }
    if (o1 == 0 && onSegment(p1, p2, q1)) {
        return true;
    }
    if (o2 == 0 && onSegment(p1, q2, q1)) {
        return true;
    }
    if (o3 == 0 && onSegment(p2, p1, q2)) {
        return true;
    }
    if (o4 == 0 && onSegment(p2, q1, q2)) {
        return true;
    }
    return false;
}

bool EarClipper::onSegment(const Node* p, const Node* q, const Node* r)
{
    return q->x <= std::max(p->x, r->x) && q->x >= std::min(p->x, r->x)
        && q->y <= std::max(p->y, r->y) && q->y >= std::min(p->y, r->y);
}

int EarClipper::sign(double val)
{
    return (0.0 < val) - (val < 0.0);
}

bool EarClipper::intersectsPolygon(const Node* a, const Node* b)
{
    const Node* p = a;
    do {
        if (p->i != a->i && p->next->i != a->i && p->i != b->i && p->next->i != b->i
            && intersects(p, p->next, a, b)) {
            return true;
        }
        p = p->next;
    } while (p != a);
    return false;
}

bool EarClipper::locallyInside(const Node* a, const Node* b)
{
    return area(a->prev, a, a->next) < 0.0 ? area(a, b, a->next) >= 0.0 && area(a, a->prev, b) >= 0.0
                                           : area(a, b, a->prev) < 0.0 || area(a, a->next, b) < 0.0;
}

bool EarClipper::middleInside(const Node* a, const Node* b)
{
    const Node* p = a;
    bool inside = false;
    const double px = (a->x + b->x) / 2.0;
    const double py = (a->y + b->y) / 2.0;
    do {
        if (((p->y > py) != (p->next->y > py)) && p->next->y != p->y
            && (px < (p->next->x - p->x) * (py - p->y) / (p->next->y - p->y) + p->x)) {
            inside = !inside;
        }
        p = p->next;
    } while (p != a);
    return inside;
}

EarClipper::Node* EarClipper::splitPolygon(Node* a, Node* b)
{
    Node* a2 = &pool.emplace_back(a->i, a->x, a->y);
    Node* b2 = &pool.emplace_back(b->i, b->x, b->y);
    Node* an = a->next;
    Node* bp = b->prev;

    a->next = b;
    b->prev = a;

    a2->next = an;
    an->prev = a2;

    b2->next = a2;
    a2->prev = b2;

    bp->next = b2;
    b2->prev = bp;

    return b2;
}

EarClipper::Node* EarClipper::insertNode(uint32_t index, double xPos, double yPos, Node* last)
{
    Node* p = &pool.emplace_back(index, xPos, yPos);
    if (!last) {
        p->prev = p;
        p->next = p;
    }
    else {
        p->next = last->next;
        p->prev = last;
        last->next->prev = p;
        last->next = p;
    }
    return p;
}

void EarClipper::removeNode(Node* p)
{
    p->next->prev = p->prev;
    p->prev->next = p->next;
    if (p->prevZ) {
        p->prevZ->nextZ = p->nextZ;
    }
    if (p->nextZ) {
        p->nextZ->prevZ = p->prevZ;
    }
}

void EarClipper::addTriangle(const Node* a, const Node* b, const Node* c)
{
    triangles.push_back(a->i);
    triangles.push_back(b->i);
    triangles.push_back(c->i);
}
}  // namespace

std::vector<uint32_t> Gui::Section::triangulatePolygon(const std::vector<std::vector<Point2d>>& rings)
{
    std::vector<uint32_t> triangles;
    EarClipper clipper(triangles);
    clipper.run(rings);
    return triangles;
}

// ----------------------------------------------------------------------------

namespace
{
struct Ring
{
    std::vector<Point2d> points;
    uint32_t firstVertex {0};
    double area {0.0};
    double minX {0.0};
    double minY {0.0};
    double maxX {0.0};
    double maxY {0.0};
    int parent {-1};
    int depth {0};
};

bool pointInRing(const Ring& ring, const Point2d& pnt)
{
    bool inside = false;
    const auto& pts = ring.points;
    for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        if (((pts[i][1] > pnt[1]) != (pts[j][1] > pnt[1]))
            && (pnt[0] < (pts[j][0] - pts[i][0]) * (pnt[1] - pts[i][1]) / (pts[j][1] - pts[i][1])
                    + pts[i][0])) {
            inside = !inside;
        }
    }
    return inside;
}

bool ringContains(const Ring& outer, const Ring& inner)
{
    if (inner.minX < outer.minX || inner.maxX > outer.maxX || inner.minY < outer.minY
        || inner.maxY > outer.maxY) {
        return false;
    }
    // Contours may touch, so vote with a few sample points
    const std::size_t count = inner.points.size();
    const std::array<std::size_t, 3> samples {0, count / 3, (2 * count) / 3};
    int votes = 0;
    for (std::size_t idx : samples) {
        const auto& p0 = inner.points[idx];
        const auto& p1 = inner.points[(idx + 1) % count];
        const Point2d mid {(p0[0] + p1[0]) * 0.5, (p0[1] + p1[1]) * 0.5};
        if (pointInRing(outer, mid)) {
            ++votes;
        }
    }
    return votes >= 2;
}
}  // namespace

double CapMesh::area() const
{
    double sum = 0.0;
    for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
        const auto& a = vertices[triangles[i]];
        const auto& b = vertices[triangles[i + 1]];
        const auto& c = vertices[triangles[i + 2]];
        sum += ((b - a) % (c - a)).Length() * 0.5;
    }
    return sum;
}

CapMesh Gui::Section::buildCap(const std::vector<Polyline>& contours, const CutPlane& plane)
{
    CapMesh cap;
    Base::Vector3d uDir;
    Base::Vector3d vDir;
    plane.getAxes(uDir, vDir);

    std::vector<Ring> rings;
    double extent = 0.0;
    for (const auto& contour : contours) {
        if (!contour.closed || contour.points.size() < 3) {
            continue;
        }
        Ring ring;
        ring.points.reserve(contour.points.size());
        ring.minX = ring.minY = std::numeric_limits<double>::max();
        ring.maxX = ring.maxY = std::numeric_limits<double>::lowest();
        for (const auto& pnt : contour.points) {
            const Point2d p2 {pnt * uDir, pnt * vDir};
            ring.points.push_back(p2);
            ring.minX = std::min(ring.minX, p2[0]);
            ring.minY = std::min(ring.minY, p2[1]);
            ring.maxX = std::max(ring.maxX, p2[0]);
            ring.maxY = std::max(ring.maxY, p2[1]);
        }
        const auto& pts = ring.points;
        for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
            ring.area += (pts[j][0] - pts[i][0]) * (pts[j][1] + pts[i][1]);
        }
        ring.area *= 0.5;
        extent = std::max({extent, ring.maxX - ring.minX, ring.maxY - ring.minY});

        ring.firstVertex = static_cast<uint32_t>(cap.vertices.size());
        cap.vertices.insert(cap.vertices.end(), contour.points.begin(), contour.points.end());
        rings.push_back(std::move(ring));
    }

    // Drop degenerate slivers, e.g. from faces lying in the cutting plane
    const double minArea = extent * extent * 1e-12;
    std::vector<int> order;
    for (int i = 0; i < static_cast<int>(rings.size()); ++i) {
        if (std::abs(rings[i].area) > minArea) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&rings](int a, int b) {
        return std::abs(rings[a].area) > std::abs(rings[b].area);
    });

    // Nesting: the parent of a ring is the smallest ring containing it
    for (std::size_t k = 0; k < order.size(); ++k) {
        Ring& ring = rings[order[k]];
        for (std::size_t j = k; j-- > 0;) {
            if (ringContains(rings[order[j]], ring)) {
                ring.parent = order[j];
                ring.depth = rings[order[j]].depth + 1;
                break;
            }
        }
    }

    for (int idx : order) {
        const Ring& ring = rings[idx];
        std::vector<uint32_t> outline(ring.points.size());
        std::iota(outline.begin(), outline.end(), ring.firstVertex);
        cap.outlines.push_back(std::move(outline));
    }

    // Even depth: material, odd depth: hole
    for (int idx : order) {
        const Ring& outer = rings[idx];
        if (outer.depth % 2 != 0) {
            continue;
        }

        std::vector<std::vector<Point2d>> polygon {outer.points};
        std::vector<uint32_t> offsets {outer.firstVertex};
        for (int hdx : order) {
            if (rings[hdx].parent == idx) {
                polygon.push_back(rings[hdx].points);
                offsets.push_back(rings[hdx].firstVertex);
            }
        }

        const std::vector<uint32_t> local = triangulatePolygon(polygon);
        // Map indices of the concatenated rings back to the cap vertices
        std::vector<uint32_t> ringStart;
        uint32_t count = 0;
        for (const auto& pts : polygon) {
            ringStart.push_back(count);
            count += static_cast<uint32_t>(pts.size());
        }
        for (uint32_t index : local) {
            const auto it = std::upper_bound(ringStart.begin(), ringStart.end(), index);
            const auto ringIndex = static_cast<std::size_t>(it - ringStart.begin()) - 1;
            cap.triangles.push_back(offsets[ringIndex] + (index - ringStart[ringIndex]));
        }
    }

    return cap;
}
