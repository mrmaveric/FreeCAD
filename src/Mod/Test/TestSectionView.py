# SPDX-License-Identifier: LGPL-2.1-or-later

"""Tests of the App::SectionView document object.

Run with FreeCADCmd -t TestSectionView. The 3D view part is tested by
TestSectionViewGui.
"""

import math
import os
import tempfile
import unittest

import FreeCAD as App


def normal(placement):
    return placement.Rotation.multVec(App.Vector(0, 0, 1))


class TestSectionView(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestSectionView")
        self.section = self.doc.addObject("App::SectionView", "SectionView")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def assertVector(self, actual, expected, places=7):
        for a, e in zip(actual, expected):
            self.assertAlmostEqual(a, e, places)

    def test_defaults(self):
        section = self.section
        self.assertEqual(section.SectionType, "Planes")
        self.assertEqual(section.Combination, "Remove intersection")
        self.assertEqual(section.Plane1Base, "XZ")
        self.assertFalse(section.Plane2Enabled)
        self.assertFalse(section.Plane3Enabled)
        self.assertEqual(section.ObjectMode, "Exclude")
        self.assertEqual(len(section.Planes), 1)
        # The XZ plane cuts away the front, like the front view looks at it
        self.assertVector(normal(section.Planes[0]), (0, -1, 0))
        self.assertVector(section.Planes[0].Base, (0, 0, 0))

    def test_principal_planes(self):
        expected = {"XY": (0, 0, 1), "XZ": (0, -1, 0), "YZ": (1, 0, 0)}
        for base, direction in expected.items():
            self.section.Plane1Base = base
            self.assertVector(normal(self.section.Planes[0]), direction)

    def test_offset_moves_along_normal(self):
        self.section.Plane1Base = "XY"
        self.section.Plane1Offset = 7.5
        self.assertVector(self.section.Planes[0].Base, (0, 0, 7.5))
        self.section.Plane1Base = "XZ"
        self.assertVector(self.section.Planes[0].Base, (0, -7.5, 0))

    def test_tilt(self):
        self.section.Plane1Base = "XY"
        self.section.Plane1AngleX = 90
        self.assertVector(normal(self.section.Planes[0]), (0, -1, 0))
        self.section.Plane1AngleX = 0
        self.section.Plane1AngleY = 90
        self.assertVector(normal(self.section.Planes[0]), (1, 0, 0))
        # The offset follows the tilted normal
        self.section.Plane1Offset = 2
        self.assertVector(self.section.Planes[0].Base, (2, 0, 0))

    def test_flip(self):
        self.section.Plane1Base = "XY"
        self.section.Plane1Offset = 3
        self.section.Plane1Flipped = True
        self.assertVector(normal(self.section.Planes[0]), (0, 0, -1))
        # Flipping doesn't move the plane
        self.assertVector(self.section.Planes[0].Base, (0, 0, 3))

    def test_multiple_planes(self):
        self.section.Plane2Enabled = True
        self.assertEqual(len(self.section.Planes), 2)
        self.section.Plane3Enabled = True
        self.assertEqual(len(self.section.Planes), 3)
        self.assertVector(normal(self.section.Planes[1]), (1, 0, 0))
        self.assertVector(normal(self.section.Planes[2]), (0, 0, 1))
        self.section.Plane2Enabled = False
        self.assertEqual(len(self.section.Planes), 2)

    def test_box(self):
        section = self.section
        section.SectionType = "Box"
        section.BoxPlacement = App.Placement(App.Vector(1, 2, 3), App.Rotation())
        section.BoxLength = 10
        section.BoxWidth = 20
        section.BoxHeight = 30
        planes = section.Planes
        self.assertEqual(len(planes), 6)
        # Every plane cuts away the outside of the box
        center = App.Vector(6, 12, 18)
        for plane in planes:
            self.assertLess((center - plane.Base).dot(normal(plane)), 0)
        bases = sorted((round(p.Base.dot(normal(p)), 6) for p in planes))
        self.assertEqual(bases, sorted([-1, 11, -2, 22, -3, 33]))

    def test_reference_to_datum_plane(self):
        part = self.doc.addObject("App::Part", "Part")
        part.Placement = App.Placement(App.Vector(0, 0, 4), App.Rotation(App.Vector(1, 0, 0), 90))
        self.doc.recompute()
        plane = [f for f in part.Origin.OriginFeatures if f.Role == "XY_Plane"][0]
        self.section.Plane1Base = "Reference"
        # A datum inside a part, referenced with its path so the part's placement applies
        self.section.Plane1Reference = (part, "Origin." + plane.Name + ".")
        self.doc.recompute()
        self.assertTrue(self.section.isValid())
        self.assertVector(normal(self.section.Planes[0]), (0, -1, 0))
        self.assertVector(self.section.Planes[0].Base, (0, 0, 4))

    def test_reference_to_face(self):
        try:
            import Part  # noqa: F401
        except ImportError:
            self.skipTest("Part module not available")
        box = self.doc.addObject("Part::Box", "Box")
        box.Length = 10
        box.Width = 20
        box.Height = 30
        box.Placement.Base = App.Vector(5, 0, 0)
        self.doc.recompute()

        top = None
        for i, face in enumerate(box.Shape.Faces):
            if face.Surface.Axis.z > 0.5 and abs(face.CenterOfMass.z - 30) < 1e-6:
                top = "Face%d" % (i + 1)
        self.assertIsNotNone(top)

        self.section.Plane1Base = "Reference"
        self.section.Plane1Reference = (box, top)
        self.doc.recompute()
        self.assertTrue(self.section.isValid())
        plane = self.section.Planes[0]
        # The outward normal of the face points to the removed side
        self.assertVector(normal(plane), (0, 0, 1))
        self.assertVector(plane.Base, (10, 10, 30))

        # The plane follows the face
        box.Height = 40
        self.doc.recompute()
        self.assertAlmostEqual(self.section.Planes[0].Base.z, 40)

        # Offsets are relative to the face
        self.section.Plane1Offset = -15
        self.assertAlmostEqual(self.section.Planes[0].Base.z, 25)

    def test_invalid_reference(self):
        try:
            import Part  # noqa: F401
        except ImportError:
            self.skipTest("Part module not available")
        cylinder = self.doc.addObject("Part::Cylinder", "Cylinder")
        self.doc.recompute()
        lateral = None
        for i, face in enumerate(cylinder.Shape.Faces):
            if face.Surface.TypeId == "Part::GeomCylinder":
                lateral = "Face%d" % (i + 1)
        self.section.Plane1Base = "Reference"
        self.section.Plane1Reference = (cylinder, lateral)
        self.doc.recompute()
        self.assertFalse(self.section.isValid())
        # Falls back to the XY plane
        self.assertVector(normal(self.section.Planes[0]), (0, 0, 1))

    def test_save_and_restore(self):
        section = self.section
        target = self.doc.addObject("App::FeaturePython", "Target")
        section.Plane1Base = "YZ"
        section.Plane1Offset = 12.5
        section.Plane1AngleY = 15
        section.Plane2Enabled = True
        section.Combination = "Keep intersection"
        section.ObjectMode = "Section only"
        section.Objects = [(target, "")]
        expected = [App.Placement(p) for p in section.Planes]

        path = os.path.join(tempfile.gettempdir(), "TestSectionView.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        try:
            restored = self.doc.getObject("SectionView")
            self.assertEqual(restored.Plane1Base, "YZ")
            self.assertAlmostEqual(restored.Plane1Offset.Value, 12.5)
            self.assertTrue(restored.Plane2Enabled)
            self.assertEqual(restored.Combination, "Keep intersection")
            self.assertEqual(restored.ObjectMode, "Section only")
            self.assertEqual(restored.Objects[0][0].Name, "Target")
            self.assertEqual(len(restored.Planes), 2)
            for a, b in zip(restored.Planes, expected):
                self.assertTrue(a.isSame(b, 1e-7))
        finally:
            os.remove(path)


if __name__ == "__main__":
    unittest.main()
