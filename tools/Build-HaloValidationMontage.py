#!/usr/bin/env python3
"""Build a compact visual proof sheet from a Halo visual-fire matrix run."""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--measurements", required=True, type=Path)
    return parser.parse_args()


def load_reticle_analyzer(repo_root: Path):
    path = repo_root / "tests" / "analyze-rendered-reticle.py"
    spec = importlib.util.spec_from_file_location("halo_reticle_analyzer", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def get_font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    candidates = [
        Path("C:/Windows/Fonts/seguisb.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf"),
        Path("C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf"),
    ]
    for path in candidates:
        if path.exists():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default()


def expected_point(name: str, degrees: float) -> tuple[float, float]:
    center_x, center_y = 316.5, 447.5
    offset = 101.5 * math.tan(math.radians(degrees)) / math.tan(math.radians(15.0))
    points = {
        "neutral": (center_x, center_y),
        "aim_yaw_pos": (center_x - offset, center_y),
        "aim_yaw_neg": (center_x + offset, center_y),
        "aim_pitch_pos": (center_x, center_y - offset),
        "aim_pitch_neg": (center_x, center_y + offset),
        "aim_roll_pos": (center_x, center_y),
        "grip_translate_x_pos": (center_x, center_y),
    }
    if name not in points:
        raise RuntimeError(f"No calibrated reticle point for {name}")
    return points[name]


def main() -> int:
    args = parse_args()
    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    repo_root = Path(__file__).resolve().parents[1]
    analyzer = load_reticle_analyzer(repo_root)
    degrees = float(summary["parameters"]["rotation_degrees"])

    title_font = get_font(24, True)
    body_font = get_font(17)
    small_font = get_font(14)
    tile_width, tile_height = 660, 550
    columns = 2
    rows = math.ceil(len(summary["cases"]) / columns)
    header_height = 100
    sheet = Image.new(
        "RGB", (tile_width * columns, header_height + tile_height * rows), (12, 16, 23)
    )
    draw = ImageDraw.Draw(sheet)
    draw.text((24, 18), "Halo CE UEVR - 6DOF reticle and ballistic proof", font=title_font, fill=(235, 245, 255))
    draw.text(
        (24, 56),
        f"{len(summary['cases'])} fired poses | exact +1 marker / +1 projectile required | {degrees:g} degree rotations",
        font=body_font,
        fill=(125, 224, 241),
    )

    measurements: list[dict] = []
    for index, case in enumerate(summary["cases"]):
        name = str(case["Name"])
        image_path = Path(case["Screenshot"])
        source = Image.open(image_path).convert("RGB")
        expected = expected_point(name, degrees)
        bgr = analyzer.load_bgr8(image_path)
        candidates = analyzer.contour_candidates(bgr, expected, 40.0)
        candidate = candidates[0] if candidates else None
        measured = candidate.center if candidate is not None else expected

        annotated = source.copy()
        adraw = ImageDraw.Draw(annotated)
        ex, ey = expected
        adraw.rectangle((ex - 13, ey - 13, ex + 13, ey + 13), outline=(255, 208, 48), width=2)
        adraw.line((ex - 18, ey, ex - 7, ey), fill=(255, 208, 48), width=2)
        adraw.line((ex + 7, ey, ex + 18, ey), fill=(255, 208, 48), width=2)
        adraw.line((ex, ey - 18, ex, ey - 7), fill=(255, 208, 48), width=2)
        adraw.line((ex, ey + 7, ex, ey + 18), fill=(255, 208, 48), width=2)

        full = annotated.resize((462, 484), Image.Resampling.LANCZOS)
        crop_radius = 24
        crop = source.crop((ex - crop_radius, ey - crop_radius, ex + crop_radius, ey + crop_radius))
        crop = crop.resize((168, 168), Image.Resampling.NEAREST)

        column, row = index % columns, index // columns
        ox, oy = column * tile_width, header_height + row * tile_height
        draw.rectangle((ox + 8, oy + 8, ox + tile_width - 8, oy + tile_height - 8), fill=(20, 27, 37), outline=(54, 74, 91), width=2)
        sheet.paste(full, (ox + 18, oy + 50))
        sheet.paste(crop, (ox + 484, oy + 50))
        draw.rectangle((ox + 483, oy + 49, ox + 653, oy + 219), outline=(255, 208, 48), width=2)

        shot = case["ShotMetrics"]
        numeric = case["NumericMetrics"]
        detection = candidate.detection_method if candidate is not None else "projected center (HUD overlap)"
        title = name.replace("_", " ")
        draw.text((ox + 18, oy + 17), title, font=title_font, fill=(235, 245, 255))
        lines = [
            f"reticle ({measured[0]:.1f}, {measured[1]:.1f})",
            detection,
            f"weapon/controller dot {float(numeric['WeaponForwardExpectedDot']):.6f}",
            f"muzzle-ray miss {1000.0 * float(shot['MuzzleTargetRayMissMeters']):.4f} mm",
            f"projectile angle {float(shot['ProjectileTargetAngleDegrees']):.3f} deg",
            f"shot counters +{int(shot['ShotSampleMarkerCountDelta'])}/+{int(shot['ShotSampleProjectileCountDelta'])}",
        ]
        for line_index, line in enumerate(lines):
            draw.text((ox + 486, oy + 238 + line_index * 29), line, font=small_font, fill=(170, 215, 226))
        draw.text((ox + 486, oy + 425), "Yellow box = calibrated controller ray", font=small_font, fill=(255, 208, 48))

        measurements.append(
            {
                "case": name,
                "screenshot": str(image_path.resolve()),
                "expected_reticle_center": list(expected),
                "measured_reticle_center": list(measured),
                "detection_method": detection,
                "weapon_controller_dot": numeric["WeaponForwardExpectedDot"],
                "muzzle_ray_miss_meters": shot["MuzzleTargetRayMissMeters"],
                "projectile_target_angle_degrees": shot["ProjectileTargetAngleDegrees"],
                "marker_delta": shot["ShotSampleMarkerCountDelta"],
                "projectile_delta": shot["ShotSampleProjectileCountDelta"],
            }
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.output, optimize=True)
    args.measurements.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "source_summary": str(args.summary.resolve()),
                "all_cases_passed": all(bool(case["Passed"]) for case in summary["cases"]),
                "machine_detected_reticle_count": sum(
                    item["detection_method"] != "projected center (HUD overlap)"
                    for item in measurements
                ),
                "case_count": len(measurements),
                "cases": measurements,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
