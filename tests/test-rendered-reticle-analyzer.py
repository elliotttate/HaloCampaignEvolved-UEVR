#!/usr/bin/env python3
"""Synthetic regression tests for analyze-rendered-reticle.py."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

import cv2  # type: ignore
import numpy as np


sys.dont_write_bytecode = True
ANALYZER_PATH = Path(__file__).with_name("analyze-rendered-reticle.py")
SPEC = importlib.util.spec_from_file_location("halo_rendered_reticle_analyzer", ANALYZER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Could not load {ANALYZER_PATH}")
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


WIDTH = 840
HEIGHT = 880
RIGHT_CENTER = (316, 448)
EYE_SHIFT = 206
LEFT_CENTER = (RIGHT_CENTER[0] + EYE_SHIFT, RIGHT_CENTER[1])


def textured_background() -> np.ndarray:
    rng = np.random.default_rng(0xCE)
    noise = rng.normal(0.0, 12.0, (HEIGHT, WIDTH, 1)).astype(np.float32)
    y, x = np.mgrid[:HEIGHT, :WIDTH]
    base = np.stack(
        (
            95.0 + 0.04 * x + 0.02 * y,
            70.0 + 0.02 * x + 0.015 * y,
            82.0 + 0.025 * x + 0.01 * y,
        ),
        axis=2,
    )
    image = np.clip(base + noise, 0.0, 255.0).astype(np.uint8)
    return cv2.GaussianBlur(image, (5, 5), 0.8)


def shifted_left_eye(right_background: np.ndarray) -> np.ndarray:
    matrix = np.float32([[1.0, 0.0, EYE_SHIFT], [0.0, 1.0, 0.0]])
    return cv2.warpAffine(
        right_background,
        matrix,
        (WIDTH, HEIGHT),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REFLECT,
    )


def draw_ring(image: np.ndarray, center: tuple[int, int], color=(160, 112, 100)) -> None:
    cv2.circle(image, center, 8, color, thickness=3, lineType=cv2.LINE_AA)


def draw_shield(
    image: np.ndarray, center: tuple[int, int], color=(160, 112, 100)
) -> None:
    x, y = center
    points = np.array(
        [[x - 7, y - 7], [x + 7, y - 7], [x + 6, y + 3], [x, y + 9], [x - 6, y + 3]],
        dtype=np.int32,
    )
    cv2.fillConvexPoly(image, points, color, lineType=cv2.LINE_AA)


def authored_ring() -> np.ndarray:
    image = np.zeros((250, 250, 4), dtype=np.uint8)
    cv2.circle(image, (125, 125), 14, (220, 180, 20, 255), 4, cv2.LINE_AA)
    return image


class RenderedReticleAnalyzerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.authored = self.root / "authored.png"
        cv2.imwrite(str(self.authored), authored_ring())

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_pair(
        self,
        *,
        right_ring: bool = True,
        left_ring: bool = True,
        color=(160, 112, 100),
        right_offset: int = 0,
        left_offset: int = 0,
        solid: bool = False,
        shield: bool = False,
    ) -> tuple[Path, Path]:
        right = textured_background()
        left = shifted_left_eye(right)
        if right_ring:
            right_center = (RIGHT_CENTER[0] + right_offset, RIGHT_CENTER[1])
            if shield:
                draw_shield(right, right_center, color)
            elif solid:
                cv2.circle(right, right_center, 8, color, -1, cv2.LINE_AA)
            else:
                draw_ring(right, right_center, color)
        if left_ring:
            center = (
                LEFT_CENTER[0] + right_offset + left_offset,
                LEFT_CENTER[1],
            )
            if shield:
                draw_shield(left, center, color)
            elif solid:
                cv2.circle(left, center, 8, color, -1, cv2.LINE_AA)
            else:
                draw_ring(left, center, color)
        left_path = self.root / "left.png"
        right_path = self.root / "right.png"
        cv2.imwrite(str(left_path), left)
        cv2.imwrite(str(right_path), right)
        return left_path, right_path

    def analyze(self, left: Path, right: Path) -> dict:
        expected = (RIGHT_CENTER[0] / (WIDTH - 1), RIGHT_CENTER[1] / (HEIGHT - 1))
        return ANALYZER.analyze_pair(
            left,
            right,
            expected_right_normalized=expected,
            authored_path=self.authored,
        )

    def test_accepts_dim_blue_stereo_ring(self) -> None:
        left, right = self.write_pair()
        result = self.analyze(left, right)
        self.assertTrue(result["passed"], result["failures"])
        self.assertLess(result["stereo_correspondence_error_px"], 2.0)
        self.assertLess(result["right_ring"]["expected_point_error_px"], 2.0)

    def test_accepts_dim_cyan_stereo_ring(self) -> None:
        left, right = self.write_pair(color=(150, 150, 80))
        result = self.analyze(left, right)
        self.assertTrue(result["passed"], result["failures"])
        self.assertLess(result["stereo_correspondence_error_px"], 2.0)

    def test_accepts_dark_annulus_as_geometry_only(self) -> None:
        left, right = self.write_pair(color=(20, 20, 20))
        result = self.analyze(left, right)
        self.assertTrue(result["passed"], result["failures"])
        self.assertEqual(
            result["right_ring"]["detection_method"],
            "calibrated_dark_annulus_geometry_only",
        )
        self.assertFalse(result["right_ring"]["color_evaluable"])
        self.assertIsNone(result["right_ring"]["color_pass"])

    def test_rejects_missing_right_ring(self) -> None:
        left, right = self.write_pair(right_ring=False)
        result = self.analyze(left, right)
        self.assertFalse(result["passed"])
        self.assertTrue(any("right eye" in item for item in result["failures"]))

    def test_rejects_non_blue_ring(self) -> None:
        left, right = self.write_pair(color=(30, 40, 220))
        result = self.analyze(left, right)
        self.assertFalse(result["passed"])

    def test_rejects_filled_disc(self) -> None:
        left, right = self.write_pair(solid=True)
        result = self.analyze(left, right)
        self.assertFalse(result["passed"])

    def test_rejects_filled_shield(self) -> None:
        left, right = self.write_pair(shield=True)
        result = self.analyze(left, right)
        self.assertFalse(result["passed"])

    def test_rejects_stereo_mismatch(self) -> None:
        left, right = self.write_pair(left_offset=14)
        result = self.analyze(left, right)
        self.assertFalse(result["passed"])
        self.assertTrue(any("stereo" in item for item in result["failures"]))

    def test_rejects_common_two_eye_projection_offset(self) -> None:
        left, right = self.write_pair(right_offset=10)
        result = self.analyze(left, right)
        self.assertFalse(result["passed"])
        self.assertTrue(any("controller-ray projection" in item for item in result["failures"]))


if __name__ == "__main__":
    unittest.main()
