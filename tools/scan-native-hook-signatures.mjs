#!/usr/bin/env node

import { createHash } from "node:crypto";
import { readFileSync, statSync } from "node:fs";

const DEFAULT_DLL =
  String.raw`E:\SteamLibrary\steamapps\common\Halo Campaign Evolved\Meteorite\Binaries\Win64\HaloSimulation_tag_release.dll`;
const IMAGE_SCN_MEM_EXECUTE = 0x20000000;
const PRIMARY_CALL_OFFSET = 26;
const PRIMARY_RETURN_OFFSET = 31;

function parsePattern(text) {
  return text.split(/\s+/).map((token) => {
    if (token === "??") {
      return null;
    }

    const value = Number.parseInt(token, 16);
    if (!Number.isInteger(value) || value < 0 || value > 0xff) {
      throw new Error(`Invalid pattern byte: ${token}`);
    }
    return value;
  });
}

// Keep these byte-for-byte aligned with src/main.cpp.
const SIGNATURES = [
  {
    name: "trigger_create_projectiles",
    pattern: parsePattern(
      "44 88 4C 24 20 4C 89 44 24 18 66 89 54 24 10 89 4C 24 08 " +
        "53 57 41 55 41 57 B8 38 26 00 00 E8 ?? ?? ?? ?? 48 2B E0",
    ),
  },
  {
    name: "object_get_markers",
    pattern: parsePattern(
      "66 44 89 4C 24 20 53 55 56 57 41 56 41 57 48 83 EC 68 45 33 " +
        "FF 8B F1 49 8B D8 44 8B F2",
    ),
  },
  {
    name: "primary_marker_call",
    pattern: parsePattern(
      "C6 44 24 30 00 41 B9 40 00 00 00 C6 44 24 20 00 4C 8D 84 24 " +
        "80 09 00 00 8B D7 E8 ?? ?? ?? ?? 44 0F B7 C0",
    ),
  },
];

function hex(value, width = 0) {
  return `0x${value.toString(16).toUpperCase().padStart(width, "0")}`;
}

function requireRange(buffer, offset, size, label) {
  if (
    !Number.isSafeInteger(offset) ||
    !Number.isSafeInteger(size) ||
    offset < 0 ||
    size < 0 ||
    offset + size > buffer.length
  ) {
    throw new Error(
      `${label} lies outside the file (offset=${hex(offset)}, size=${hex(size)})`,
    );
  }
}

function parsePe(buffer) {
  requireRange(buffer, 0, 0x40, "DOS header");
  if (buffer.readUInt16LE(0) !== 0x5a4d) {
    throw new Error("Missing DOS MZ signature");
  }

  const peOffset = buffer.readUInt32LE(0x3c);
  requireRange(buffer, peOffset, 24, "PE and COFF headers");
  if (buffer.readUInt32LE(peOffset) !== 0x00004550) {
    throw new Error("Missing PE signature");
  }

  const coffOffset = peOffset + 4;
  const machine = buffer.readUInt16LE(coffOffset);
  const sectionCount = buffer.readUInt16LE(coffOffset + 2);
  const optionalHeaderSize = buffer.readUInt16LE(coffOffset + 16);
  const optionalHeaderOffset = coffOffset + 20;
  requireRange(
    buffer,
    optionalHeaderOffset,
    optionalHeaderSize,
    "optional header",
  );

  const optionalMagic = buffer.readUInt16LE(optionalHeaderOffset);
  if (optionalMagic !== 0x20b && optionalMagic !== 0x10b) {
    throw new Error(`Unsupported optional-header magic ${hex(optionalMagic)}`);
  }

  const imageSize = buffer.readUInt32LE(optionalHeaderOffset + 56);
  const sectionTableOffset = optionalHeaderOffset + optionalHeaderSize;
  requireRange(
    buffer,
    sectionTableOffset,
    sectionCount * 40,
    "section table",
  );

  const sections = [];
  for (let index = 0; index < sectionCount; ++index) {
    const offset = sectionTableOffset + index * 40;
    const rawName = buffer.subarray(offset, offset + 8);
    const nul = rawName.indexOf(0);
    const name = rawName
      .subarray(0, nul >= 0 ? nul : rawName.length)
      .toString("ascii");
    const virtualSize = buffer.readUInt32LE(offset + 8);
    const virtualAddress = buffer.readUInt32LE(offset + 12);
    const rawSize = buffer.readUInt32LE(offset + 16);
    const rawOffset = buffer.readUInt32LE(offset + 20);
    const characteristics = buffer.readUInt32LE(offset + 36);

    if (rawSize > 0) {
      requireRange(buffer, rawOffset, rawSize, `section ${name}`);
    }

    sections.push({
      name,
      virtualSize,
      virtualAddress,
      rawSize,
      rawOffset,
      characteristics,
      executable: (characteristics & IMAGE_SCN_MEM_EXECUTE) !== 0,
    });
  }

  return {
    machine,
    optionalMagic,
    imageSize,
    sections,
  };
}

function patternMatches(buffer, offset, pattern) {
  for (let index = 0; index < pattern.length; ++index) {
    const expected = pattern[index];
    if (expected !== null && buffer[offset + index] !== expected) {
      return false;
    }
  }
  return true;
}

function scanExecutableSections(buffer, pe, signature) {
  const firstConcreteIndex = signature.pattern.findIndex(
    (value) => value !== null,
  );
  if (firstConcreteIndex < 0) {
    throw new Error(`${signature.name} contains no concrete bytes`);
  }

  const firstConcreteByte = signature.pattern[firstConcreteIndex];
  const needle = Buffer.from([firstConcreteByte]);
  const matches = [];

  for (const section of pe.sections.filter((item) => item.executable)) {
    const virtualSpan = section.virtualSize || section.rawSize;
    const scanSize = Math.min(section.rawSize, virtualSpan);
    if (scanSize < signature.pattern.length) {
      continue;
    }

    const sectionBegin = section.rawOffset;
    const lastStart =
      sectionBegin + scanSize - signature.pattern.length;
    let searchFrom = sectionBegin + firstConcreteIndex;

    while (searchFrom <= lastStart + firstConcreteIndex) {
      const concreteOffset = buffer.indexOf(
        needle,
        searchFrom,
        lastStart + firstConcreteIndex + 1,
      );
      if (concreteOffset < 0) {
        break;
      }

      const candidateOffset = concreteOffset - firstConcreteIndex;
      if (
        candidateOffset >= sectionBegin &&
        patternMatches(buffer, candidateOffset, signature.pattern)
      ) {
        matches.push({
          section: section.name,
          fileOffset: candidateOffset,
          rva:
            section.virtualAddress + (candidateOffset - section.rawOffset),
        });
      }

      searchFrom = concreteOffset + 1;
    }
  }

  return matches;
}

function resolvePrimaryCall(buffer, primaryMatch) {
  const opcodeOffset = primaryMatch.fileOffset + PRIMARY_CALL_OFFSET;
  requireRange(buffer, opcodeOffset, 5, "primary marker call instruction");
  if (buffer[opcodeOffset] !== 0xe8) {
    throw new Error(
      `Expected E8 at primary call +${hex(PRIMARY_CALL_OFFSET)}, found ${hex(buffer[opcodeOffset], 2)}`,
    );
  }

  const displacement = buffer.readInt32LE(opcodeOffset + 1);
  return {
    displacement,
    returnRva: primaryMatch.rva + PRIMARY_RETURN_OFFSET,
    targetRva:
      primaryMatch.rva + PRIMARY_RETURN_OFFSET + displacement,
  };
}

function usage() {
  console.log(
    "Usage: node tools/scan-native-hook-signatures.mjs [--json] [HaloSimulation_tag_release.dll]",
  );
  console.log(
    "The DLL may also be supplied through the HALO_SIMULATION_DLL environment variable.",
  );
}

function main() {
  const arguments_ = process.argv.slice(2);
  const wantsJson = arguments_.includes("--json");
  const positional = arguments_.filter((value) => value !== "--json");

  if (positional.includes("--help") || positional.includes("-h")) {
    usage();
    return 0;
  }
  if (positional.length > 1) {
    usage();
    return 1;
  }

  const dllPath =
    positional[0] ?? process.env.HALO_SIMULATION_DLL ?? DEFAULT_DLL;
  const buffer = readFileSync(dllPath);
  const fileStat = statSync(dllPath);
  const pe = parsePe(buffer);
  const results = Object.fromEntries(
    SIGNATURES.map((signature) => [
      signature.name,
      scanExecutableSections(buffer, pe, signature),
    ]),
  );

  const primaryMatches = results.primary_marker_call;
  const resolutions = primaryMatches.map((match) =>
    resolvePrimaryCall(buffer, match),
  );
  const markerRvas = new Set(
    results.object_get_markers.map((match) => match.rva),
  );
  const validationPassed =
    SIGNATURES.every((signature) => results[signature.name].length === 1) &&
    resolutions.length === 1 &&
    markerRvas.has(resolutions[0].targetRva);

  const report = {
    path: dllPath,
    bytes: buffer.length,
    modifiedUtc: fileStat.mtime.toISOString(),
    sha256: createHash("sha256").update(buffer).digest("hex").toUpperCase(),
    pe: {
      machine: pe.machine,
      optionalMagic: pe.optionalMagic,
      imageSize: pe.imageSize,
      executableSections: pe.sections
        .filter((section) => section.executable)
        .map((section) => ({
          name: section.name,
          rva: section.virtualAddress,
          virtualSize: section.virtualSize,
          rawOffset: section.rawOffset,
          rawSize: section.rawSize,
        })),
    },
    signatures: results,
    primaryCallResolutions: resolutions.map((resolution) => ({
      ...resolution,
      matchesObjectGetMarkers: markerRvas.has(resolution.targetRva),
    })),
    validationPassed,
  };

  if (wantsJson) {
    console.log(JSON.stringify(report, null, 2));
  } else {
    console.log(`File: ${report.path}`);
    console.log(
      `Size: ${report.bytes} bytes  SHA-256: ${report.sha256}`,
    );
    console.log(
      `PE: machine ${hex(pe.machine, 4)}, image size ${hex(pe.imageSize)}`,
    );
    console.log("Executable sections:");
    for (const section of report.pe.executableSections) {
      console.log(
        `  ${section.name}: RVA ${hex(section.rva)}, virtual ${hex(section.virtualSize)}, raw ${hex(section.rawSize)}`,
      );
    }

    for (const signature of SIGNATURES) {
      const matches = results[signature.name];
      console.log(`${signature.name}: ${matches.length} match(es)`);
      matches.forEach((match, index) => {
        console.log(
          `  [${index}] RVA ${hex(match.rva)}  file ${hex(match.fileOffset)}  section ${match.section}`,
        );
      });
    }

    resolutions.forEach((resolution, index) => {
      console.log(
        `primary_marker_call[${index}] target: ${hex(resolution.targetRva)} ` +
          `(return ${hex(resolution.returnRva)}, displacement ${resolution.displacement})`,
      );
      console.log(
        `  target matches object_get_markers: ${markerRvas.has(resolution.targetRva) ? "yes" : "NO"}`,
      );
    });
    console.log(`Validation: ${validationPassed ? "PASS" : "FAIL"}`);
  }

  return validationPassed ? 0 : 2;
}

try {
  process.exitCode = main();
} catch (error) {
  console.error(
    `scan-native-hook-signatures: ${error instanceof Error ? error.message : String(error)}`,
  );
  process.exitCode = 1;
}
