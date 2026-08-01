#!/usr/bin/env python3
"""Validate Halo's authored reticle in composited stereo eye images.

The raw render-target oracle proves the widget itself is a centered cyan ring.
This oracle covers the downstream path: WidgetComponent projection, pass-through
material, scene exposure, and stereo composition.  HDR can make the ring dark,
so composited color is intentionally looser than the raw-image color check.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from dataclasses import dataclass
from pathlib import Path

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")

import cv2  # type: ignore
import numpy as np


DEFAULT_RIGHT_POINT = (316.5 / 839.0, 447.5 / 879.0)


@dataclass(frozen=True)
class Candidate:
    detection_method: str
    center: tuple[float, float]
    bbox: tuple[int, int, int, int]
    dimensions: tuple[int, int]
    area_px: int
    hole_count: int
    inner_disc_occupancy: float
    annular_occupancy: float
    circularity: float
    distance: float
    median_bgr: tuple[float, float, float]
    median_hsv: tuple[float, float, float]
    center_annulus_contrast: float
    score: float
    color_evaluable: bool = True
    color_pass: bool | None = True

    def as_dict(self) -> dict:
        return {
            "detection_method": self.detection_method,
            "centroid": list(self.center),
            "bbox": list(self.bbox),
            "dimensions": list(self.dimensions),
            "area_px": self.area_px,
            "hole_count": self.hole_count,
            "inner_disc_occupancy": self.inner_disc_occupancy,
            "annular_occupancy": self.annular_occupancy,
            "circularity": self.circularity,
            "expected_point_error_px": self.distance,
            "median_bgr": list(self.median_bgr),
            "median_hsv": list(self.median_hsv),
            "center_annulus_contrast": self.center_annulus_contrast,
            "score": self.score,
            "color_evaluable": self.color_evaluable,
            "color_pass": self.color_pass,
        }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--left", required=True, type=Path)
    parser.add_argument("--right", required=True, type=Path)
    parser.add_argument("--authored", type=Path)
    parser.add_argument("--json", dest="json_path", type=Path)
    parser.add_argument(
        "--expected-right",
        nargs=2,
        type=float,
        metavar=("NORMALIZED_X", "NORMALIZED_Y"),
        default=DEFAULT_RIGHT_POINT,
        help="Expected neutral controller-ray point in the right eye, normalized 0..1.",
    )
    parser.add_argument(
        "--search-radius",
        type=float,
        default=0.04,
        help="Allowed normalized distance from the expected controller-ray point.",
    )
    parser.add_argument("--maximum-projection-error", type=float, default=6.0)
    parser.add_argument("--maximum-stereo-error", type=float, default=6.0)
    parser.add_argument("--minimum-phase-response", type=float, default=0.05)
    parser.add_argument(
        "--allow-geometry-only",
        action="store_true",
        help=(
            "Permit a calibrated dark annulus when its blue/cyan color cannot "
            "be verified. This is diagnostic-only; release validation must "
            "leave this disabled."
        ),
    )
    return parser.parse_args()


def load_bgr8(path: Path) -> np.ndarray:
    image = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if image is None:
        raise RuntimeError(f"OpenCV could not read {path}")
    if image.ndim != 3 or image.shape[2] not in (3, 4):
        raise RuntimeError(f"Expected a three- or four-channel image, got {image.shape}")
    bgr = image[:, :, :3]
    if np.issubdtype(bgr.dtype, np.floating):
        return np.clip(bgr * 255.0, 0.0, 255.0).astype(np.uint8)
    if bgr.dtype == np.uint16:
        return (bgr / 257).astype(np.uint8)
    return bgr.astype(np.uint8)


def analyze_authored_reference(path: Path) -> dict:
    image = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if image is None:
        raise RuntimeError(f"OpenCV could not read authored reticle {path}")
    bgr = load_bgr8(path)
    if image.shape[2] == 4:
        alpha = image[:, :, 3]
        if np.issubdtype(alpha.dtype, np.floating):
            mask = alpha > 0.01
        elif alpha.dtype == np.uint16:
            mask = alpha > 655
        else:
            mask = alpha > 2
    else:
        mask = cv2.cvtColor(bgr, cv2.COLOR_BGR2HSV)[:, :, 2] > 10
    ys, xs = np.where(mask)
    if len(xs) == 0:
        raise RuntimeError("Authored reticle contains no visible pixels")
    bbox = [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())]
    width = bbox[2] - bbox[0] + 1
    height = bbox[3] - bbox[1] + 1
    center = np.array([float(xs.mean()), float(ys.mean())])
    yy, xx = np.ogrid[: mask.shape[0], : mask.shape[1]]
    radius = np.sqrt((xx - center[0]) ** 2 + (yy - center[1]) ** 2)
    inner_occupancy = float(mask[radius <= min(width, height) * 0.28].mean())
    hsv = cv2.cvtColor(bgr, cv2.COLOR_BGR2HSV)
    median_hsv = [float(v) for v in np.median(hsv[mask], axis=0)]
    if not 0.8 <= width / height <= 1.25:
        raise RuntimeError(f"Authored reticle bbox is not ring-like: {width}x{height}")
    if inner_occupancy > 0.05:
        raise RuntimeError(
            f"Authored reticle is not hollow: inner occupancy {inner_occupancy:.4f}"
        )
    return {
        "image": str(path.resolve()),
        "bbox": bbox,
        "dimensions": [width, height],
        "centroid": center.tolist(),
        "inner_disc_occupancy": inner_occupancy,
        "median_hsv": median_hsv,
    }


def component_holes(mask: np.ndarray) -> tuple[int, float]:
    contours, hierarchy = cv2.findContours(
        mask.astype(np.uint8) * 255,
        cv2.RETR_CCOMP,
        cv2.CHAIN_APPROX_SIMPLE,
    )
    if hierarchy is None:
        return 0, 0.0
    holes = sum(1 for entry in hierarchy[0] if int(entry[3]) >= 0)
    outer_circularity = 0.0
    for index, contour in enumerate(contours):
        if int(hierarchy[0][index][3]) >= 0:
            continue
        perimeter = float(cv2.arcLength(contour, True))
        if perimeter > 0.0:
            outer_circularity = max(
                outer_circularity,
                4.0
                * math.pi
                * float(cv2.contourArea(contour))
                / (perimeter * perimeter),
            )
    return holes, outer_circularity


def cyan_component_candidates(
    image: np.ndarray,
    expected: tuple[float, float],
    search_radius_px: float,
) -> list[Candidate]:
    """Find the authored stroke from strict composited cyan pixels.

    Sampling the entire geometric annulus is unreliable because most of that
    band can be the scene behind an eleven-pixel reticle.  This path instead
    measures only pixels that survive the authored-reticle HSV mask.
    """

    height = image.shape[0]
    scale = height / 880.0
    min_dimension = max(5, int(round(7.0 * scale)))
    max_dimension = max(min_dimension + 2, int(round(20.0 * scale)))
    min_area = max(12, int(round(25.0 * scale * scale)))
    max_area = max(min_area + 10, int(round(120.0 * scale * scale)))
    maximum_distance = min(search_radius_px, max(4.0, 20.0 * scale))

    hsv = cv2.cvtColor(image, cv2.COLOR_BGR2HSV)
    cyan_mask = cv2.inRange(
        hsv,
        np.array([90, 90, 20], dtype=np.uint8),
        np.array([120, 255, 255], dtype=np.uint8),
    )
    count, labels, stats, centroids = cv2.connectedComponentsWithStats(
        cyan_mask, 8
    )
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    grid_y, grid_x = np.ogrid[:height, : image.shape[1]]
    candidates: list[Candidate] = []
    for index in range(1, count):
        x, y, box_width, box_height, area = map(int, stats[index])
        center = tuple(float(value) for value in centroids[index])
        distance = float(math.dist(center, expected))
        if distance > maximum_distance:
            continue
        if not (
            min_area <= area <= max_area
            and min_dimension <= box_width <= max_dimension
            and min_dimension <= box_height <= max_dimension
            and 0.60 <= box_width / box_height <= 1.60
        ):
            continue

        component = labels == index
        radius_px = (box_width + box_height) / 4.0
        radius = np.sqrt(
            (grid_x - center[0]) ** 2 + (grid_y - center[1]) ** 2
        )
        inner = radius <= max(1.5, radius_px * 0.40)
        annulus = (radius >= radius_px * 0.55) & (
            radius <= radius_px * 1.15
        )
        inner_occupancy = float(component[inner].mean())
        annular_occupancy = float(component[annulus].mean())
        holes, circularity = component_holes(component)
        if not (
            inner_occupancy <= 0.20
            and annular_occupancy >= 0.35
            and holes >= 1
            and circularity >= 0.60
        ):
            continue

        median_bgr = tuple(float(v) for v in np.median(image[component], axis=0))
        median_hsv = tuple(float(v) for v in np.median(hsv[component], axis=0))
        contrast = abs(float(gray[inner].mean()) - float(gray[component].mean()))
        distance_score = 1.0 - min(1.0, distance / maximum_distance)
        score = (
            2.0 * distance_score
            + circularity
            + min(1.0, contrast / 20.0)
            + min(1.0, annular_occupancy)
        )
        candidates.append(
            Candidate(
                detection_method="strict_cyan_connected_component",
                center=center,
                bbox=(x, y, x + box_width - 1, y + box_height - 1),
                dimensions=(box_width, box_height),
                area_px=area,
                hole_count=holes,
                inner_disc_occupancy=inner_occupancy,
                annular_occupancy=annular_occupancy,
                circularity=circularity,
                distance=distance,
                median_bgr=median_bgr,
                median_hsv=median_hsv,
                center_annulus_contrast=contrast,
                score=score,
            )
        )
    candidates.sort(key=lambda item: item.score, reverse=True)
    return candidates


def contour_candidates(
    image: np.ndarray,
    expected: tuple[float, float],
    search_radius_px: float,
) -> list[Candidate]:
    strict_candidates = cyan_component_candidates(
        image, expected, search_radius_px
    )
    if strict_candidates:
        return strict_candidates

    height, width = image.shape[:2]
    scale = height / 880.0
    min_dimension = max(5, int(round(6.0 * scale)))
    max_dimension = max(min_dimension + 2, int(round(20.0 * scale)))
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    blurred = cv2.GaussianBlur(gray, (3, 3), 0.5)
    edges = cv2.Canny(blurred, 20, 60)
    contours, hierarchy = cv2.findContours(
        edges, cv2.RETR_TREE, cv2.CHAIN_APPROX_SIMPLE
    )
    if hierarchy is None:
        return []
    hsv = cv2.cvtColor(image, cv2.COLOR_BGR2HSV)
    grid_y, grid_x = np.ogrid[:height, :width]
    candidates: list[Candidate] = []
    for index, contour in enumerate(contours):
        if int(hierarchy[0][index][2]) < 0:
            continue
        x, y, box_width, box_height = cv2.boundingRect(contour)
        if not (
            min_dimension <= box_width <= max_dimension
            and min_dimension <= box_height <= max_dimension
        ):
            continue
        aspect = box_width / box_height
        if not 0.70 <= aspect <= 1.43:
            continue
        area = float(abs(cv2.contourArea(contour)))
        perimeter = float(cv2.arcLength(contour, True))
        if perimeter <= 0.0:
            continue
        circularity = 4.0 * math.pi * area / (perimeter * perimeter)
        if circularity < 0.55:
            continue
        center = (x + (box_width - 1) / 2.0, y + (box_height - 1) / 2.0)
        distance = float(math.dist(center, expected))
        if distance > search_radius_px:
            continue

        diameter = float(max(box_width, box_height))
        radius = np.sqrt((grid_x - center[0]) ** 2 + (grid_y - center[1]) ** 2)
        inner = radius <= diameter * 0.28
        annulus = (radius >= diameter * 0.45) & (radius <= diameter * 0.90)
        if not np.any(inner) or not np.any(annulus):
            continue
        # HDR/tonemap can shift the composited ring outside the strict cyan
        # range.  For that fallback, keep the Canny-proven hollow contour but
        # sample only blue/cyan stroke pixels rather than the scene-filled
        # geometric annulus.
        color_pixels = (
            annulus
            & (hsv[:, :, 0] >= 85)
            & (hsv[:, :, 0] <= 145)
            & (hsv[:, :, 1] >= 35)
            & (hsv[:, :, 2] >= 15)
            & (image[:, :, 0].astype(np.int16)
               >= image[:, :, 2].astype(np.int16) + 6)
        )
        minimum_color_pixels = max(8, int(round(float(annulus.sum()) * 0.20)))
        color_pixel_count = int(color_pixels.sum())
        if color_pixel_count < minimum_color_pixels:
            continue
        median_bgr = tuple(float(v) for v in np.median(image[color_pixels], axis=0))
        median_hsv = tuple(float(v) for v in np.median(hsv[color_pixels], axis=0))
        hue, saturation, value = median_hsv
        # The authored ring is cyan, but HDR composition can drive it into a
        # dark blue-violet.  Preserve hue-family and saturation requirements
        # while allowing low luminance.
        if not (85.0 <= hue <= 145.0 and saturation >= 35.0 and value >= 15.0):
            continue
        blue, _green, red = median_bgr
        if blue < red + 6.0:
            continue
        inner_luma = float(gray[inner].mean())
        annulus_luma = float(gray[color_pixels].mean())
        contrast = abs(inner_luma - annulus_luma)
        if contrast < 8.0:
            continue
        distance_score = 1.0 - min(1.0, distance / search_radius_px)
        score = 2.0 * distance_score + circularity + min(1.0, contrast / 40.0)
        candidates.append(
            Candidate(
                detection_method="hollow_contour_hdr_color_mask",
                center=center,
                bbox=(x, y, x + box_width - 1, y + box_height - 1),
                dimensions=(box_width, box_height),
                area_px=color_pixel_count,
                hole_count=1,
                inner_disc_occupancy=0.0,
                annular_occupancy=color_pixel_count / float(annulus.sum()),
                circularity=circularity,
                distance=distance,
                median_bgr=median_bgr,
                median_hsv=median_hsv,
                center_annulus_contrast=contrast,
                score=score,
            )
        )
    candidates.sort(key=lambda item: item.score, reverse=True)
    # Canny can produce coincident inner/outer edge contours for one stroke.
    # Treat those as one rendered ring instead of reporting a duplicate.
    unique: list[Candidate] = []
    for candidate in candidates:
        if any(math.dist(candidate.center, item.center) <= 1.0 for item in unique):
            continue
        unique.append(candidate)
    if unique:
        return unique
    dark = dark_annulus_candidate(image, expected, search_radius_px)
    return [dark] if dark is not None else []


def dark_annulus_candidate(
    image: np.ndarray,
    expected: tuple[float, float],
    search_radius_px: float,
) -> Candidate | None:
    """Fit the exposure-darkened authored annulus at the calibrated ray only.

    This is geometry evidence, not a color pass. The three-pixel center window
    and narrow radius range deliberately prevent a generic dark scene circle
    elsewhere in the search region from being promoted to the reticle.
    """

    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY).astype(np.float32)
    expected_x, expected_y = expected
    center_window = min(3.0, search_radius_px)
    x0 = max(0, int(expected_x) - 13)
    x1 = min(image.shape[1], int(expected_x) + 14)
    y0 = max(0, int(expected_y) - 13)
    y1 = min(image.shape[0], int(expected_y) + 14)
    if x1 - x0 < 20 or y1 - y0 < 20:
        return None
    yy, xx = np.mgrid[y0:y1, x0:x1]
    local = gray[y0:y1, x0:x1]
    best: tuple[float, float, float, float, float, float] | None = None
    for cy in np.arange(expected_y - center_window, expected_y + center_window + 0.01, 0.5):
        for cx in np.arange(expected_x - center_window, expected_x + center_window + 0.01, 0.5):
            distance = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
            angle = (np.arctan2(yy - cy, xx - cx) + math.pi) % (2.0 * math.pi)
            for radius in np.arange(4.0, 6.51, 0.5):
                inner = distance <= radius - 2.0
                ring = np.abs(distance - radius) <= 1.0
                outer = (distance >= radius + 1.8) & (distance <= radius + 3.3)
                if not np.any(inner) or not np.any(ring) or not np.any(outer):
                    continue
                segment_means = []
                for segment in range(8):
                    sector = (
                        ring
                        & (angle >= segment * math.pi / 4.0)
                        & (angle < (segment + 1) * math.pi / 4.0)
                    )
                    if not np.any(sector):
                        segment_means = []
                        break
                    segment_means.append(float(local[sector].mean()))
                if not segment_means:
                    continue
                ring_mean = float(np.mean(segment_means))
                contrast = (
                    0.55 * (float(local[inner].mean()) - ring_mean)
                    + 0.45 * (float(local[outer].mean()) - ring_mean)
                )
                angular_stddev = float(np.std(segment_means))
                score = contrast - 0.25 * angular_stddev
                candidate = (score, contrast, cx, cy, radius, angular_stddev)
                if best is None or candidate > best:
                    best = candidate
    if best is None:
        return None
    score, contrast, cx, cy, radius, _angular_stddev = best
    if score < 8.0 or contrast < 10.0:
        return None
    distance = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    ring = np.abs(distance - radius) <= 1.0
    ring_pixels = image[y0:y1, x0:x1][ring]
    median_bgr = tuple(float(value) for value in np.median(ring_pixels, axis=0))
    ring_hsv = cv2.cvtColor(
        ring_pixels.reshape((-1, 1, 3)), cv2.COLOR_BGR2HSV
    ).reshape((-1, 3))
    median_hsv = tuple(float(value) for value in np.median(ring_hsv, axis=0))
    left = int(math.floor(cx - radius - 1.0))
    top = int(math.floor(cy - radius - 1.0))
    right = int(math.ceil(cx + radius + 1.0))
    bottom = int(math.ceil(cy + radius + 1.0))
    return Candidate(
        detection_method="calibrated_dark_annulus_geometry_only",
        center=(float(cx), float(cy)),
        bbox=(left, top, right, bottom),
        dimensions=(right - left + 1, bottom - top + 1),
        area_px=int(ring.sum()),
        hole_count=1,
        inner_disc_occupancy=0.0,
        annular_occupancy=1.0,
        circularity=1.0,
        distance=float(math.dist((cx, cy), expected)),
        median_bgr=median_bgr,
        median_hsv=median_hsv,
        center_annulus_contrast=float(contrast),
        score=float(score),
        color_evaluable=False,
        color_pass=None,
    )


def estimate_eye_shift(left: np.ndarray, right: np.ndarray) -> tuple[tuple[float, float], float]:
    if left.shape != right.shape:
        raise RuntimeError(
            f"Stereo eye resolutions differ: left {left.shape}, right {right.shape}"
        )
    height, width = left.shape[:2]
    y0 = int(round(height * 0.06))
    y1 = int(round(height * 0.45))
    left_gray = cv2.cvtColor(left[y0:y1], cv2.COLOR_BGR2GRAY).astype(np.float32)
    right_gray = cv2.cvtColor(right[y0:y1], cv2.COLOR_BGR2GRAY).astype(np.float32)
    window = cv2.createHanningWindow((width, y1 - y0), cv2.CV_32F)
    shift, response = cv2.phaseCorrelate(left_gray, right_gray, window)
    return (float(shift[0]), float(shift[1])), float(response)


def analyze_pair(
    left_path: Path,
    right_path: Path,
    expected_right_normalized: tuple[float, float] = DEFAULT_RIGHT_POINT,
    search_radius_normalized: float = 0.04,
    maximum_projection_error: float = 6.0,
    maximum_stereo_error: float = 6.0,
    minimum_phase_response: float = 0.05,
    authored_path: Path | None = None,
    require_color: bool = True,
) -> dict:
    left = load_bgr8(left_path)
    right = load_bgr8(right_path)
    failures: list[str] = []
    height, width = right.shape[:2]
    expected_right = (
        expected_right_normalized[0] * (width - 1),
        expected_right_normalized[1] * (height - 1),
    )
    search_radius_px = search_radius_normalized * min(width, height)

    authored = None
    if authored_path is not None:
        try:
            authored = analyze_authored_reference(authored_path)
        except Exception as error:  # surfaced as an oracle failure in JSON
            failures.append(str(error))

    right_candidates = contour_candidates(right, expected_right, search_radius_px)
    right_ring = right_candidates[0] if right_candidates else None
    if right_ring is None:
        failures.append(
            "right eye has no hollow blue/cyan ring near the expected controller-ray point"
        )
    elif right_ring.distance > maximum_projection_error:
        failures.append(
            f"right-eye controller-ray projection error was {right_ring.distance:.3f}px, "
            f"expected <= {maximum_projection_error:.3f}px"
        )
    if right_ring is not None and require_color and not right_ring.color_evaluable:
        failures.append(
            "right-eye ring geometry was present, but its authored blue/cyan "
            "color was not verifiable"
        )

    shift = (0.0, 0.0)
    phase_response = 0.0
    expected_left = expected_right
    try:
        shift, phase_response = estimate_eye_shift(left, right)
        if phase_response < minimum_phase_response:
            failures.append(
                f"stereo background phase response was {phase_response:.4f}, "
                f"expected >= {minimum_phase_response:.4f}"
            )
        # Keep the left-eye oracle independent of the detected right ring. A
        # common displacement in both eyes must fail projection, not disappear
        # by using one measured ring as the other eye's expected anchor.
        expected_left = (
            expected_right[0] - shift[0],
            expected_right[1] - shift[1],
        )
    except Exception as error:
        failures.append(str(error))

    left_candidates = contour_candidates(left, expected_left, search_radius_px)
    left_ring = left_candidates[0] if left_candidates else None
    if left_ring is None:
        failures.append(
            "left eye has no hollow blue/cyan ring at the stereo-corresponding controller-ray point"
        )
    elif left_ring.distance > maximum_projection_error:
        failures.append(
            f"left-eye controller-ray projection error was {left_ring.distance:.3f}px, "
            f"expected <= {maximum_projection_error:.3f}px"
        )
    if left_ring is not None and require_color and not left_ring.color_evaluable:
        failures.append(
            "left-eye ring geometry was present, but its authored blue/cyan "
            "color was not verifiable"
        )

    stereo_error = None
    if left_ring is not None and right_ring is not None:
        expected_left_from_right_ring = (
            right_ring.center[0] - shift[0],
            right_ring.center[1] - shift[1],
        )
        stereo_error = float(
            math.dist(left_ring.center, expected_left_from_right_ring)
        )
        if stereo_error > maximum_stereo_error:
            failures.append(
                f"stereo reticle correspondence error was {stereo_error:.3f}px, "
                f"expected <= {maximum_stereo_error:.3f}px"
            )
        size_ratio = max(left_ring.dimensions) / max(right_ring.dimensions)
        if not 0.70 <= size_ratio <= 1.43:
            failures.append(
                f"stereo reticle size ratio was {size_ratio:.3f}, expected 0.70..1.43"
            )

    return {
        "schema_version": 1,
        "passed": not failures,
        "failures": failures,
        "left_image": str(left_path.resolve()),
        "right_image": str(right_path.resolve()),
        "resolution": [width, height],
        "expected_right_point": list(expected_right),
        "search_radius_px": search_radius_px,
        "maximum_projection_error_px": maximum_projection_error,
        "require_reticle_color": require_color,
        "authored_reference": authored,
        "eye_background_shift_left_to_right": list(shift),
        "eye_background_phase_response": phase_response,
        "expected_left_point": list(expected_left),
        "stereo_correspondence_error_px": stereo_error,
        "right_candidate_count": len(right_candidates),
        "left_candidate_count": len(left_candidates),
        "right_ring": right_ring.as_dict() if right_ring is not None else None,
        "left_ring": left_ring.as_dict() if left_ring is not None else None,
    }


def main() -> int:
    args = parse_args()
    result = analyze_pair(
        args.left,
        args.right,
        tuple(args.expected_right),
        args.search_radius,
        args.maximum_projection_error,
        args.maximum_stereo_error,
        args.minimum_phase_response,
        args.authored,
        require_color=not args.allow_geometry_only,
    )
    output = json.dumps(result, indent=2)
    print(output)
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(output + "\n", encoding="utf-8")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
