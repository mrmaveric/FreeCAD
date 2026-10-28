// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include <Gui/Section/SectionGeometry.h>

using namespace Gui::Section;
using Base::Vector3d;

namespace
{

void addQuad(
    std::vector<Vector3d>& soup,
    const Vector3d& p0,
    const Vector3d& p1,
    const Vector3d& p2,
    const Vector3d& p3
)
{
    soup.insert(soup.end(), {p0, p1, p2, p0, p2, p3});
}

/// Triangle soup of an axis aligned box, every face with its own vertices
std::vector<Vector3d> boxSoup(const Vector3d& min, const Vector3d& max, bool inward = false)
{
    std::array<Vector3d, 8> c {
        Vector3d(min.x, min.y, min.z),
        Vector3d(max.x, min.y, min.z),
        Vector3d(max.x, max.y, min.z),
        Vector3d(min.x, max.y, min.z),
        Vector3d(min.x, min.y, max.z),
        Vector3d(max.x, min.y, max.z),
        Vector3d(max.x, max.y, max.z),
        Vector3d(min.x, max.y, max.z),
    };
    std::vector<Vector3d> soup;
    auto quad = [&](int a, int b, int d, int e) {
        if (inward) {
            addQuad(soup, c[e], c[d], c[b], c[a]);
        }
        else {
            addQuad(soup, c[a], c[b], c[d], c[e]);
        }
    };
    quad(0, 3, 2, 1);  // bottom
    quad(4, 5, 6, 7);  // top
    quad(0, 1, 5, 4);  // front
    quad(1, 2, 6, 5);  // right
    quad(2, 3, 7, 6);  // back
    quad(3, 0, 4, 7);  // left
    return soup;
}

/// Closed cylinder along z, approximated with @p segments facets
std::vector<Vector3d> cylinderSoup(double radius, double height, int segments)
{
    std::vector<Vector3d> soup;
    const Vector3d bottom(0, 0, 0);
    const Vector3d top(0, 0, height);
    for (int i = 0; i < segments; ++i) {
        const double a0 = 2.0 * std::numbers::pi * i / segments;
        const double a1 = 2.0 * std::numbers::pi * (i + 1) / segments;
        const Vector3d p0(radius * std::cos(a0), radius * std::sin(a0), 0);
        const Vector3d p1(radius * std::cos(a1), radius * std::sin(a1), 0);
        const Vector3d q0 = p0 + top;
        const Vector3d q1 = p1 + top;
        addQuad(soup, p0, p1, q1, q0);
        soup.insert(soup.end(), {bottom, p1, p0});
        soup.insert(soup.end(), {top, q0, q1});
    }
    return soup;
}

double polygonArea(const std::vector<Point2d>& pts)
{
    double sum = 0.0;
    for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        sum += (pts[j][0] + pts[i][0]) * (pts[i][1] - pts[j][1]);
    }
    return sum * 0.5;
}

double triangulatedArea(
    const std::vector<std::vector<Point2d>>& rings,
    const std::vector<uint32_t>& tris,
    bool* allCounterClockwise = nullptr
)
{
    std::vector<Point2d> all;
    for (const auto& ring : rings) {
        all.insert(all.end(), ring.begin(), ring.end());
    }
    double sum = 0.0;
    bool ccw = true;
    for (std::size_t i = 0; i < tris.size(); i += 3) {
        const double area = polygonArea({all[tris[i]], all[tris[i + 1]], all[tris[i + 2]]});
        ccw = ccw && area >= 0.0;
        sum += std::abs(area);
    }
    if (allCounterClockwise) {
        *allCounterClockwise = ccw;
    }
    return sum;
}

IndexedMesh weld(const std::vector<Vector3d>& soup)
{
    return weldTriangleSoup(soup, 1e-9);
}

}  // namespace

TEST(SectionGeometry, cutPlaneConvention)
{
    CutPlane plane(Vector3d(0, 0, 2), Vector3d(0, 0, 5));
    EXPECT_DOUBLE_EQ(plane.distance, 2.0);
    EXPECT_DOUBLE_EQ(plane.signedDistance(Vector3d(1, 1, 3)), 1.0);
    EXPECT_DOUBLE_EQ(plane.flipped().signedDistance(Vector3d(1, 1, 3)), -1.0);

    Vector3d u;
    Vector3d v;
    plane.getAxes(u, v);
    EXPECT_NEAR(u * plane.normal, 0.0, 1e-12);
    EXPECT_NEAR(v * plane.normal, 0.0, 1e-12);
    EXPECT_NEAR(((u % v) - (-plane.normal)).Length(), 0.0, 1e-12);

    Base::BoundBox3d box(0, 0, 0, 1, 1, 1);
    EXPECT_EQ(plane.classify(box), -1);
    EXPECT_EQ(plane.flipped().classify(box), 1);
    EXPECT_EQ(CutPlane(Vector3d(0, 0, 0.5), Vector3d(0, 0, 1)).classify(box), 0);
}

TEST(SectionGeometry, triangulateSquare)
{
    std::vector<std::vector<Point2d>> rings {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    auto tris = triangulatePolygon(rings);
    ASSERT_EQ(tris.size(), 6U);
    bool ccw = false;
    EXPECT_NEAR(triangulatedArea(rings, tris, &ccw), 1.0, 1e-12);
    EXPECT_TRUE(ccw);

    // Orientation of the input doesn't matter
    std::vector<std::vector<Point2d>> reversed {{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    tris = triangulatePolygon(reversed);
    EXPECT_NEAR(triangulatedArea(reversed, tris, &ccw), 1.0, 1e-12);
    EXPECT_TRUE(ccw);
}

TEST(SectionGeometry, triangulateConcaveWithHoles)
{
    // An L shape with two square holes
    std::vector<std::vector<Point2d>> rings {
        {{0, 0}, {4, 0}, {4, 1}, {1, 1}, {1, 4}, {0, 4}},
        {{0.25, 0.25}, {0.75, 0.25}, {0.75, 0.75}, {0.25, 0.75}},
        {{2.25, 0.25}, {2.25, 0.75}, {2.75, 0.75}, {2.75, 0.25}},
    };
    auto tris = triangulatePolygon(rings);
    bool ccw = false;
    EXPECT_NEAR(triangulatedArea(rings, tris, &ccw), 7.0 - 0.5, 1e-12);
    EXPECT_TRUE(ccw);
}

TEST(SectionGeometry, triangulateLargeCircleWithHole)
{
    // Big enough to use the z-order hashed code path
    const int count = 500;
    std::vector<Point2d> outer;
    std::vector<Point2d> inner;
    for (int i = 0; i < count; ++i) {
        const double a = 2.0 * std::numbers::pi * i / count;
        outer.push_back({10.0 * std::cos(a), 10.0 * std::sin(a)});
        inner.push_back({4.0 * std::cos(a), 4.0 * std::sin(a)});
    }
    std::vector<std::vector<Point2d>> rings {outer, inner};
    auto tris = triangulatePolygon(rings);
    const double expected = std::abs(polygonArea(outer)) - std::abs(polygonArea(inner));
    EXPECT_NEAR(triangulatedArea(rings, tris), expected, 1e-9);
}

TEST(SectionGeometry, weldBox)
{
    auto soup = boxSoup(Vector3d(0, 0, 0), Vector3d(1, 1, 1));
    ASSERT_EQ(soup.size(), 36U);

    // Jitter the duplicated vertices a little like a real tessellation does
    for (std::size_t i = 0; i < soup.size(); ++i) {
        soup[i].x += (i % 3) * 1e-11;
    }

    IndexedMesh mesh = weld(soup);
    EXPECT_EQ(mesh.points.size(), 8U);
    EXPECT_EQ(mesh.triangles.size(), 12U);
    EXPECT_TRUE(mesh.boundBox.IsValid());
}

TEST(SectionGeometry, weldDropsDegenerateAndInvalidTriangles)
{
    std::vector<Vector3d> soup {
        Vector3d(0, 0, 0),
        Vector3d(1e-12, 0, 0),
        Vector3d(0, 1, 0),
        Vector3d(0, 0, 0),
        Vector3d(1, 0, 0),
        Vector3d(0, std::nan(""), 0),
    };
    IndexedMesh mesh = weld(soup);
    EXPECT_TRUE(mesh.triangles.empty());
}

TEST(SectionGeometry, sliceBox)
{
    IndexedMesh mesh = weld(boxSoup(Vector3d(0, 0, 0), Vector3d(2, 3, 4)));
    CutPlane plane(Vector3d(0, 0, 1.5), Vector3d(0, 0, 1));
    SliceResult slice = sliceMesh(mesh, plane, 1e-6);
    ASSERT_EQ(slice.contours.size(), 1U);
    EXPECT_TRUE(slice.contours.front().closed);
    EXPECT_EQ(slice.openContours, 0U);
    for (const auto& pnt : slice.contours.front().points) {
        EXPECT_NEAR(pnt.z, 1.5, 1e-12);
    }

    CapMesh cap = buildCap(slice.contours, plane);
    EXPECT_NEAR(cap.area(), 6.0, 1e-9);
    EXPECT_EQ(cap.outlines.size(), 1U);

    // Cap triangles face the removed side
    for (std::size_t i = 0; i < cap.triangles.size(); i += 3) {
        const auto& a = cap.vertices[cap.triangles[i]];
        const auto& b = cap.vertices[cap.triangles[i + 1]];
        const auto& c = cap.vertices[cap.triangles[i + 2]];
        const Vector3d normal = (b - a) % (c - a);
        if (normal.Length() > 1e-12) {
            EXPECT_LT(normal * plane.normal, 0.0);
        }
    }
}

TEST(SectionGeometry, sliceObliqueBox)
{
    IndexedMesh mesh = weld(boxSoup(Vector3d(-1, -1, -1), Vector3d(1, 1, 1)));
    // A plane through the center perpendicular to a face diagonal
    CutPlane plane(Vector3d(0, 0, 0), Vector3d(1, 1, 0));
    SliceResult slice = sliceMesh(mesh, plane, 1e-6);
    ASSERT_EQ(slice.contours.size(), 1U);
    CapMesh cap = buildCap(slice.contours, plane);
    EXPECT_NEAR(cap.area(), 2.0 * std::sqrt(8.0), 1e-9);
}

TEST(SectionGeometry, slicePlaneThroughVertices)
{
    IndexedMesh mesh = weld(boxSoup(Vector3d(0, 0, 0), Vector3d(1, 1, 1)));

    // Coincides with the bottom face: vertices on the plane count as kept, so
    // nothing is cut
    SliceResult slice = sliceMesh(mesh, CutPlane(Vector3d(0, 0, 0), Vector3d(0, 0, 1)), 1e-6);
    EXPECT_TRUE(slice.contours.empty());

    // Same plane, keeping the other side: the face lies in the plane and the
    // box is entirely removed. Must not produce garbage.
    slice = sliceMesh(mesh, CutPlane(Vector3d(0, 0, 0), Vector3d(0, 0, -1)), 1e-6);
    CapMesh cap = buildCap(slice.contours, CutPlane(Vector3d(0, 0, 0), Vector3d(0, 0, -1)));
    EXPECT_LE(cap.area(), 1.0 + 1e-9);

    // Diagonal plane passing exactly through four corners
    CutPlane diagonal(Vector3d(0, 0, 0), Vector3d(1, -1, 0));
    slice = sliceMesh(mesh, diagonal, 1e-6);
    cap = buildCap(slice.contours, diagonal);
    EXPECT_EQ(slice.openContours, 0U);
    EXPECT_NEAR(cap.area(), std::sqrt(2.0), 1e-9);
}

TEST(SectionGeometry, sliceHollowBox)
{
    // A box with a box shaped cavity gives a cap with a hole
    auto soup = boxSoup(Vector3d(0, 0, 0), Vector3d(10, 10, 10));
    auto inner = boxSoup(Vector3d(2, 3, 2), Vector3d(8, 7, 8), true);
    soup.insert(soup.end(), inner.begin(), inner.end());

    IndexedMesh mesh = weld(soup);
    CutPlane plane(Vector3d(0, 0, 5), Vector3d(0, 0, 1));
    SliceResult slice = sliceMesh(mesh, plane, 1e-6);
    ASSERT_EQ(slice.contours.size(), 2U);
    CapMesh cap = buildCap(slice.contours, plane);
    EXPECT_NEAR(cap.area(), 100.0 - 24.0, 1e-9);
    EXPECT_EQ(cap.outlines.size(), 2U);
}

TEST(SectionGeometry, sliceNestedIslands)
{
    // Hollow box containing a separate solid: outer, hole and island
    auto soup = boxSoup(Vector3d(0, 0, 0), Vector3d(10, 10, 10));
    auto cavity = boxSoup(Vector3d(1, 1, 1), Vector3d(9, 9, 9), true);
    auto island = boxSoup(Vector3d(4, 4, 4), Vector3d(6, 6, 6));
    soup.insert(soup.end(), cavity.begin(), cavity.end());
    soup.insert(soup.end(), island.begin(), island.end());

    IndexedMesh mesh = weld(soup);
    CutPlane plane(Vector3d(0, 0, 5), Vector3d(0, 0, -1));
    SliceResult slice = sliceMesh(mesh, plane, 1e-6);
    ASSERT_EQ(slice.contours.size(), 3U);
    CapMesh cap = buildCap(slice.contours, plane);
    EXPECT_NEAR(cap.area(), 100.0 - 64.0 + 4.0, 1e-9);
}

TEST(SectionGeometry, sliceSeparateSolids)
{
    auto soup = boxSoup(Vector3d(0, 0, 0), Vector3d(1, 1, 1));
    auto second = boxSoup(Vector3d(3, 0, 0), Vector3d(5, 1, 1));
    soup.insert(soup.end(), second.begin(), second.end());

    IndexedMesh mesh = weld(soup);
    CutPlane plane(Vector3d(0, 0.5, 0), Vector3d(0, 1, 0));
    SliceResult slice = sliceMesh(mesh, plane, 1e-6);
    ASSERT_EQ(slice.contours.size(), 2U);
    CapMesh cap = buildCap(slice.contours, plane);
    EXPECT_NEAR(cap.area(), 1.0 + 2.0, 1e-9);
}

TEST(SectionGeometry, sliceCylinder)
{
    const int segments = 64;
    IndexedMesh mesh = weld(cylinderSoup(5.0, 20.0, segments));
    CutPlane plane(Vector3d(0, 0, 7.25), Vector3d(0, 0, 1));
    SliceResult slice = sliceMesh(mesh, plane, 1e-6);
    ASSERT_EQ(slice.contours.size(), 1U);
    CapMesh cap = buildCap(slice.contours, plane);
    const double polygon = 0.5 * segments * 25.0 * std::sin(2.0 * std::numbers::pi / segments);
    EXPECT_NEAR(cap.area(), polygon, 1e-9);

    // Lengthwise
    CutPlane lengthwise(Vector3d(0, 0, 0), Vector3d(0, 1, 0));
    slice = sliceMesh(mesh, lengthwise, 1e-6);
    ASSERT_EQ(slice.contours.size(), 1U);
    cap = buildCap(slice.contours, lengthwise);
    EXPECT_NEAR(cap.area(), 10.0 * 20.0, 1e-9);
}

TEST(SectionGeometry, sliceBridgesSmallCracks)
{
    // Shift one face so its vertices don't weld with the neighbours, like a
    // tessellation with slightly different edge discretization
    auto soup = boxSoup(Vector3d(0, 0, 0), Vector3d(1, 1, 1));
    for (std::size_t i = 18; i < 24; ++i) {  // the right face
        soup[i].x += 1e-6;
    }
    IndexedMesh mesh = weld(soup);
    CutPlane plane(Vector3d(0, 0, 0.5), Vector3d(0, 0, 1));

    SliceResult slice = sliceMesh(mesh, plane, 1e-9);
    EXPECT_GT(slice.openContours, 0U);

    slice = sliceMesh(mesh, plane, 1e-4);
    EXPECT_EQ(slice.openContours, 0U);
    ASSERT_EQ(slice.contours.size(), 1U);
    CapMesh cap = buildCap(slice.contours, plane);
    EXPECT_NEAR(cap.area(), 1.0, 1e-5);
}

TEST(SectionGeometry, openMeshIsNotCapped)
{
    // A box without its top
    auto soup = boxSoup(Vector3d(0, 0, 0), Vector3d(1, 1, 1));
    soup.erase(soup.begin() + 6, soup.begin() + 12);
    IndexedMesh mesh = weld(soup);

    CutPlane plane(Vector3d(0, 0.5, 0), Vector3d(0, 1, 0));
    SliceResult slice = sliceMesh(mesh, plane, 1e-6);
    EXPECT_EQ(slice.openContours, 1U);
    CapMesh cap = buildCap(slice.contours, plane);
    EXPECT_TRUE(cap.empty());
}

TEST(SectionGeometry, defaultWeldTolerance)
{
    Base::BoundBox3d box(0, 0, 0, 100, 100, 100);
    const double tol = defaultWeldTolerance(box);
    EXPECT_GT(tol, 0.0);
    EXPECT_LT(tol, 1e-2);
    EXPECT_GT(defaultWeldTolerance(Base::BoundBox3d()), 0.0);
}
