#!/usr/bin/env python3
"""Build an annotated montage while retaining untouched stereo source images."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    path = Path(
        "C:/Windows/Fonts/seguisb.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf"
    )
    return ImageFont.truetype(str(path), size) if path.exists() else ImageFont.load_default()


def annotate(image: Image.Image, expected: list[float], ring: dict | None) -> Image.Image:
    result = image.copy()
    draw = ImageDraw.Draw(result)
    x, y = expected
    draw.line((x - 14, y, x + 14, y), fill=(255, 212, 48), width=2)
    draw.line((x, y - 14, x, y + 14), fill=(255, 212, 48), width=2)
    if ring:
        x0, y0, x1, y1 = ring["bbox"]
        draw.rectangle((x0 - 2, y0 - 2, x1 + 2, y1 + 2), outline=(71, 255, 169), width=2)
    return result


def crop(image: Image.Image, center: list[float], radius: int = 27) -> Image.Image:
    x, y = center
    box = (int(x - radius), int(y - radius), int(x + radius), int(y + radius))
    return image.crop(box).resize((150, 150), Image.Resampling.NEAREST)


def main() -> int:
    args = parse_args()
    summary = json.loads(args.summary.read_text(encoding="utf-8-sig"))
    cases = summary["cases"]
    width, height = 1900, 1390
    sheet = Image.new("RGB", (width, height), (12, 17, 24))
    draw = ImageDraw.Draw(sheet)
    draw.text((28, 18), "Halo CE UEVR - Authored Reticle Stereo Sweep", font=font(30, True), fill=(240, 248, 255))
    draw.text((29, 58), "Yellow = calibrated controller ray | Green = detected hollow ring | source PNGs retained", font=font(17), fill=(166, 210, 224))
    card_w, card_h = 620, 630
    for index, case in enumerate(cases):
        column, row = index % 3, index // 3
        ox, oy = 15 + column * 628, 100 + row * 640
        passed = bool(case["passed"])
        outline = (55, 214, 143) if passed else (244, 91, 91)
        draw.rounded_rectangle((ox, oy, ox + card_w, oy + card_h), 12, fill=(23, 31, 41), outline=outline, width=3)
        draw.text((ox + 18, oy + 13), case["name"].replace("_", " "), font=font(21, True), fill=(242, 247, 255))
        analysis = case["analysis"]
        for eye_index, eye in enumerate(("left", "right")):
            source = Image.open(case[f"{eye}_image"]).convert("RGB")
            expected = analysis[f"expected_{eye}_point"]
            ring = analysis[f"{eye}_ring"]
            marked = annotate(source, expected, ring).resize((270, 283), Image.Resampling.LANCZOS)
            px = ox + 18 + eye_index * 298
            py = oy + 50
            sheet.paste(marked, (px, py))
            ring_center = ring["centroid"] if ring else expected
            sheet.paste(crop(source, ring_center), (px + 60, py + 292))
            draw.text((px, py + 448), eye.upper(), font=font(14, True), fill=(190, 211, 230))
            if ring:
                draw.text((px, py + 471), f"projection error {ring['expected_point_error_px']:.3f}px", font=font(13), fill=(112, 238, 181))
                draw.text((px, py + 493), f"{ring['dimensions'][0]}x{ring['dimensions'][1]} px, holes {ring['hole_count']}", font=font(13), fill=(176, 204, 220))
                color_line = (
                    f"HSV {tuple(round(v, 1) for v in ring['median_hsv'])} PASS"
                    if ring.get("color_evaluable", True)
                    else "COLOR N/E (exposure-dark geometry)"
                )
                draw.text((px, py + 515), color_line, font=font(13), fill=(176, 204, 220))
            else:
                draw.text((px, py + 471), "RING NOT DETECTED", font=font(13, True), fill=(255, 111, 111))
        draw.text((ox + 18, oy + 592), f"stereo error {analysis['stereo_correspondence_error_px']!s} px | {'PASS' if passed else 'FAIL'}", font=font(14, True), fill=outline)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.output, optimize=True)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
