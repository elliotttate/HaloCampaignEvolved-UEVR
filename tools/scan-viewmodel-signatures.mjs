#!/usr/bin/env node

import { readFileSync } from "node:fs";

const DEFAULT_DLL =
  String.raw`E:\SteamLibrary\steamapps\common\Halo Campaign Evolved\Meteorite\Binaries\Win64\HaloSimulation_tag_release.dll`;

const SIGNATURES = {
  legacy_first_person_build_node_matrices:
    "81 EC F0 00 00 00 53 55 25 FF FF 00 00 56 8B F1 8B 0D",
  compose_bones:
    "45 85 C0 0F 8E ?? ?? ?? ?? 48 89 5C 24 08 57 48 83 EC 20 45 8B D0 49 8B F9 4C 8B C9",
  compose_special_bones:
    "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 55 41 56 48 8D 68 B8",
  first_person_interpolate:
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 33 DB 49 63 E8",
  first_person_visible_palette:
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 49 8B F0 0F B7 C9 4C 8B F2",
  gun_camera_array:
    "48 89 5C 24 08 57 48 83 EC 20 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 81 C3 ?? 28 00 00 48 83 EF 01 75 ??",
  first_person_camera_rebuild:
    "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 50 48 8D 79 08 0F 29 78 E8 F3 0F 10 3D ?? ?? ?? ?? 48 8B F1",
};

function parsePattern(pattern) {
  return pattern.split(/\s+/).map((byte) =>
    byte === "??" ? null : Number.parseInt(byte, 16),
  );
}

function parseExecutableSections(buffer) {
  const peOffset = buffer.readUInt32LE(0x3c);
  const coffOffset = peOffset + 4;
  const sectionCount = buffer.readUInt16LE(coffOffset + 2);
  const optionalHeaderSize = buffer.readUInt16LE(coffOffset + 16);
  const tableOffset = coffOffset + 20 + optionalHeaderSize;
  const sections = [];

  for (let index = 0; index < sectionCount; ++index) {
    const offset = tableOffset + index * 40;
    const name = buffer
      .subarray(offset, offset + 8)
      .toString("ascii")
      .replace(/\0.*$/, "");
    const virtualSize = buffer.readUInt32LE(offset + 8);
    const virtualAddress = buffer.readUInt32LE(offset + 12);
    const rawSize = buffer.readUInt32LE(offset + 16);
    const rawOffset = buffer.readUInt32LE(offset + 20);
    const characteristics = buffer.readUInt32LE(offset + 36);
    if ((characteristics & 0x20000000) !== 0) {
      sections.push({
        name,
        virtualAddress,
        rawOffset,
        scanSize: Math.min(rawSize, virtualSize || rawSize),
      });
    }
  }

  return sections;
}

function scan(buffer, sections, pattern) {
  const bytes = parsePattern(pattern);
  const anchor = bytes.findIndex((byte) => byte !== null);
  const needle = Buffer.from([bytes[anchor]]);
  const matches = [];

  for (const section of sections) {
    const last = section.rawOffset + section.scanSize - bytes.length;
    let cursor = section.rawOffset + anchor;
    while (cursor <= last + anchor) {
      const found = buffer.indexOf(needle, cursor, last + anchor + 1);
      if (found < 0) {
        break;
      }
      const candidate = found - anchor;
      if (
        bytes.every(
          (expected, index) =>
            expected === null || buffer[candidate + index] === expected,
        )
      ) {
        matches.push({
          section: section.name,
          fileOffset: candidate,
          rva:
            section.virtualAddress + candidate - section.rawOffset,
        });
      }
      cursor = found + 1;
    }
  }

  return matches;
}

function hex(value) {
  return `0x${value.toString(16).toUpperCase()}`;
}

const dllPath = process.argv[2] ?? DEFAULT_DLL;
const buffer = readFileSync(dllPath);
const sections = parseExecutableSections(buffer);

console.log(`File: ${dllPath}`);
for (const [name, pattern] of Object.entries(SIGNATURES)) {
  const matches = scan(buffer, sections, pattern);
  console.log(`${name}: ${matches.length} match(es)`);
  for (const match of matches) {
    console.log(
      `  RVA ${hex(match.rva)} file ${hex(match.fileOffset)} section ${match.section}`,
    );
  }
}
