#!/usr/bin/env python3
"""Validate Halo's raw 250x250 world-reticle render target.

The oracle intentionally analyzes the WidgetComponent render target rather
than a scene screenshot. That makes shape and color checks independent of
scene exposure, stereo projection, and background clutter.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")

import cv2  # type: ignore
import numpy as np


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument(
        "--color",
        choices=("cyan", "red", "any"),
        default="cyan",
    )
    parser.add_argument("--json", dest="json_path", type=Path)
    parser.add_argument("--reference-mask", type=Path)
    parser.add_argument("--minimum-mask-iou", type=float, default=0.90)
    return parser.parse_args()


def load_image(path: Path) -> np.ndarray:
    image = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if image is None:
        raise RuntimeError(f"OpenCV could not read {path}")
    if image.ndim != 3 or image.shape[2] not in (3, 4):
        raise RuntimeError(f"Expected a three- or four-channel image, got {image.shape}")
    return image


def to_bgr8(image: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    bgr = image[:, :, :3]
    if np.issubdtype(bgr.dtype, np.floating):
        bgr8 = np.clip(bgr * 255.0, 0.0, 255.0).astype(np.uint8)
    elif bgr.dtype == np.uint16:
        bgr8 = (bgr / 257).astype(np.uint8)
    else:
        bgr8 = bgr.astype(np.uint8)

    value_mask = cv2.cvtColor(bgr8, cv2.COLOR_BGR2HSV)[:, :, 2] > 10
    if image.shape[2] == 4:
        alpha = image[:, :, 3]
        if np.issubdtype(alpha.dtype, np.floating):
            alpha_mask = alpha > 0.01
        elif alpha.dtype == np.uint16:
            alpha_mask = alpha > 655
        else:
            alpha_mask = alpha > 2
        mask = alpha_mask & value_mask
    else:
        mask = value_mask
    return bgr8, mask


def analyze(path: Path, expected_color: str) -> tuple[dict, np.ndarray]:
    image = load_image(path)
    bgr, mask = to_bgr8(image)
    height, width = mask.shape
    failures: list[str] = []

    if (width, height) != (250, 250):
        failures.append(f"resolution was {width}x{height}, expected 250x250")

    count, labels, stats, _ = cv2.connectedComponentsWithStats(
        mask.astype(np.uint8), connectivity=8
    )
    components = [
        int(stats[index, cv2.CC_STAT_AREA])
        for index in range(1, count)
        if int(stats[index, cv2.CC_STAT_AREA]) >= 2
    ]
    components.sort(reverse=True)
    if len(components) != 1:
        failures.append(f"found {len(components)} foreground components, expected 1")

    ys, xs = np.where(mask)
    if len(xs) == 0:
        failures.append("render target contains no visible reticle pixels")
        bbox = None
        centroid = None
        area = 0
        inner_occupancy = 1.0
        median_hsv = None
    else:
        x0, x1 = int(xs.min()), int(xs.max())
        y0, y1 = int(ys.min()), int(ys.max())
        bbox = [x0, y0, x1, y1]
        bbox_width = x1 - x0 + 1
        bbox_height = y1 - y0 + 1
        centroid = [float(xs.mean()), float(ys.mean())]
        area = int(len(xs))

        if not 250 <= area <= 450:
            failures.append(f"foreground area was {area}, expected 250..450")
        if not 28 <= bbox_width <= 32 or not 28 <= bbox_height <= 32:
            failures.append(
                f"bbox was {bbox_width}x{bbox_height}, expected each dimension 28..32"
            )
        expected_center = np.array([(width - 1) / 2.0, (height - 1) / 2.0])
        center_error = float(
            np.linalg.norm(np.array(centroid, dtype=float) - expected_center)
        )
        if center_error > 1.0:
            failures.append(f"centroid error was {center_error:.3f}px, expected <=1px")

        grid_y, grid_x = np.ogrid[:height, :width]
        radius = np.sqrt(
            (grid_x - expected_center[0]) ** 2
            + (grid_y - expected_center[1]) ** 2
        )
        inner = radius <= 9.0
        inner_occupancy = float(mask[inner].mean())
        if inner_occupancy > 0.01:
            failures.append(
                f"inner-disc occupancy was {inner_occupancy:.4f}, expected <=0.01"
            )

        hsv = cv2.cvtColor(bgr, cv2.COLOR_BGR2HSV)
        median_hsv = [float(value) for value in np.median(hsv[mask], axis=0)]
        hue, saturation, _ = median_hsv
        if saturation < 180:
            failures.append(f"median saturation was {saturation:.1f}, expected >=180")
        if expected_color == "cyan" and not 90 <= hue <= 105:
            failures.append(f"median hue was {hue:.1f}, expected cyan 90..105")
        if expected_color == "red" and not (hue <= 10 or hue >= 170):
            failures.append(f"median hue was {hue:.1f}, expected red <=10 or >=170")

    result = {
        "schema_version": 1,
        "image": str(path.resolve()),
        "expected_color": expected_color,
        "passed": not failures,
        "failures": failures,
        "resolution": [width, height],
        "component_areas": components,
        "foreground_area": area,
        "bbox": bbox,
        "centroid": centroid,
        "inner_disc_occupancy": inner_occupancy,
        "median_hsv": median_hsv,
    }
    return result, mask


def main() -> int:
    args = parse_args()
    result, mask = analyze(args.image, args.color)

    if args.reference_mask:
        _, reference = analyze(args.reference_mask, "any")
        intersection = int(np.logical_and(mask, reference).sum())
        union = int(np.logical_or(mask, reference).sum())
        iou = float(intersection / union) if union else 0.0
        result["reference_mask_iou"] = iou
        if iou < args.minimum_mask_iou:
            result["failures"].append(
                f"reference mask IoU was {iou:.4f}, expected >= {args.minimum_mask_iou}"
            )
            result["passed"] = False

    output = json.dumps(result, indent=2)
    print(output)
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(output + "\n", encoding="utf-8")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
