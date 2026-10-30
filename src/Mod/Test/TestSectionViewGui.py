# SPDX-License-Identifier: LGPL-2.1-or-later

"""Tests of the section view in the 3D view.

Run with FreeCAD -t TestSectionViewGui.
"""

import math
import unittest

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

try:
    from pivy import coin
except ImportError:
    coin = None


def process_events():
    # Activation and geometry updates are deferred to the event loop
    for _ in range(3):
        QtCore.QCoreApplication.processEvents()


@unittest.skipIf(coin is None, "pivy not available")
class TestSectionViewGui(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestSectionViewGui")
        self.box = self.doc.addObject("Part::Box", "Box")
        self.box.Length = 20
        self.box.Width = 20
        self.box.Height = 20
        self.doc.recompute()
        self.view = Gui.getDocument(self.doc.Name).ActiveView
        process_events()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def add_box(self, name, x):
        box = self.doc.addObject("Part::Box", name)
        box.Length = 20
        box.Width = 20
        box.Height = 20
        box.Placement.Base = App.Vector(x, 0, 0)
        self.doc.recompute()
        process_events()
        return box

    def add_section(self, name="SectionView", offset=-10):
        section = self.doc.addObject("App::SectionView", name)
        section.Plane1Base = "XZ"
        section.Plane1Offset = offset
        process_events()
        return section

    @staticmethod
    def section_root(view):
        scene = view.getViewer().getSoRenderManager().getSceneGraph()
        search = coin.SoSearchAction()
        search.setName(coin.SbName("SectionRoot"))
        search.setInterest(coin.SoSearchAction.FIRST)
        search.setSearchingAll(True)
        search.apply(scene)
        path = search.getPath()
        return path.getTail() if path else None

    @staticmethod
    def count_type(node, type_id, children_only=False):
        if children_only:
            return sum(
                1 for i in range(node.getNumChildren()) if node.getChild(i).isOfType(type_id)
            )
        search = coin.SoSearchAction()
        search.setType(type_id)
        search.setInterest(coin.SoSearchAction.ALL)
        search.setSearchingAll(True)
        search.apply(node)
        return search.getPaths().getLength()

    def passes(self, view=None):
        root = self.section_root(view or self.view)
        return [
            root.getChild(i)
            for i in range(root.getNumChildren())
            if root.getChild(i).getName().getString() != "SectionCaps"
        ]

    def caps(self, view=None):
        root = self.section_root(view or self.view)
        for i in range(root.getNumChildren()):
            child = root.getChild(i)
            if child.getName().getString() == "SectionCaps":
                return child
        return None

    def all_caps(self, view=None):
        root = self.section_root(view or self.view)
        return [
            root.getChild(i)
            for i in range(root.getNumChildren())
            if root.getChild(i).getName().getString() == "SectionCaps"
        ]

    def hits_top(self, x, y):
        """Whether a ray straight down at (x, y) hits the top face of a box"""
        manager = self.view.getViewer().getSoRenderManager()
        pick = coin.SoRayPickAction(manager.getViewportRegion())
        pick.setRay(coin.SbVec3f(x, y, 100), coin.SbVec3f(0, 0, -1))
        pick.apply(manager.getSceneGraph())
        point = pick.getPickedPoint()
        return point is not None and abs(point.getPoint()[2] - 20) < 1e-3

    def clip_planes(self, pass_node):
        return self.count_type(pass_node, coin.SoClipPlane.getClassTypeId(), True)

    def test_scene_without_section(self):
        passes = self.passes()
        self.assertEqual(len(passes), 1)
        self.assertEqual(self.clip_planes(passes[0]), 0)
        self.assertIsNone(self.caps())

    def test_activate_and_deactivate(self):
        section = self.add_section()
        passes = self.passes()
        self.assertEqual(len(passes), 1)
        self.assertEqual(self.clip_planes(passes[0]), 1)
        caps = self.caps()
        self.assertIsNotNone(caps)
        self.assertGreater(self.count_type(caps, coin.SoIndexedFaceSet.getClassTypeId()), 0)

        section.Visibility = False
        process_events()
        self.assertEqual(self.clip_planes(self.passes()[0]), 0)
        self.assertIsNone(self.caps())

        section.Visibility = True
        process_events()
        self.assertEqual(self.clip_planes(self.passes()[0]), 1)

    def test_no_cap_when_plane_misses(self):
        self.add_section(offset=50)
        caps = self.caps()
        self.assertEqual(self.count_type(caps, coin.SoIndexedFaceSet.getClassTypeId()), 0)

    def test_sections_cutting_the_same_objects_replace_each_other(self):
        first = self.add_section("First")
        second = self.add_section("Second")
        self.assertFalse(first.Visibility)
        self.assertTrue(second.Visibility)
        first.Visibility = True
        process_events()
        self.assertFalse(second.Visibility)

    def sections_of_different_objects(self):
        """Shows a section of the box and a section of another box together"""
        other = self.add_box("Other", 40)
        first = self.add_section("First")
        first.ObjectMode = "Section only"
        first.Objects = [(self.box, "")]
        # A new section cuts everything, so it hides the first one until it is limited
        second = self.add_section("Second", offset=-5)
        self.assertFalse(first.Visibility)
        second.ObjectMode = "Section only"
        second.Objects = [(other, "")]
        first.Visibility = True
        process_events()
        return first, second, other

    def test_sections_of_different_objects_are_shown_together(self):
        self.assertTrue(self.hits_top(10, 7))
        first, second, _ = self.sections_of_different_objects()
        self.assertTrue(first.Visibility)
        self.assertTrue(second.Visibility)
        # A pass for each section and one for the objects neither cuts
        self.assertEqual([self.clip_planes(p) for p in self.passes()], [1, 1, 0])
        caps = self.all_caps()
        self.assertEqual(len(caps), 2)
        for cap in caps:
            self.assertGreater(self.count_type(cap, coin.SoIndexedFaceSet.getClassTypeId()), 0)
        # Each box is only cut by its own section, at y = 10 and y = 5
        self.assertFalse(self.hits_top(10, 7))
        self.assertTrue(self.hits_top(10, 13))
        self.assertTrue(self.hits_top(50, 7))
        self.assertFalse(self.hits_top(50, 3))

        first.Visibility = False
        process_events()
        self.assertTrue(self.hits_top(10, 7))
        self.assertFalse(self.hits_top(50, 3))
        self.assertEqual(len(self.all_caps()), 1)

    def test_section_changed_to_cut_the_same_objects_hides_the_other(self):
        first, second, other = self.sections_of_different_objects()
        second.Objects = [(other, ""), (self.box, "")]
        process_events()
        self.assertTrue(second.Visibility)
        self.assertFalse(first.Visibility)

    def test_combination_passes(self):
        section = self.add_section()
        section.Plane2Enabled = True
        section.Plane2Offset = 10
        process_events()
        # Removing the corner renders the kept region in two passes
        passes = self.passes()
        self.assertEqual(len(passes), 2)
        self.assertEqual(self.clip_planes(passes[0]), 1)
        self.assertEqual(self.clip_planes(passes[1]), 2)

        section.Combination = "Keep intersection"
        process_events()
        passes = self.passes()
        self.assertEqual(len(passes), 1)
        self.assertEqual(self.clip_planes(passes[0]), 2)

    def test_box(self):
        section = self.add_section()
        section.SectionType = "Box"
        section.BoxPlacement = App.Placement(App.Vector(5, 5, 5), App.Rotation())
        process_events()
        passes = self.passes()
        self.assertEqual(len(passes), 1)
        self.assertEqual(self.clip_planes(passes[0]), 6)

    def test_excluded_objects_get_a_pass(self):
        section = self.add_section()
        section.Objects = [(self.box, "")]
        process_events()
        passes = self.passes()
        self.assertEqual(len(passes), 2)
        self.assertEqual(self.clip_planes(passes[1]), 0)
        # Excluded objects aren't capped
        caps = self.caps()
        self.assertEqual(self.count_type(caps, coin.SoIndexedFaceSet.getClassTypeId()), 0)

    def test_caps_follow_geometry(self):
        self.add_section()

        def cap_extent():
            action = coin.SoGetBoundingBoxAction(
                self.view.getViewer().getSoRenderManager().getViewportRegion()
            )
            action.apply(self.caps())
            return action.getBoundingBox().getSize().getValue()

        self.assertAlmostEqual(cap_extent()[0], 20, 3)
        self.box.Length = 35
        self.doc.recompute()
        process_events()
        self.assertAlmostEqual(cap_extent()[0], 35, 3)

    def test_new_view_shows_section(self):
        self.add_section()
        Gui.runCommand("Std_ViewCreate")
        process_events()
        view = Gui.getDocument(self.doc.Name).ActiveView
        self.assertNotEqual(view, self.view)
        self.assertEqual(self.clip_planes(self.passes(view)[0]), 1)

    def test_toggle_command(self):
        section = self.add_section()
        Gui.runCommand("Std_SectionView")
        process_events()
        self.assertFalse(section.Visibility)
        Gui.runCommand("Std_SectionView")
        process_events()
        self.assertTrue(section.Visibility)

    def test_toggle_command_restores_all_sections(self):
        first, second, _ = self.sections_of_different_objects()
        Gui.runCommand("Std_SectionView")
        process_events()
        self.assertFalse(first.Visibility)
        self.assertFalse(second.Visibility)
        Gui.runCommand("Std_SectionView")
        process_events()
        self.assertTrue(first.Visibility)
        self.assertTrue(second.Visibility)

    def test_delete_and_undo(self):
        self.doc.UndoMode = 1
        self.doc.openTransaction("Create section")
        section = self.add_section()
        self.doc.commitTransaction()
        self.doc.openTransaction("Delete section")
        self.doc.removeObject(section.Name)
        self.doc.commitTransaction()
        process_events()
        self.assertEqual(self.clip_planes(self.passes()[0]), 0)
        self.doc.undo()
        process_events()
        self.assertEqual(self.clip_planes(self.passes()[0]), 1)

    def test_principal_base_stops_picking_the_reference(self):
        section = self.add_section()
        Gui.getDocument(self.doc.Name).setEdit(section.Name)
        try:
            process_events()
            # Only the page of the first plane is shown, panels of earlier tests may
            # still exist hidden
            window = Gui.getMainWindow()
            base = [
                c
                for c in window.findChildren(QtWidgets.QComboBox)
                if c.isVisible() and c.count() == 4 and c.itemText(3) == "Reference"
            ][0]
            select = [
                b
                for b in window.findChildren(QtWidgets.QPushButton)
                if b.isVisible() and b.isCheckable() and b.text().startswith("Select")
            ][0]

            # Choosing Reference without one starts picking it
            base.setCurrentIndex(3)
            process_events()
            self.assertEqual(section.Plane1Base, "Reference")
            self.assertTrue(select.isChecked())

            base.setCurrentIndex(0)
            process_events()
            self.assertEqual(section.Plane1Base, "XY")
            self.assertFalse(select.isChecked())
        finally:
            Gui.getDocument(self.doc.Name).resetEdit()
            process_events()

    def test_offset_arrow_follows_tilt_drag(self):
        """After tilting the plane with its gizmo, the offset arrow points along the new normal"""
        section = self.add_section()
        Gui.getDocument(self.doc.Name).setEdit(section.Name)
        try:
            process_events()
            scene = self.view.getViewer().getSoRenderManager().getSceneGraph()
            arrows = self.find_paths(scene, "SoLinearDraggerContainer")
            arcs = self.find_paths(scene, "SoRotationDraggerContainer")
            if not arrows or not arcs:
                self.skipTest("gizmos are disabled")
            arrow = arrows[0].getTail()
            self.assertLess(self.arrow_error(section, arrow), 0.5)

            # The window and the gizmos settle in the event loop, which can take a
            # while when FreeCAD has just started
            for _ in range(10):
                self.view.viewIsometric()
                self.view.fitAll()
                for _ in range(3):
                    Gui.runCommand("Std_ViewZoomOut")
                process_events()
                start, direction = self.screen_position(scene, arcs[0].getTail())
                if start is not None:
                    break
                QtCore.QThread.msleep(200)
            else:
                self.skipTest("the tilt gizmo is not on the screen")
            angle = section.Plane1AngleX.Value
            self.drag(start, direction, 72)
            if section.Plane1AngleX.Value == angle:
                self.skipTest("the simulated drag did not reach the tilt gizmo")
            self.assertLess(self.arrow_error(section, arrow), 0.5)
        finally:
            Gui.getDocument(self.doc.Name).resetEdit()
            process_events()

    @staticmethod
    def find_paths(node, type_name):
        search = coin.SoSearchAction()
        search.setType(coin.SoType.fromName(coin.SbName(type_name)))
        search.setInterest(coin.SoSearchAction.ALL)
        search.setSearchingAll(True)
        search.apply(node)
        paths = search.getPaths()
        return [paths[i].copy() for i in range(paths.getLength())]

    @staticmethod
    def arrow_error(section, arrow):
        """Angle in degrees between the offset arrow and the plane normal, either way"""
        tip = arrow.rotation.getValue().multVec(coin.SbVec3f(0, 1, 0))
        direction = App.Vector(*tip.getValue()).normalize()
        normal = section.Planes[0].Rotation.multVec(App.Vector(0, 0, 1))
        return math.degrees(math.acos(min(1.0, abs(direction.dot(normal)))))

    def screen_position(self, scene, node):
        """A widget position on the node and the direction along it, found with pick rays"""
        region = self.view.getViewer().getSoRenderManager().getViewportRegion()
        width, height = region.getViewportSizePixels().getValue()
        hits = []
        for y in range(0, height, 8):
            for x in range(0, width, 8):
                pick = coin.SoRayPickAction(region)
                pick.setPoint(coin.SbVec2s(x, y))
                pick.apply(scene)
                point = pick.getPickedPoint()
                if point and point.getPath().containsNode(node):
                    hits.append((x, height - 1 - y))
        if len(hits) < 2:
            return None, None
        (x0, y0), (x1, y1) = hits[0], hits[-1]
        length = math.hypot(x1 - x0, y1 - y0)
        x, y = hits[len(hits) // 2]
        # The viewport is in device pixels, mouse events are in the widget's pixels
        ratio = self.view.graphicsView().viewport().devicePixelRatioF()
        position = QtCore.QPoint(round(x / ratio), round(y / ratio))
        return position, ((x1 - x0) / length, (y1 - y0) / length)

    def drag(self, start, direction, distance):
        widget = self.view.graphicsView().viewport()

        def send(kind, pos, buttons):
            event = QtGui.QMouseEvent(
                kind,
                QtCore.QPointF(pos),
                QtCore.QPointF(widget.mapToGlobal(pos)),
                QtCore.Qt.LeftButton,
                buttons,
                QtCore.Qt.NoModifier,
            )
            QtWidgets.QApplication.sendEvent(widget, event)
            process_events()

        send(QtCore.QEvent.MouseMove, start, QtCore.Qt.NoButton)
        send(QtCore.QEvent.MouseButtonPress, start, QtCore.Qt.LeftButton)
        pos = start
        for step in range(1, 13):
            offset = distance * step / 12
            pos = start + QtCore.QPoint(round(direction[0] * offset), round(direction[1] * offset))
            send(QtCore.QEvent.MouseMove, pos, QtCore.Qt.LeftButton)
        send(QtCore.QEvent.MouseButtonRelease, pos, QtCore.Qt.NoButton)
        for _ in range(5):
            process_events()


if __name__ == "__main__":
    unittest.main()
