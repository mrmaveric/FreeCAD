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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <thread>
#include <numbers>

#include <QPointer>
#include <QTimer>

#include <Inventor/SbViewVolume.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/SoFullPath.h>
#include <Inventor/SoPrimitiveVertex.h>
#include <Inventor/actions/SoCallbackAction.h>
#include <Inventor/details/SoDetail.h>
#include <Inventor/details/SoFaceDetail.h>
#include <Inventor/elements/SoDrawStyleElement.h>
#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoClipPlane.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoNormal.h>
#include <Inventor/nodes/SoNormalBinding.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoPolygonOffset.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoShape.h>
#include <Inventor/nodes/SoShapeHints.h>
#include <Inventor/sensors/SoNodeSensor.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>

#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/MDIView.h>
#include <Gui/Selection/SoFCSelectionContext.h>
#include <Gui/Selection/SoFCUnifiedSelection.h>
#include <Gui/SplitView3DInventor.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Gui/ViewProvider.h>
#include <Gui/ViewProviderDocumentObject.h>

#include "SectionScene.h"
#include "SoFCSectionNodes.h"

FC_LOG_LEVEL_INIT("Section", true, true)

using namespace Gui;
using namespace Gui::Section;

namespace
{
/// Runs func(i) for i in [0, count) on all cores
template<typename Func>
void parallelFor(std::size_t count, Func&& func)
{
    const std::size_t threads
        = std::min<std::size_t>(std::max(1U, std::thread::hardware_concurrency()), count / 2);
    if (threads <= 1) {
        for (std::size_t i = 0; i < count; ++i) {
            func(i);
        }
        return;
    }

    std::atomic<std::size_t> next {0};
    auto worker = [&]() {
        for (std::size_t i = next++; i < count; i = next++) {
            try {
                func(i);
            }
            catch (...) {
                // A failing instance just isn't capped
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (std::size_t t = 1; t < threads; ++t) {
        pool.emplace_back(worker);
    }
    worker();
    for (auto& thread : pool) {
        thread.join();
    }
}

std::map<const View3DInventorViewer*, SectionScene*>& attachedScenes()
{
    static std::map<const View3DInventorViewer*, SectionScene*> scenes;
    return scenes;
}

std::set<SectionScene*>& allScenes()
{
    static std::set<SectionScene*> scenes;
    return scenes;
}

SbVec3f toSb(const Base::Vector3d& vec)
{
    return {static_cast<float>(vec.x), static_cast<float>(vec.y), static_cast<float>(vec.z)};
}

/// Rounds to 1, 2 or 5 times a power of ten
double niceNumber(double value)
{
    if (value <= 0.0 || !std::isfinite(value)) {
        return 1.0;
    }
    const double exponent = std::floor(std::log10(value));
    const double base = std::pow(10.0, exponent);
    const double fraction = value / base;
    double nice = 10.0;
    if (fraction < 1.5) {
        nice = 1.0;
    }
    else if (fraction < 3.5) {
        nice = 2.0;
    }
    else if (fraction < 7.5) {
        nice = 5.0;
    }
    return nice * base;
}

/// A color for lines drawn on top of a fill color
SbColor contrastColor(const Base::Color& fill)
{
    const float luminance = 0.299F * fill.r + 0.587F * fill.g + 0.114F * fill.b;
    if (luminance > 0.3F) {
        const float factor = 0.35F;
        return {fill.r * factor, fill.g * factor, fill.b * factor};
    }
    const float mix = 0.55F;
    return {
        fill.r + (1.0F - fill.r) * mix,
        fill.g + (1.0F - fill.g) * mix,
        fill.b + (1.0F - fill.b) * mix
    };
}

/// Geometry of an instance, kept between collections
struct CachedBucket
{
    SoFCSectionFilter::Stack stack;
    std::shared_ptr<const IndexedMesh> mesh;
    double gapTolerance {0.0};
    /// Material colors and their frequency
    std::map<uint32_t, std::size_t> colors;
    /// Color override of a link, if any, and the selection root applying it
    const SoNode* overrideRoot {nullptr};
    uint32_t overrideColor {0};
};

/// Geometry below a selection root, valid as long as the node didn't change
struct CacheEntry
{
    SbUniqueId nodeId {0};
    SbMatrix matrix;
    std::vector<std::shared_ptr<const CachedBucket>> buckets;
};

/// Keyed by instance, as colors may depend on the path through links
using GeometryCache = std::map<SoFCSectionFilter::Stack, CacheEntry>;

/** Collects the triangles of the displayed scene, grouped by instance.
 *
 * Sub-trees that didn't change since the last collection are taken from the
 * cache. Their meshes are only transformed if the instance moved, which keeps
 * moving parts of a big assembly cheap.
 */
class TriangleCollector
{
public:
    struct Result
    {
        SoFCSectionFilter::Stack stack;
        // New geometry
        std::vector<Base::Vector3d> soup;
        std::map<uint32_t, std::size_t> colors;
        const SoNode* overrideRoot {nullptr};
        uint32_t overrideColor {0};
        // Geometry from the cache
        std::shared_ptr<const CachedBucket> cached;
        SbMatrix transform;
        bool moved {false};
    };

    struct Frame
    {
        SoFCSectionFilter::Stack stack;
        SbUniqueId nodeId;
        SbMatrix matrix;
        std::size_t first;
        std::size_t last {0};
    };

    std::vector<Result> results;
    std::vector<Frame> frames;

    explicit TriangleCollector(const GeometryCache& cache)
        : cache(cache)
    {}

    void collect(SoNode* root, const SbViewportRegion& viewport)
    {
        SoCallbackAction action(viewport);
        action.addPreCallback(SoFCSelectionRoot::getClassTypeId(), preRoot, this);
        action.addPostCallback(SoFCSelectionRoot::getClassTypeId(), postRoot, this);
        action.addTriangleCallback(SoShape::getClassTypeId(), triangle, this);
        action.apply(root);
    }

    /// Final color of a result, taking link color overrides into account
    static void getColor(const Result& result, Base::Color& color, float& transparency)
    {
        uint32_t packed = 0;
        if (result.overrideRoot) {
            packed = result.overrideColor;
        }
        else {
            const auto& colors = result.cached ? result.cached->colors : result.colors;
            std::size_t best = 0;
            for (const auto& [value, count] : colors) {
                if (count > best) {
                    best = count;
                    packed = value;
                }
            }
        }
        SbColor sbColor;
        sbColor.setPackedValue(packed, transparency);
        color = Base::Color(sbColor[0], sbColor[1], sbColor[2]);
    }

private:
    const GeometryCache& cache;
    SoFCSectionFilter::Stack stack;
    std::map<SoFCSectionFilter::Stack, std::size_t> resultIndex;
    Result* current {nullptr};
    uint32_t counter {0};
    // Color override of links, applied by the outermost selection root while rendering
    const SoFCSelectionRoot* overrideRoot {nullptr};
    std::vector<std::size_t> openFrames;
    SoNode* lastShape {nullptr};
    std::shared_ptr<SoFCSelectionContextEx> shapeContext;

    static uint32_t packedOverride(const SoFCSelectionRoot* root)
    {
        const Base::Color color = root->getColorOverride();
        return SbColor(color.r, color.g, color.b).getPackedValue(1.0F - color.a);
    }

    static bool sameMatrix(const SbMatrix& a, const SbMatrix& b)
    {
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                if (std::abs(a[i][j] - b[i][j]) > 1e-6F * (1.0F + std::abs(a[i][j]))) {
                    return false;
                }
            }
        }
        return true;
    }

    static SoCallbackAction::Response preRoot(void* data, SoCallbackAction* action, const SoNode* node)
    {
        auto self = static_cast<TriangleCollector*>(data);
        self->stack.push_back(const_cast<SoNode*>(node));  // NOLINT
        auto root = static_cast<const SoFCSelectionRoot*>(node);
        if (!self->overrideRoot && root->hasColorOverride()) {
            self->overrideRoot = root;
        }
        self->current = nullptr;
        self->lastShape = nullptr;

        const SbMatrix& matrix = action->getModelMatrix();
        auto it = self->cache.find(self->stack);
        if (it != self->cache.end() && it->second.nodeId == node->getNodeId()) {
            const std::size_t first = self->results.size();
            self->replay(it->second, matrix);
            self->frames.push_back(
                {self->stack, node->getNodeId(), matrix, first, self->results.size()}
            );
            return SoCallbackAction::PRUNE;
        }

        self->openFrames.push_back(self->frames.size());
        self->frames.push_back({self->stack, node->getNodeId(), matrix, self->results.size()});
        return SoCallbackAction::CONTINUE;
    }

    static SoCallbackAction::Response postRoot(void* data, SoCallbackAction*, const SoNode* node)
    {
        auto self = static_cast<TriangleCollector*>(data);
        if (!self->stack.empty() && self->stack.back() == node) {
            self->stack.pop_back();
        }
        if (self->overrideRoot == node) {
            self->overrideRoot = nullptr;
        }
        if (!self->openFrames.empty()
            && self->frames[self->openFrames.back()].stack.size() == self->stack.size() + 1) {
            self->frames[self->openFrames.back()].last = self->results.size();
            self->openFrames.pop_back();
        }
        self->current = nullptr;
        return SoCallbackAction::CONTINUE;
    }

    void replay(const CacheEntry& entry, const SbMatrix& matrix)
    {
        const bool moved = !sameMatrix(matrix, entry.matrix);
        // Cached geometry is in world coordinates of the time it was collected
        const SbMatrix transform = moved ? entry.matrix.inverse() * matrix : SbMatrix::identity();
        for (const auto& bucket : entry.buckets) {
            Result result;
            result.cached = bucket;
            result.moved = moved;
            result.transform = transform;
            result.stack = bucket->stack;
            result.overrideRoot = bucket->overrideRoot;
            result.overrideColor = bucket->overrideColor;
            results.push_back(std::move(result));
        }
    }

    Result& result()
    {
        if (!current) {
            auto [it, inserted] = resultIndex.try_emplace(stack, results.size());
            if (inserted) {
                results.emplace_back();
                results.back().stack = stack;
            }
            current = &results[it->second];
        }
        return *current;
    }

    static void triangle(
        void* data,
        SoCallbackAction* action,
        const SoPrimitiveVertex* v1,
        const SoPrimitiveVertex* v2,
        const SoPrimitiveVertex* v3
    )
    {
        auto self = static_cast<TriangleCollector*>(data);
        if (SoDrawStyleElement::get(action->getState()) != SoDrawStyleElement::FILLED) {
            return;
        }

        Result& result = self->result();
        const SbMatrix& matrix = action->getModelMatrix();
        for (const SoPrimitiveVertex* vertex : {v1, v2, v3}) {
            SbVec3f pnt;
            matrix.multVecMatrix(vertex->getPoint(), pnt);
            result.soup.emplace_back(pnt[0], pnt[1], pnt[2]);
        }

        if (self->overrideRoot && !result.overrideRoot) {
            result.overrideRoot = self->overrideRoot;
            result.overrideColor = packedOverride(self->overrideRoot);
        }

        // Sample the material now and then to find the dominant color
        if ((self->counter++ & 15U) != 0) {
            return;
        }

        // Colors set per face or by links are applied while rendering
        SoNode* shape = action->getCurPathTail();
        if (shape != self->lastShape) {
            self->lastShape = shape;
            self->shapeContext
                = SoFCSelectionRoot::getSecondaryActionContext<SoFCSelectionContextEx>(action, shape);
        }
        if (self->shapeContext) {
            int part = -1;
            if (auto detail = v1->getDetail();
                detail && detail->isOfType(SoFaceDetail::getClassTypeId())) {
                part = static_cast<const SoFaceDetail*>(detail)->getPartIndex();
            }
            std::vector<uint32_t> packed;
            bool transparent = false;
            if (self->shapeContext->applyColor(part, packed, transparent)) {
                ++result.colors[packed.back()];
                return;
            }
        }

        SbColor ambient;
        SbColor diffuse;
        SbColor specular;
        SbColor emission;
        float shininess = 0.0F;
        float transparency = 0.0F;
        action->getMaterial(
            ambient,
            diffuse,
            specular,
            emission,
            shininess,
            transparency,
            v1->getMaterialIndex()
        );
        ++result.colors[diffuse.getPackedValue(transparency)];
    }
};

}  // namespace

// ----------------------------------------------------------------------------

std::vector<Point2d> Gui::Section::hatchRings(
    const std::vector<std::vector<Point2d>>& rings,
    double angle,
    double spacing,
    double phase
)
{
    std::vector<Point2d> segments;
    if (spacing <= 0.0 || !std::isfinite(spacing)) {
        return segments;
    }

    // Rotate so that the hatch lines become horizontal
    const double cosA = std::cos(angle);
    const double sinA = std::sin(angle);

    struct Edge
    {
        double y0;
        double y1;
        double x0;
        double slope;
    };
    std::vector<Edge> edges;
    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    for (const auto& ring : rings) {
        const std::size_t count = ring.size();
        for (std::size_t i = 0; i < count; ++i) {
            const auto& p = ring[i];
            const auto& q = ring[(i + 1) % count];
            Point2d a {p[0] * cosA + p[1] * sinA, -p[0] * sinA + p[1] * cosA};
            Point2d b {q[0] * cosA + q[1] * sinA, -q[0] * sinA + q[1] * cosA};
            if (a[1] == b[1]) {
                continue;
            }
            if (a[1] > b[1]) {
                std::swap(a, b);
            }
            edges.push_back({a[1], b[1], a[0], (b[0] - a[0]) / (b[1] - a[1])});
            minY = std::min(minY, a[1]);
            maxY = std::max(maxY, b[1]);
        }
    }
    if (edges.empty()) {
        return segments;
    }

    // Don't produce an unreasonable amount of lines
    constexpr double maxLines = 20000.0;
    if ((maxY - minY) / spacing > maxLines) {
        return segments;
    }

    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });

    std::vector<const Edge*> active;
    std::vector<double> crossings;
    std::size_t next = 0;
    const auto first = static_cast<int64_t>(std::ceil(minY / spacing - phase));
    for (int64_t k = first;; ++k) {
        const double y = (static_cast<double>(k) + phase) * spacing;
        if (y >= maxY) {
            break;
        }
        while (next < edges.size() && edges[next].y0 <= y) {
            active.push_back(&edges[next++]);
        }
        std::erase_if(active, [y](const Edge* edge) { return edge->y1 <= y; });

        crossings.clear();
        for (const Edge* edge : active) {
            if (y >= edge->y0 && y < edge->y1) {
                crossings.push_back(edge->x0 + (y - edge->y0) * edge->slope);
            }
        }
        std::sort(crossings.begin(), crossings.end());
        for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
            for (double x : {crossings[i], crossings[i + 1]}) {
                segments.push_back({x * cosA - y * sinA, x * sinA + y * cosA});
            }
        }
    }
    return segments;
}

// ----------------------------------------------------------------------------

class SectionScene::Private
{
public:
    struct Instance
    {
        SoFCSectionFilter::Stack stack;
        App::SubObjectT owner;
        std::shared_ptr<const IndexedMesh> mesh;
        double gapTolerance {0.0};
        Base::Color color;
        float transparency {0.0F};
        bool sectioned {true};
    };

    /// The cap of an instance on one plane
    struct Cap
    {
        std::size_t instance {0};
        std::size_t plane {0};
        CapMesh mesh;
        bool open {false};
        CoinPtr<SoSeparator> hatchGroup;
    };

    Gui::Document* document;
    std::vector<CutPlane> planes;
    Combination combination {Combination::RemoveCommon};
    DisplaySettings settings;

    std::vector<App::SubObjectT> filterItems;
    bool sectionListed {false};
    std::shared_ptr<SoFCSectionFilter> filter;

    std::vector<Instance> instances;
    GeometryCache geometryCache;
    Base::BoundBox3d boundBox;
    bool geometryValid {false};
    std::vector<Cap> caps;
    std::map<const SoNode*, std::size_t> pickMap;
    int hatchLevel {0};

    CoinPtr<SoSeparator> capsRoot;
    std::vector<CoinPtr<SoClipPlane>> keptPlanes;
    std::vector<CoinPtr<SoClipPlane>> removedPlanes;

    std::vector<QPointer<View3DInventorViewer>> viewers;
    bool attached {false};

    QTimer geometryTimer;
    std::unique_ptr<SoNodeSensor> cameraSensor;
    QPointer<View3DInventorViewer> cameraViewer;

    fastsignals::scoped_connection connectChangedObject;
    fastsignals::scoped_connection connectNewObject;
    fastsignals::scoped_connection connectDeletedObject;
    fastsignals::scoped_connection connectChangedViewObject;
    fastsignals::scoped_connection connectActivateView;

    SectionScene* owner;

    Private(SectionScene* owner, Gui::Document* doc)
        : document(doc)
        , owner(owner)
    {
        capsRoot = new SoSeparator;
        capsRoot->setName("SectionCaps");

        geometryTimer.setSingleShot(true);
        geometryTimer.setInterval(0);
        QObject::connect(&geometryTimer, &QTimer::timeout, [this]() {
            if (attached) {
                updateGeometry();
            }
        });

        cameraSensor = std::make_unique<SoNodeSensor>(cameraChanged, this);
    }

    ~Private()
    {
        cameraSensor->detach();
    }

    // ---- viewers -----------------------------------------------------------

    std::vector<View3DInventorViewer*> documentViewers() const
    {
        std::vector<View3DInventorViewer*> result;
        if (!document) {
            return result;
        }
        for (auto view : document->getMDIViewsOfType(View3DInventor::getClassTypeId())) {
            result.push_back(static_cast<View3DInventor*>(view)->getViewer());
        }
        for (auto view : document->getMDIViewsOfType(AbstractSplitView::getClassTypeId())) {
            auto split = static_cast<AbstractSplitView*>(view);
            for (int i = 0; i < split->getSize(); ++i) {
                result.push_back(split->getViewer(i));
            }
        }
        return result;
    }

    View3DInventorViewer* primaryViewer() const
    {
        for (const auto& viewer : viewers) {
            if (viewer) {
                return viewer;
            }
        }
        return nullptr;
    }

    std::vector<SoFCSectionRoot::PassSetup> passSetups() const
    {
        std::vector<SoFCSectionRoot::PassSetup> passes;
        const std::size_t count = planes.size();
        if (count == 0) {
            passes.emplace_back();
        }
        else if (combination == Combination::KeepCommon || count == 1) {
            SoFCSectionRoot::PassSetup pass;
            for (const auto& plane : keptPlanes) {
                pass.planes.push_back(plane);
            }
            passes.push_back(pass);
        }
        else {
            // The kept region is the union of the kept half-spaces. Render it as
            // disjoint pieces so transparent objects aren't drawn twice.
            for (std::size_t k = 0; k < count; ++k) {
                SoFCSectionRoot::PassSetup pass;
                pass.planes.push_back(keptPlanes[k]);
                for (std::size_t j = 0; j < k; ++j) {
                    pass.planes.push_back(removedPlanes[j]);
                }
                passes.push_back(pass);
            }
        }

        if (filter && !filter->empty()) {
            SoFCSectionRoot::PassSetup unclipped;
            unclipped.mode = SoFCSectionPass::Mode::Unclipped;
            passes.push_back(unclipped);
        }
        return passes;
    }

    void attachViewer(View3DInventorViewer* viewer)
    {
        if (!viewer || !viewer->getSectionRoot()) {
            return;
        }
        auto& scenes = attachedScenes();
        auto it = scenes.find(viewer);
        if (it != scenes.end() && it->second != owner) {
            // Another section is active in this view
            it->second->d->detachViewer(viewer);
        }
        scenes[viewer] = owner;
        viewer->getSectionRoot()->setSection(passSetups(), filter, capsRoot);
        if (std::find(viewers.begin(), viewers.end(), viewer) == viewers.end()) {
            viewers.emplace_back(viewer);
        }
        // Forget the viewer when it goes away, once per viewer
        static std::set<const View3DInventorViewer*> watched;
        if (watched.insert(viewer).second) {
            QObject::connect(viewer, &QObject::destroyed, [viewer]() {
                attachedScenes().erase(viewer);
                watched.erase(viewer);
            });
        }
    }

    void detachViewer(View3DInventorViewer* viewer)
    {
        auto& scenes = attachedScenes();
        auto it = scenes.find(viewer);
        if (it != scenes.end() && it->second == owner) {
            scenes.erase(it);
            if (viewer->getSectionRoot()) {
                viewer->getSectionRoot()->clearSection();
            }
        }
        std::erase_if(viewers, [viewer](const auto& ptr) { return !ptr || ptr == viewer; });
        if (cameraViewer == viewer) {
            cameraSensor->detach();
            cameraViewer = nullptr;
        }
    }

    void updatePasses()
    {
        if (!attached) {
            return;
        }
        for (const auto& viewer : viewers) {
            if (viewer) {
                viewer->getSectionRoot()->setSection(passSetups(), filter, capsRoot);
            }
        }
    }

    void trackCamera(View3DInventorViewer* viewer)
    {
        if (cameraViewer == viewer && cameraSensor->getAttachedNode()) {
            return;
        }
        cameraSensor->detach();
        cameraViewer = viewer;
        if (viewer && viewer->getSoRenderManager()->getCamera()) {
            cameraSensor->attach(viewer->getSoRenderManager()->getCamera());
        }
    }

    static void cameraChanged(void* data, SoSensor*)
    {
        auto self = static_cast<Private*>(data);
        if (self->settings.adaptiveHatch && self->computeHatchLevel() != self->hatchLevel) {
            self->rebuildHatch();
        }
    }

    // ---- geometry ----------------------------------------------------------

    void updateGeometry()
    {
        geometryTimer.stop();
        View3DInventorViewer* viewer = primaryViewer();
        instances.clear();
        boundBox = Base::BoundBox3d();
        if (!viewer) {
            geometryValid = false;
            return;
        }

        const auto timeStart = std::chrono::steady_clock::now();
        TriangleCollector collector(geometryCache);
        collector.collect(viewer->getObjectGroup(), viewer->getSoRenderManager()->getViewportRegion());
        auto& results = collector.results;

        // Weld new geometry and move cached geometry in parallel
        std::vector<std::shared_ptr<const CachedBucket>> buckets(results.size());
        parallelFor(results.size(), [&](std::size_t k) {
            auto& result = results[k];
            if (result.cached && !result.moved) {
                buckets[k] = result.cached;
                return;
            }

            auto bucket = std::make_shared<CachedBucket>();
            bucket->stack = result.stack;
            bucket->overrideRoot = result.overrideRoot;
            bucket->overrideColor = result.overrideColor;
            if (result.cached) {
                bucket->colors = result.cached->colors;
                bucket->gapTolerance = result.cached->gapTolerance;
                auto mesh = std::make_shared<IndexedMesh>(*result.cached->mesh);
                mesh->boundBox = Base::BoundBox3d();
                for (auto& pnt : mesh->points) {
                    SbVec3f src(
                        static_cast<float>(pnt.x),
                        static_cast<float>(pnt.y),
                        static_cast<float>(pnt.z)
                    );
                    SbVec3f dst;
                    result.transform.multVecMatrix(src, dst);
                    pnt = Base::Vector3d(dst[0], dst[1], dst[2]);
                    mesh->boundBox.Add(pnt);
                }
                bucket->mesh = std::move(mesh);
            }
            else {
                bucket->colors = result.colors;
                if (result.soup.size() >= 3) {
                    Base::BoundBox3d box;
                    for (const auto& pnt : result.soup) {
                        box.Add(pnt);
                    }
                    const double tolerance = defaultWeldTolerance(box);
                    bucket->mesh = std::make_shared<IndexedMesh>(
                        weldTriangleSoup(result.soup, tolerance)
                    );
                    bucket->gapTolerance = tolerance * 100.0;
                }
                else {
                    bucket->mesh = std::make_shared<IndexedMesh>();
                }
                // Free the soup early, it can be big
                std::vector<Base::Vector3d>().swap(result.soup);
            }
            buckets[k] = std::move(bucket);
        });

        // Remember the geometry of every selection root for the next time
        GeometryCache cache;
        for (const auto& frame : collector.frames) {
            CacheEntry entry;
            entry.nodeId = frame.nodeId;
            entry.matrix = frame.matrix;
            for (std::size_t k = frame.first; k < frame.last && k < buckets.size(); ++k) {
                entry.buckets.push_back(buckets[k]);
            }
            cache[frame.stack] = std::move(entry);
        }
        geometryCache = std::move(cache);

        for (std::size_t k = 0; k < results.size(); ++k) {
            const auto& bucket = buckets[k];
            if (!bucket || !bucket->mesh || bucket->mesh->empty()) {
                continue;
            }
            Instance instance;
            instance.stack = results[k].stack;
            instance.owner = resolveOwner(instance.stack);
            instance.mesh = bucket->mesh;
            instance.gapTolerance = bucket->gapTolerance;
            TriangleCollector::getColor(results[k], instance.color, instance.transparency);
            boundBox.Add(instance.mesh->boundBox);
            instances.push_back(std::move(instance));
        }

        geometryValid = true;
        applyFilter();
        rebuildCaps();
        if (FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG)) {
            std::size_t reused = 0;
            for (const auto& result : results) {
                reused += result.cached ? 1 : 0;
            }
            const auto duration = std::chrono::steady_clock::now() - timeStart;
            FC_LOG(
                "Collected " << results.size() << " instances, " << reused << " from cache, in "
                             << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()
                             << " ms"
            );
        }
    }

    std::vector<SoNode*> resolveStack(const App::SubObjectT& item) const
    {
        std::vector<SoNode*> stack;
        App::DocumentObject* obj = item.getObject();
        auto vp = freecad_cast<ViewProviderDocumentObject*>(
            Application::Instance->getViewProvider(obj)
        );
        if (!vp) {
            return stack;
        }

        CoinPtr<SoFullPath> path(static_cast<SoFullPath*>(new SoPath(10)));
        SoDetail* detail = nullptr;
        const std::string subname = item.getSubNameNoElement();
        if (vp->getDetailPath(subname.c_str(), path, true, detail)) {
            for (int i = 0; i < path->getLength(); ++i) {
                SoNode* node = path->getNode(i);
                if (node->isOfType(SoFCSelectionRoot::getClassTypeId())) {
                    stack.push_back(node);
                }
            }
        }
        delete detail;
        return stack;
    }

    void applyFilter()
    {
        auto newFilter = std::make_shared<SoFCSectionFilter>();
        newFilter->sectionListed = sectionListed;
        for (const auto& item : filterItems) {
            auto stack = resolveStack(item);
            if (!stack.empty()) {
                newFilter->addInstance(std::move(stack));
            }
        }

        if (newFilter->empty() && !sectionListed) {
            newFilter.reset();
        }
        // Keep the passes untouched if nothing changed
        const bool same = (!filter && !newFilter) || (filter && newFilter && *filter == *newFilter);
        if (!same) {
            filter = newFilter;
        }

        for (auto& instance : instances) {
            instance.sectioned = !filter || filter->isSectioned(instance.stack);
        }
        updatePasses();
    }

    // ---- caps --------------------------------------------------------------

    double baseHatchSpacing() const
    {
        if (settings.hatchSpacing > 0.0) {
            return settings.hatchSpacing;
        }
        if (!boundBox.IsValid()) {
            return 1.0;
        }
        return niceNumber(boundBox.CalcDiagonalLength() / 100.0);
    }

    /// Pixels per model unit at the center of the model in the tracked view
    double pixelsPerUnit() const
    {
        View3DInventorViewer* viewer = cameraViewer;
        if (!viewer) {
            viewer = primaryViewer();
        }
        if (!viewer || !boundBox.IsValid()) {
            return 0.0;
        }
        SoCamera* camera = viewer->getSoRenderManager()->getCamera();
        if (!camera) {
            return 0.0;
        }
        const SbViewportRegion& region = viewer->getSoRenderManager()->getViewportRegion();
        const SbViewVolume volume = camera->getViewVolume(region.getViewportAspectRatio());
        SbVec3f up;
        camera->orientation.getValue().multVec(SbVec3f(0, 1, 0), up);
        const SbVec3f center = toSb(boundBox.GetCenter());
        SbVec3f s0;
        SbVec3f s1;
        volume.projectToScreen(center, s0);
        volume.projectToScreen(center + up, s1);
        return std::abs(s1[1] - s0[1]) * region.getViewportSizePixels()[1];
    }

    int computeHatchLevel() const
    {
        if (!settings.adaptiveHatch) {
            return 0;
        }
        const double ppu = pixelsPerUnit();
        if (ppu <= 0.0) {
            return hatchLevel;
        }
        // Keep the lines between 9 and 18 pixels apart
        constexpr double minPixels = 9.0;
        const double pixels = baseHatchSpacing() * ppu;
        const int level = static_cast<int>(std::ceil(std::log2(minPixels / pixels)));
        return std::clamp(level, -8, 16);
    }

    void rebuildCaps()
    {
        capsRoot->removeAllChildren();
        caps.clear();
        pickMap.clear();

        // Clip planes are kept in sync even without caps
        while (keptPlanes.size() > planes.size()) {
            keptPlanes.pop_back();
            removedPlanes.pop_back();
        }
        bool structureChanged = false;
        while (keptPlanes.size() < planes.size()) {
            keptPlanes.emplace_back(new SoClipPlane);
            removedPlanes.emplace_back(new SoClipPlane);
            structureChanged = true;
        }
        for (std::size_t i = 0; i < planes.size(); ++i) {
            const auto& plane = planes[i];
            keptPlanes[i]->plane.setValue(
                SbPlane(toSb(plane.normal), static_cast<float>(plane.distance))
            );
            const CutPlane flipped = plane.flipped();
            removedPlanes[i]->plane.setValue(
                SbPlane(toSb(flipped.normal), static_cast<float>(flipped.distance))
            );
        }
        if (structureChanged) {
            updatePasses();
        }

        if (!settings.showCaps || !geometryValid) {
            return;
        }

        auto hints = new SoShapeHints;
        hints->vertexOrdering = SoShapeHints::COUNTERCLOCKWISE;
        hints->shapeType = SoShapeHints::UNKNOWN_SHAPE_TYPE;
        hints->faceType = SoShapeHints::CONVEX;
        capsRoot->addChild(hints);

        for (std::size_t p = 0; p < planes.size(); ++p) {
            buildPlaneCaps(p);
        }
        hatchLevel = computeHatchLevel();
        rebuildHatch();
    }

    void buildPlaneCaps(std::size_t planeIndex)
    {
        const CutPlane& plane = planes[planeIndex];

        auto group = new SoSeparator;
        // A cap only exists where the other planes cut material away (corner)
        // or keep it (slab, box)
        for (std::size_t j = 0; j < planes.size(); ++j) {
            if (j != planeIndex) {
                group->addChild(
                    combination == Combination::KeepCommon ? keptPlanes[j] : removedPlanes[j]
                );
            }
        }

        auto faces = new SoSeparator;
        auto offset = new SoPolygonOffset;
        offset->factor = 1.0F;
        offset->units = 1.0F;
        faces->addChild(offset);
        auto normalBinding = new SoNormalBinding;
        normalBinding->value = SoNormalBinding::OVERALL;
        faces->addChild(normalBinding);
        auto normal = new SoNormal;
        normal->vector.setValue(toSb(-plane.normal));
        faces->addChild(normal);

        auto lines = new SoSeparator;
        auto lightModel = new SoLightModel;
        lightModel->model = SoLightModel::BASE_COLOR;
        lines->addChild(lightModel);
        auto pickStyle = new SoPickStyle;
        pickStyle->style = SoPickStyle::UNPICKABLE;
        lines->addChild(pickStyle);

        auto hatchStyle = new SoDrawStyle;
        hatchStyle->lineWidth = settings.hatchLineWidth;
        lines->addChild(hatchStyle);
        auto hatchLines = new SoSeparator;
        lines->addChild(hatchLines);

        auto outlineStyle = new SoDrawStyle;
        outlineStyle->lineWidth = settings.outlineWidth;
        auto outlineColor = new SoBaseColor;
        outlineColor->rgb
            .setValue(settings.outlineColor.r, settings.outlineColor.g, settings.outlineColor.b);
        auto outlines = new SoSeparator;
        outlines->addChild(outlineStyle);
        outlines->addChild(outlineColor);
        if (settings.showOutline) {
            lines->addChild(outlines);
        }

        // Cutting is independent per instance, so do it in parallel
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < instances.size(); ++i) {
            const Instance& instance = instances[i];
            if (instance.sectioned && plane.classify(instance.mesh->boundBox) == 0) {
                candidates.push_back(i);
            }
        }
        std::vector<Cap> planeCaps(candidates.size());
        parallelFor(candidates.size(), [&](std::size_t k) {
            const Instance& instance = instances[candidates[k]];
            SliceResult slice = sliceMesh(*instance.mesh, plane, instance.gapTolerance);
            Cap& cap = planeCaps[k];
            cap.instance = candidates[k];
            cap.plane = planeIndex;
            cap.open = slice.openContours > 0;
            cap.mesh = buildCap(slice.contours, plane);
        });

        for (auto& cap : planeCaps) {
            const std::size_t i = cap.instance;
            const Instance& instance = instances[i];
            if (cap.mesh.empty()) {
                if (cap.open) {
                    caps.push_back(std::move(cap));
                }
                continue;
            }

            auto coords = new SoCoordinate3;
            coords->point.setNum(static_cast<int>(cap.mesh.vertices.size()));
            SbVec3f* pts = coords->point.startEditing();
            for (std::size_t k = 0; k < cap.mesh.vertices.size(); ++k) {
                pts[k] = toSb(cap.mesh.vertices[k]);
            }
            coords->point.finishEditing();

            // Faces
            auto faceSep = new SoSeparator;
            auto material = new SoMaterial;
            const Base::Color fill = settings.fillColor == DisplaySettings::FillColor::Uniform
                ? settings.uniformColor
                : instance.color;
            material->diffuseColor.setValue(fill.r, fill.g, fill.b);
            material->transparency = instance.transparency;
            faceSep->addChild(material);
            faceSep->addChild(coords);
            auto faceSet = new SoIndexedFaceSet;
            const std::size_t numTriangles = cap.mesh.triangles.size() / 3;
            faceSet->coordIndex.setNum(static_cast<int>(numTriangles * 4));
            int32_t* indices = faceSet->coordIndex.startEditing();
            for (std::size_t t = 0; t < numTriangles; ++t) {
                indices[4 * t] = static_cast<int32_t>(cap.mesh.triangles[3 * t]);
                indices[4 * t + 1] = static_cast<int32_t>(cap.mesh.triangles[3 * t + 1]);
                indices[4 * t + 2] = static_cast<int32_t>(cap.mesh.triangles[3 * t + 2]);
                indices[4 * t + 3] = SO_END_FACE_INDEX;
            }
            faceSet->coordIndex.finishEditing();
            faceSep->addChild(faceSet);
            faces->addChild(faceSep);
            pickMap[faceSet] = i;

            // Outline
            auto outlineSep = new SoSeparator;
            outlineSep->addChild(coords);
            auto lineSet = new SoIndexedLineSet;
            std::vector<int32_t> lineIndices;
            for (const auto& loop : cap.mesh.outlines) {
                for (uint32_t idx : loop) {
                    lineIndices.push_back(static_cast<int32_t>(idx));
                }
                lineIndices.push_back(static_cast<int32_t>(loop.front()));
                lineIndices.push_back(SO_END_LINE_INDEX);
            }
            lineSet->coordIndex.setValues(0, static_cast<int>(lineIndices.size()), lineIndices.data());
            outlineSep->addChild(lineSet);
            outlines->addChild(outlineSep);

            // Hatch, filled in by rebuildHatch()
            cap.hatchGroup = new SoSeparator;
            hatchLines->addChild(cap.hatchGroup);

            caps.push_back(std::move(cap));
        }

        group->addChild(faces);
        group->addChild(lines);
        capsRoot->addChild(group);
    }

    void rebuildHatch()
    {
        hatchLevel = computeHatchLevel();
        const double spacing = baseHatchSpacing() * std::pow(2.0, hatchLevel);
        const double baseAngle = settings.hatchAngle * std::numbers::pi / 180.0;
        constexpr std::array<double, 3> spacingFactors {1.0, 0.7, 1.4};

        for (auto& cap : caps) {
            if (cap.hatchGroup) {
                cap.hatchGroup->removeAllChildren();
            }
        }
        if (settings.hatch == DisplaySettings::Hatch::None) {
            return;
        }

        // Compute the lines in parallel, then build the nodes
        std::vector<std::vector<Point2d>> segments(caps.size());
        parallelFor(caps.size(), [&](std::size_t k) {
            const Cap& cap = caps[k];
            if (!cap.hatchGroup) {
                return;
            }
            const CutPlane& plane = planes[cap.plane];

            // Neighboring objects get different patterns. The pattern only depends
            // on the object, so it doesn't change while the plane moves.
            const std::size_t pattern = cap.instance;
            double angle = baseAngle;
            double instSpacing = spacing;
            double phase = 0.0;
            if (settings.varyHatch) {
                angle += static_cast<double>(pattern % 2) * std::numbers::pi / 2.0;
                instSpacing *= spacingFactors[(pattern / 2) % spacingFactors.size()];
                phase = std::fmod(static_cast<double>(pattern) * 0.618034, 1.0);
            }

            Base::Vector3d uDir;
            Base::Vector3d vDir;
            plane.getAxes(uDir, vDir);
            std::vector<std::vector<Point2d>> rings;
            for (const auto& loop : cap.mesh.outlines) {
                std::vector<Point2d> ring;
                ring.reserve(loop.size());
                for (uint32_t idx : loop) {
                    const auto& pnt = cap.mesh.vertices[idx];
                    ring.push_back({pnt * uDir, pnt * vDir});
                }
                rings.push_back(std::move(ring));
            }

            segments[k] = hatchRings(rings, angle, instSpacing, phase);
            if (settings.hatch == DisplaySettings::Hatch::Cross) {
                auto cross = hatchRings(rings, angle + std::numbers::pi / 2.0, instSpacing, phase);
                segments[k].insert(segments[k].end(), cross.begin(), cross.end());
            }
        });

        for (std::size_t k = 0; k < caps.size(); ++k) {
            Cap& cap = caps[k];
            const auto& lines = segments[k];
            if (!cap.hatchGroup || lines.empty()) {
                continue;
            }
            const CutPlane& plane = planes[cap.plane];
            Base::Vector3d uDir;
            Base::Vector3d vDir;
            plane.getAxes(uDir, vDir);
            const Base::Vector3d origin = plane.normal * plane.distance;

            auto coords = new SoCoordinate3;
            coords->point.setNum(static_cast<int>(lines.size()));
            SbVec3f* pts = coords->point.startEditing();
            for (std::size_t i = 0; i < lines.size(); ++i) {
                pts[i] = toSb(origin + uDir * lines[i][0] + vDir * lines[i][1]);
            }
            coords->point.finishEditing();

            const Base::Color fill = settings.fillColor == DisplaySettings::FillColor::Uniform
                ? settings.uniformColor
                : instances[cap.instance].color;
            auto color = new SoBaseColor;
            color->rgb.setValue(contrastColor(fill));

            auto lineSet = new SoLineSet;
            const int numSegments = static_cast<int>(lines.size() / 2);
            lineSet->numVertices.setNum(numSegments);
            int32_t* counts = lineSet->numVertices.startEditing();
            std::fill(counts, counts + numSegments, 2);
            lineSet->numVertices.finishEditing();

            cap.hatchGroup->addChild(color);
            cap.hatchGroup->addChild(coords);
            cap.hatchGroup->addChild(lineSet);
        }
    }

    /// The object an instance belongs to, as top level object and sub-object path
    static App::SubObjectT resolveOwner(const SoFCSectionFilter::Stack& stack)
    {
        std::vector<App::DocumentObject*> objects;
        for (SoNode* node : stack) {
            auto vp = static_cast<SoFCSelectionRoot*>(node)->getViewProvider();
            auto vpd = freecad_cast<ViewProviderDocumentObject*>(vp);
            if (!vpd || !vpd->getObject()) {
                continue;
            }
            if (objects.empty() || objects.back() != vpd->getObject()) {
                objects.push_back(vpd->getObject());
            }
        }
        if (objects.empty()) {
            return {};
        }

        App::DocumentObject* top = objects.front();
        std::string subname;
        for (std::size_t i = 1; i < objects.size(); ++i) {
            // Links resolve their linked object implicitly, so skip path
            // components that don't resolve
            std::string candidate = subname + objects[i]->getNameInDocument() + ".";
            if (top->getSubObject(candidate.c_str()) == objects[i]) {
                subname = std::move(candidate);
            }
        }
        return {top, subname.c_str()};
    }

    App::DocumentObject* pickedObject(const SoPath* path, std::string& subname) const
    {
        auto full = static_cast<const SoFullPath*>(path);
        if (!full || full->getLength() == 0) {
            return nullptr;
        }
        auto it = pickMap.find(full->getTail());
        if (it == pickMap.end() || it->second >= instances.size()) {
            return nullptr;
        }
        // The owner is resolved when collecting, as the scene may have changed since
        const App::SubObjectT& owner = instances[it->second].owner;
        App::DocumentObject* top = owner.getObject();
        if (top) {
            subname = owner.getSubName();
        }
        return top;
    }
};

// ----------------------------------------------------------------------------

SectionScene::SectionScene(Gui::Document* doc)
    : d(std::make_unique<Private>(this, doc))
{
    allScenes().insert(this);

    auto invalidate = [this]() {
        if (d->attached) {
            d->geometryTimer.start();
        }
    };

    auto isRelevant = [](const App::DocumentObject& obj) {
        auto vp = Application::Instance->getViewProvider(&obj);
        return !vp || vp->isPartOfPhysicalObject();
    };

    // Any change of geometry, placement, visibility or appearance of any object,
    // in any document because of external links, may change the displayed scene.
    d->connectChangedObject = App::GetApplication().signalChangedObject.connect(
        [invalidate, isRelevant](const App::DocumentObject& obj, const App::Property& prop) {
            if (isRelevant(obj) && prop.getName() && std::strcmp(prop.getName(), "Label") != 0
                && std::strcmp(prop.getName(), "Label2") != 0) {
                invalidate();
            }
        }
    );
    d->connectNewObject = App::GetApplication().signalNewObject.connect(
        [invalidate](const App::DocumentObject&) { invalidate(); }
    );
    d->connectDeletedObject = App::GetApplication().signalDeletedObject.connect(
        [invalidate](const App::DocumentObject&) { invalidate(); }
    );
    d->connectChangedViewObject = Application::Instance->signalChangedObject.connect(
        [invalidate](const ViewProvider& vp, const App::Property&) {
            if (vp.isPartOfPhysicalObject()) {
                invalidate();
            }
        }
    );
    d->connectActivateView = Application::Instance->signalActivateView.connect(
        [this](const MDIView* view) {
            if (!d->attached || !view || view->getGuiDocument() != d->document) {
                return;
            }
            // Views opened after the section was activated
            bool added = false;
            for (auto viewer : d->documentViewers()) {
                if (std::find(d->viewers.begin(), d->viewers.end(), viewer) == d->viewers.end()) {
                    d->attachViewer(viewer);
                    added = true;
                }
            }
            if (auto view3d = qobject_cast<const View3DInventor*>(view)) {
                d->trackCamera(view3d->getViewer());
            }
            if (added && !d->geometryValid) {
                invalidateGeometry();
            }
        }
    );
}

SectionScene::~SectionScene()
{
    detach();
    allScenes().erase(this);
}

void SectionScene::setPlanes(const std::vector<CutPlane>& planes, Combination combination)
{
    const bool structureChanged = planes.size() != d->planes.size() || combination != d->combination;
    d->planes = planes;
    d->combination = combination;
    if (structureChanged) {
        d->keptPlanes.clear();
        d->removedPlanes.clear();
    }
    d->rebuildCaps();
    if (structureChanged) {
        d->updatePasses();
    }
}

const std::vector<CutPlane>& SectionScene::getPlanes() const
{
    return d->planes;
}

void SectionScene::setDisplaySettings(const DisplaySettings& settings)
{
    if (settings == d->settings) {
        return;
    }
    const DisplaySettings old = d->settings;
    d->settings = settings;

    // Hatch parameters only need the hatch lines to be regenerated
    DisplaySettings hatchOnly = old;
    hatchOnly.hatch = settings.hatch;
    hatchOnly.hatchSpacing = settings.hatchSpacing;
    hatchOnly.hatchAngle = settings.hatchAngle;
    hatchOnly.adaptiveHatch = settings.adaptiveHatch;
    hatchOnly.varyHatch = settings.varyHatch;
    if (hatchOnly == settings) {
        d->rebuildHatch();
    }
    else {
        d->rebuildCaps();
    }
}

const DisplaySettings& SectionScene::getDisplaySettings() const
{
    return d->settings;
}

void SectionScene::setFilter(const std::vector<App::SubObjectT>& items, bool sectionListed)
{
    d->filterItems = items;
    d->sectionListed = sectionListed;
    if (d->geometryValid) {
        d->applyFilter();
        d->rebuildCaps();
    }
}

void SectionScene::attach()
{
    d->attached = true;
    for (auto viewer : d->documentViewers()) {
        d->attachViewer(viewer);
    }
    if (auto view = d->document ? d->document->getActiveView() : nullptr) {
        if (auto view3d = qobject_cast<View3DInventor*>(view)) {
            d->trackCamera(view3d->getViewer());
        }
    }
    if (!d->geometryValid) {
        updateGeometry();
    }
    else {
        d->rebuildCaps();
    }
}

void SectionScene::detach()
{
    d->attached = false;
    d->geometryTimer.stop();
    auto viewers = d->viewers;
    for (const auto& viewer : viewers) {
        if (viewer) {
            d->detachViewer(viewer);
        }
    }
    d->viewers.clear();
    d->cameraSensor->detach();
    d->cameraViewer = nullptr;
}

bool SectionScene::isAttached() const
{
    return d->attached;
}

void SectionScene::invalidateGeometry()
{
    d->geometryValid = false;
    if (d->attached) {
        d->geometryTimer.start();
    }
}

void SectionScene::updateGeometry()
{
    d->updateGeometry();
}

SectionStats SectionScene::getStats() const
{
    SectionStats stats;
    stats.objects = d->instances.size();
    for (const auto& instance : d->instances) {
        if (instance.sectioned) {
            ++stats.sectioned;
        }
    }
    std::set<std::size_t> capped;
    std::set<std::size_t> open;
    for (const auto& cap : d->caps) {
        if (!cap.mesh.empty()) {
            capped.insert(cap.instance);
        }
        if (cap.open) {
            open.insert(cap.instance);
        }
    }
    stats.capped = capped.size();
    stats.open = open.size();
    return stats;
}

Base::BoundBox3d SectionScene::getBoundBox() const
{
    return d->boundBox;
}

App::DocumentObject* SectionScene::getPickedObject(const SoPath* path, std::string& subname) const
{
    return d->pickedObject(path, subname);
}

SectionScene* SectionScene::getAttached(const View3DInventorViewer* viewer)
{
    auto& scenes = attachedScenes();
    auto it = scenes.find(viewer);
    return it != scenes.end() ? it->second : nullptr;
}

App::DocumentObject* SectionScene::resolvePickedCap(const SoPath* path, std::string& subname)
{
    for (auto scene : allScenes()) {
        if (scene->d->attached) {
            if (auto obj = scene->getPickedObject(path, subname)) {
                return obj;
            }
        }
    }
    return nullptr;
}
