// tests/model.test.js
// Standalone unit tests for Model.js, executable via Deno or Node.
//   deno run --allow-read tests/model.test.js
//   node tests/model.test.js
//
// Model.js is loaded from disk and evaluated. There is deliberately no bundled
// "reference implementation" fallback: a test that can pass without the file it
// is testing is not a test.

let readFileSync;
if (typeof Deno !== "undefined") {
  readFileSync = (p) => Deno.readTextFileSync(p);
} else if (typeof process !== "undefined") {
  const fs = await import("fs");
  readFileSync = (p) => fs.readFileSync(p, "utf-8");
} else {
  throw new Error("Unsupported runtime: expected Deno or Node.js");
}

const CANDIDATE_PATHS = ["Model.js", "../Model.js", "plugin/Model.js"];

let modelSource = null;
let modelPath = null;
for (const p of CANDIDATE_PATHS) {
  try {
    modelSource = readFileSync(p);
    modelPath = p;
    break;
  } catch (_e) {
    // Try next candidate.
  }
}

if (!modelSource) {
  console.error(`[FATAL] Model.js not found. Looked in: ${CANDIDATE_PATHS.join(", ")}`);
  console.error("        Run this from the repository root.");
  if (typeof Deno !== "undefined") Deno.exit(1);
  if (typeof process !== "undefined") process.exit(1);
}
console.log(`[INFO] Loaded Model.js from: ${modelPath}`);

const Model = new Function(
  modelSource +
  `; return {
    SUPPORTED_SCHEMA, LEVEL_UNKNOWN,
    CNC_DEFAULT_MAX, EQ_DEFAULT_MIN, EQ_DEFAULT_MAX,
    EQ_BANDS, SIDETONE_LEVELS,
    defaultStatus, parseStatus, clamp,
    cncLevelName, cycleCncLevel, eqBandLabel, sidetoneName,
    voicePromptsDescription, formatBattery, levelFraction,
    batteryIcon, elideError
  };`
)();

// ---------------------------------------------------------------------------
// Tiny assertion harness
// ---------------------------------------------------------------------------
let passed = 0;
let failed = 0;

function check(label, actual, expected) {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  if (a === e) {
    passed++;
  } else {
    failed++;
    console.error(`  FAIL  ${label}\n        expected ${e}\n        actual   ${a}`);
  }
}

function suite(name, fn) {
  console.log(`\n${name}`);
  fn();
}

function fixture(name) {
  for (const dir of ["tests/fixtures/", "fixtures/", "../tests/fixtures/"]) {
    try {
      return readFileSync(dir + name);
    } catch (_e) {
      // Try next directory.
    }
  }
  throw new Error(`fixture not found: ${name}`);
}

// ---------------------------------------------------------------------------
suite("Suite 1: Empty & malformed input", () => {
  for (const [name, label] of [
    ["status_empty.json", "empty file"],
    ["status_whitespace.json", "whitespace-only file"],
  ]) {
    const r = Model.parseStatus(fixture(name));
    check(`${label} -> not ok`, r.ok, false);
    check(`${label} -> not connected`, r.connected, false);
    check(`${label} -> reports an error`, r.lastError.length > 0, true);
  }

  const corrupted = Model.parseStatus(fixture("status_corrupted.json"));
  check("truncated JSON -> not ok", corrupted.ok, false);
  check("truncated JSON -> reports an error", corrupted.lastError.length > 0, true);

  const nonObject = Model.parseStatus(fixture("status_non_object.json"));
  check("JSON array -> not ok", nonObject.ok, false);

  check("null input -> not ok", Model.parseStatus(null).ok, false);
  check("undefined input -> not ok", Model.parseStatus(undefined).ok, false);
});

// ---------------------------------------------------------------------------
suite("Suite 2: Schema version handling", () => {
  const missing = Model.parseStatus(fixture("status_missing_schema.json"));
  check("missing schema -> not ok", missing.ok, false);
  check("missing schema -> reports an error", missing.lastError.length > 0, true);

  const tooNew = Model.parseStatus(fixture("status_schema_too_new.json"));
  check("future schema -> not ok", tooNew.ok, false);
  check("future schema -> flagged as too new", tooNew.schemaTooNew, true);
  check("future schema -> version retained", tooNew.schemaVersion, 99);
});

// ---------------------------------------------------------------------------
suite("Suite 3: Full payload parsing", () => {
  const r = Model.parseStatus(fixture("status_connected_full.json"));
  check("full -> ok", r.ok, true);
  check("full -> connected", r.connected, true);
  check("full -> schema version", r.schemaVersion, 1);
  check("full -> device name", r.deviceName, "Panthère");
  check("full -> device model", r.deviceModel, "Bose NC Headphones 700");
  check("full -> firmware", r.firmwareVersion, "1.8.2-11524+e0f7590");
  check("full -> address", r.deviceAddress, "4C:87:5D:A3:D1:4F");
  check("full -> battery", r.batteryLevel, 87);
  check("full -> cnc level", r.cncLevel, 0);
  check("full -> cnc max", r.cncMax, 10);
  check("full -> eq bass", r.eqBass, 0);
  check("full -> eq mid", r.eqMid, 2);
  check("full -> eq treble", r.eqTreble, -1);
  check("full -> eq range", [r.eqMin, r.eqMax], [-10, 10]);
  check("full -> sidetone", r.sidetone, "medium");
  check("full -> voice prompts off", r.voicePrompts, false);
  check("full -> prompt language", r.voicePromptsLanguage, "French");
  check("full -> multipoint", r.multipoint, true);
  check("full -> updated_at", r.updatedAt, "2026-09-12T10:00:00Z");
});

// ---------------------------------------------------------------------------
suite("Suite 4: Disconnected payload", () => {
  const r = Model.parseStatus(fixture("status_disconnected.json"));
  check("disconnected -> ok (well-formed)", r.ok, true);
  check("disconnected -> not connected", r.connected, false);
  check("disconnected -> battery unknown", r.batteryLevel, Model.LEVEL_UNKNOWN);
  check("disconnected -> no device name", r.deviceName, "");
  check("disconnected -> updated_at still read", r.updatedAt, "2026-09-12T10:05:00Z");
});

// ---------------------------------------------------------------------------
suite("Suite 5: Clamping & unknown vocabulary", () => {
  const r = Model.parseStatus(fixture("status_extreme_values.json"));
  check("battery 250 -> clamped to 100", r.batteryLevel, 100);
  check("cnc 99 -> clamped to max", r.cncLevel, 10);
  check("eq bass -99 -> clamped", r.eqBass, -10);
  check("eq mid 99 -> clamped", r.eqMid, 10);
  check("eq treble non-numeric -> default", r.eqTreble, 0);
  check("unknown sidetone -> unknown", r.sidetone, "unknown");
  check("non-boolean prompts flag -> treated as on", r.voicePrompts, true);
  check("non-string language -> stringified", r.voicePromptsLanguage, "42");
  check("non-boolean multipoint -> off", r.multipoint, false);

  check("clamp non-numeric -> default", Model.clamp("abc", 0, 10, 7), 7);
  check("clamp rounds", Model.clamp(3.7, 0, 10, 0), 4);
});

// ---------------------------------------------------------------------------
suite("Suite 6: Minimal payload falls back to defaults", () => {
  // The daemon omits whatever the headset has not answered yet; every group
  // may simply be absent.
  const r = Model.parseStatus(fixture("status_connected_minimal.json"));
  check("minimal -> ok", r.ok, true);
  check("minimal -> connected", r.connected, true);
  check("minimal -> battery unknown", r.batteryLevel, Model.LEVEL_UNKNOWN);
  check("minimal -> cnc defaults", [r.cncLevel, r.cncMax], [0, Model.CNC_DEFAULT_MAX]);
  check("minimal -> eq defaults", [r.eqBass, r.eqMid, r.eqTreble], [0, 0, 0]);
  check("minimal -> eq range defaults", [r.eqMin, r.eqMax], [Model.EQ_DEFAULT_MIN, Model.EQ_DEFAULT_MAX]);
  check("minimal -> sidetone unknown", r.sidetone, "unknown");
  check("minimal -> voice prompts default on", r.voicePrompts, true);
  check("minimal -> no prompt language", r.voicePromptsLanguage, "");
  check("minimal -> multipoint default off", r.multipoint, false);
  check("minimal -> no firmware", r.firmwareVersion, "");
});

// ---------------------------------------------------------------------------
suite("Suite 7: Vocabulary shared with the daemon", () => {
  // These lists are the plugin's half of a contract with bose-700-ctl. If one
  // drifts, the panel starts issuing commands the daemon rejects.
  check("sidetone levels", Model.SIDETONE_LEVELS, ["off", "low", "medium", "high"]);
  check("eq bands", Model.EQ_BANDS, ["bass", "mid", "treble"]);
  check("default cnc max", Model.CNC_DEFAULT_MAX, 10);
  check("default eq range", [Model.EQ_DEFAULT_MIN, Model.EQ_DEFAULT_MAX], [-10, 10]);
});

// ---------------------------------------------------------------------------
suite("Suite 8: Display helpers", () => {
  check("cnc 0 is transparency", Model.cncLevelName(0, 10), "Transparency");
  check("cnc max is max ANC", Model.cncLevelName(10, 10), "Max ANC");
  check("cnc mid names the level", Model.cncLevelName(5, 10), "ANC 5");

  check("cycle max -> transparency", Model.cycleCncLevel(10, 10), 0);
  check("cycle transparency -> halfway", Model.cycleCncLevel(0, 10), 5);
  check("cycle mid -> max", Model.cycleCncLevel(5, 10), 10);
  check("cycle without max uses 10", Model.cycleCncLevel(7), 10);

  check("sidetone names", Model.SIDETONE_LEVELS.map(Model.sidetoneName),
        ["Off", "Low", "Medium", "High"]);
  check("sidetone unknown name", Model.sidetoneName("unknown"), "Unknown");

  check("eq band labels", Model.EQ_BANDS.map(Model.eqBandLabel), ["Bass", "Mid", "Treble"]);

  check("prompts on with language", Model.voicePromptsDescription(true, "French"), "Spoken prompts in French");
  check("prompts off", Model.voicePromptsDescription(false, "French"), "Spoken prompts are off");

  check("formatBattery(87)", Model.formatBattery(87), "87%");
  check("formatBattery(0)", Model.formatBattery(0), "0%");
  check("formatBattery(-1)", Model.formatBattery(-1), "—");

  check("levelFraction(87)", Model.levelFraction(87), 0.87);
  check("levelFraction(100)", Model.levelFraction(100), 1.0);
  check("levelFraction(-1)", Model.levelFraction(-1), 0.0);

  check("batteryIcon is a string", typeof Model.batteryIcon(50), "string");
  check("batteryIcon low differs from full", Model.batteryIcon(5) !== Model.batteryIcon(100), true);

  const longErr = "Error: " + "a".repeat(200);
  const elided = Model.elideError(longErr);
  check("elideError length <= 140", elided.length <= 140, true);
  check("elideError ends with ellipsis", elided.endsWith("…"), true);
  check("elideError('') is empty", Model.elideError(""), "");
  check("elideError collapses whitespace", Model.elideError("a  \n b"), "a b");
});

// ---------------------------------------------------------------------------
console.log(`\nSummary: ${passed} passed, ${failed} failed`);
if (failed > 0) {
  if (typeof Deno !== "undefined") Deno.exit(1);
  if (typeof process !== "undefined") process.exit(1);
}
