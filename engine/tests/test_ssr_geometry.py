# SPDX-License-Identifier: GPL-2.0-or-later
import math
import re
import unittest
from pathlib import Path

SHADERS = Path(__file__).resolve().parents[1] / "gpu/shadps4/video_core/host_shaders"

def smoothstep(lo, hi, value):
    t = max(0, min(1, (value - lo) / (hi - lo)))
    return t * t * (3 - 2 * t)

class SsrGeometryTest(unittest.TestCase):
    def test_shader_paths_match(self):
        compute = (SHADERS / "ssr_trace.comp").read_text()
        fragment = (SHADERS / "ssr.frag").read_text()

        def intersections(source):
            return source.split("float hitT = -1.0;", 1)[1].split(
                "vec4 hc =", 1)[0].strip()
        self.assertEqual(intersections(compute), intersections(fragment))

        def normals(source):
            body = source.split("vec3 stableNormal(vec2 uv) {", 1)[1].split(
                "vec4 trace(", 1)[0]
            body = re.sub(r"const vec2 k\[8\].*?;", "", body, flags=re.S)
            body = re.sub(r"vec2 t = .*?;", "vec2 t;", body)
            body = body.replace("const vec2 t;", "vec2 t;")
            body = body.replace("* 1.5", "* 3.0")
            body = body.replace("vec2(2.0 * t.x, 0.0)", "vec2(t.x, 0.0)")
            body = body.replace("vec2(0.0, 2.0 * t.y)", "vec2(0.0, t.y)")
            return re.sub(r"\s+", "", body)
        self.assertEqual(normals(compute), normals(fragment))
        body = intersections(compute)
        self.assertIn("prevDiff < 0.0 && diff >= 0.0", body)
        self.assertIn("prevDiff = diff;", body)
        self.assertIn("abs(surface.z - ray.z) <= tolerance && off_ray <= tolerance", body)
        self.assertIn("distance_along_ray > max(0.02, 0.001 * abs(P.z))", body)

    def test_filter_rejects_foreign_surfaces(self):
        def weight(relative_depth, normal_dot):
            return ((1 - smoothstep(0.01, 0.03, relative_depth)) *
                    smoothstep(0.8, 0.95, normal_dot))
        self.assertEqual(weight(0, 1), 1)
        self.assertEqual(weight(0.1, 1), 0)
        self.assertEqual(weight(0, 0.5), 0)
        self.assertEqual(weight(0, -1), 0)
        self.assertAlmostEqual(weight(0.02, 1), 0.5)

        center = (0, 1, 0)
        neighbors = [((1, 0, 0), 0), ((0, -1, 0), 0.2)]
        total = list(center)
        for normal, depth in neighbors:
            w = weight(depth, normal[1])
            total = [a + b * w for a, b in zip(total, normal)]
        self.assertEqual(tuple(total), center)

    def test_crossing_and_hit_validation(self):
        def bracket(previous, current, depth):
            return previous < 0 <= current < 0.0002 and depth < 1
        self.assertFalse(bracket(0, 0.00001, 0.9))
        self.assertFalse(bracket(0.00001, 0.00002, 0.9))
        self.assertFalse(bracket(-0.0001, 0.0001, 1))
        self.assertTrue(bracket(-0.0001, 0.0001, 0.9))
        def valid(surface, ray, direction, origin):
            delta = [a - b for a, b in zip(surface, origin)]
            along = sum(a * b for a, b in zip(delta, direction))
            off = math.sqrt(sum((a - along * b) ** 2 for a, b in zip(delta, direction)))
            tolerance = max(0.02, min(0.2, 0.002 * abs(surface[2])))
            return (along > max(0.02, 0.001 * abs(origin[2])) and
                    abs(surface[2] - ray[2]) <= tolerance and off <= tolerance)
        self.assertTrue(valid((0, 0, 10), (0, 0, 10.001), (0, 0, 1), (0, 0, 2)))
        self.assertFalse(valid((0, 0, 10), (0, 0, 11), (0, 0, 1), (0, 0, 2)))
        self.assertFalse(valid((1, 0, 10), (1, 0, 10), (0, 0, 1), (0, 0, 2)))
        self.assertFalse(valid((0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 2)))
        self.assertFalse(valid((0, 0, 2.001), (0, 0, 2.001), (0, 0, 1), (0, 0, 2)))

    def test_planar_receiver_keeps_subtle_ripples(self):
        def confidence(error, continuity, alignment):
            return ((1 - smoothstep(0.02, 0.08, error)) *
                    (1 - smoothstep(0.01, 0.03, continuity)) *
                    smoothstep(0.8, 0.95, alignment))
        self.assertEqual(confidence(0, 0, 1), 1)
        self.assertEqual(confidence(0.1, 0, 1), 0)
        self.assertEqual(confidence(0, 0.1, 1), 0)
        self.assertEqual(confidence(0, 0, 0.5), 0)
        for angle in (0, 0.5, 5, 15):
            tangent = math.sin(math.radians(angle))
            limited = min(tangent, 0.02)
            actual = math.degrees(math.atan(limited))
            self.assertLessEqual(actual, 1.146)
            if angle > 0:
                self.assertGreater(actual, 0)
        source = (SHADERS / "ssr_trace.comp").read_text()
        self.assertIn("0.02 / max(length(ripple), 0.0001)", source)
        self.assertIn("smoothstep(0.02, 0.08, plane_error)", source)

if __name__ == "__main__":
    unittest.main()
