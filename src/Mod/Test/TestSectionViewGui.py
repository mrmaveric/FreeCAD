# SPDX-License-Identifier: LGPL-2.1-or-later

"""Tests of the section view in the 3D view.

Run with FreeCAD -t TestSectionViewGui.
"""

import unittest

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore

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

    def test_only_one_section_is_shown(self):
        first = self.add_section("First")
        second = self.add_section("Second")
        self.assertFalse(first.Visibility)
        self.assertTrue(second.Visibility)
        first.Visibility = True
        process_events()
        self.assertFalse(second.Visibility)

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


if __name__ == "__main__":
    unittest.main()
