#!/usr/bin/env python3
"""Synthetic regression tests for analyze-reticle.py."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

import cv2  # type: ignore
import numpy as np


sys.dont_write_bytecode = True
ANALYZER_PATH = Path(__file__).with_name("analyze-reticle.py")
SPEC = importlib.util.spec_from_file_location("halo_reticle_analyzer", ANALYZER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Could not load {ANALYZER_PATH}")
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


def ring_image(color: tuple[int, int, int, int]) -> np.ndarray:
    image = np.zeros((250, 250, 4), dtype=np.uint8)
    cv2.circle(image, (125, 125), 13, color, 3, cv2.LINE_8)
    return image


class RawReticleAnalyzerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, name: str, image: np.ndarray) -> Path:
        path = self.root / name
        if not cv2.imwrite(str(path), image):
            raise RuntimeError(f"Could not write {path}")
        return path

    def test_accepts_authored_cyan_ring(self) -> None:
        path = self.write("cyan.png", ring_image((255, 220, 0, 255)))
        result, _ = ANALYZER.analyze(path, "cyan")
        self.assertTrue(result["passed"], result["failures"])

    def test_accepts_authored_red_ring(self) -> None:
        path = self.write("red.png", ring_image((0, 0, 255, 255)))
        result, _ = ANALYZER.analyze(path, "red")
        self.assertTrue(result["passed"], result["failures"])

    def test_rejects_grey_ring(self) -> None:
        path = self.write("grey.png", ring_image((100, 100, 100, 255)))
        result, _ = ANALYZER.analyze(path, "cyan")
        self.assertFalse(result["passed"])
        self.assertTrue(any("saturation" in item for item in result["failures"]))

    def test_rejects_filled_ball(self) -> None:
        image = np.zeros((250, 250, 4), dtype=np.uint8)
        cv2.circle(image, (125, 125), 14, (255, 220, 0, 255), -1, cv2.LINE_8)
        path = self.write("ball.png", image)
        result, _ = ANALYZER.analyze(path, "cyan")
        self.assertFalse(result["passed"])
        self.assertTrue(
            any("inner-disc occupancy" in item for item in result["failures"])
        )

    def test_rejects_second_visible_component(self) -> None:
        image = ring_image((255, 220, 0, 255))
        cv2.circle(image, (40, 40), 3, (255, 220, 0, 255), -1)
        path = self.write("duplicate.png", image)
        result, _ = ANALYZER.analyze(path, "cyan")
        self.assertFalse(result["passed"])
        self.assertTrue(any("components" in item for item in result["failures"]))

    def test_rejects_wrong_render_target_size(self) -> None:
        image = np.zeros((512, 512, 4), dtype=np.uint8)
        cv2.circle(image, (256, 256), 14, (255, 220, 0, 255), 4, cv2.LINE_AA)
        path = self.write("wrong-size.png", image)
        result, _ = ANALYZER.analyze(path, "cyan")
        self.assertFalse(result["passed"])
        self.assertTrue(any("resolution" in item for item in result["failures"]))


if __name__ == "__main__":
    unittest.main()
