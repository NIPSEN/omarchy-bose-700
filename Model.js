// plugin/Model.js
// Pure ECMAScript model library for Bose Noise Cancelling Headphones 700.
// Zero QML dependencies; runnable in QML, Deno, and Node.js runtimes.

var SUPPORTED_SCHEMA = 1;
var LEVEL_UNKNOWN = -1;

// The NC700 exposes noise control as one continuous 0-10 axis: 0 is full
// transparency (outside sound let in), 10 is maximum noise cancelling.
var CNC_DEFAULT_MAX = 10;

var EQ_DEFAULT_MIN = -10;
var EQ_DEFAULT_MAX = 10;
var EQ_BANDS = ["bass", "mid", "treble"];

var SIDETONE_LEVELS = ["off", "low", "medium", "high"];

var MAX_ERROR_CHARS = 140;
var ELIDED_ERROR_CHARS = 137;

function defaultStatus() {
  return {
    ok: false,
    lastError: "",
    schemaVersion: 0,
    schemaTooNew: false,
    connected: false,
    deviceName: "",
    deviceModel: "",
    deviceAddress: "",
    batteryLevel: LEVEL_UNKNOWN,
    cncLevel: 0,
    cncMax: CNC_DEFAULT_MAX,
    eqBass: 0,
    eqMid: 0,
    eqTreble: 0,
    eqMin: EQ_DEFAULT_MIN,
    eqMax: EQ_DEFAULT_MAX,
    sidetone: "unknown",
    voicePrompts: true,
    voicePromptsLanguage: "",
    multipoint: false,
    firmwareVersion: "",
    updatedAt: ""
  };
}

function clamp(val, min, max, def) {
  var n = Number(val);
  if (isNaN(n)) return def;
  return Math.max(min, Math.min(max, Math.round(n)));
}

function oneOf(list, value, fallback) {
  var v = String(value || "").toLowerCase();
  return list.indexOf(v) !== -1 ? v : fallback;
}

function parseStatus(raw) {
  if (raw === null || raw === undefined) {
    var res = defaultStatus();
    res.lastError = "The bose status file is empty";
    return res;
  }
  var text = String(raw).trim();
  if (!text) {
    var res = defaultStatus();
    res.lastError = "The bose status file is empty";
    return res;
  }

  var parsed;
  try {
    parsed = JSON.parse(text);
  } catch (_e) {
    var res = defaultStatus();
    res.lastError = "Could not read the bose status file";
    return res;
  }

  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
    var res = defaultStatus();
    res.lastError = "The bose status file is invalid";
    return res;
  }

  if (parsed.schema === undefined || parsed.schema === null) {
    var res = defaultStatus();
    res.lastError = "The bose status file carried no schema";
    return res;
  }

  var version = Number(parsed.schema);
  if (isNaN(version) || version > SUPPORTED_SCHEMA) {
    var res = defaultStatus();
    res.schemaTooNew = true;
    res.schemaVersion = isNaN(version) ? 0 : version;
    res.lastError = "bose daemon speaks status schema " + version + ", this panel reads " + SUPPORTED_SCHEMA;
    return res;
  }

  var res = defaultStatus();
  res.ok = true;
  res.schemaVersion = version;
  res.connected = parsed.connected === true;
  res.updatedAt = String(parsed.updated_at || "");

  if (!res.connected) {
    return res;
  }

  // Every group is optional: the daemon omits what the headset has not
  // answered yet, and the panel must still render.
  var device = (parsed.device && typeof parsed.device === "object") ? parsed.device : {};
  res.deviceName = String(device.name || "");
  res.deviceModel = String(device.model || "");
  res.firmwareVersion = String(device.firmware || "");
  res.deviceAddress = String(device.address || "");

  var battery = (parsed.battery && typeof parsed.battery === "object") ? parsed.battery : {};
  if (battery.level !== undefined && battery.level !== null) {
    var bl = Number(battery.level);
    if (!isNaN(bl) && bl >= 0) {
      res.batteryLevel = Math.min(100, Math.round(bl));
    } else {
      res.batteryLevel = LEVEL_UNKNOWN;
    }
  }

  var cnc = (parsed.cnc && typeof parsed.cnc === "object") ? parsed.cnc : {};
  res.cncMax = clamp(cnc.max, 1, CNC_DEFAULT_MAX, CNC_DEFAULT_MAX);
  res.cncLevel = clamp(cnc.level, 0, res.cncMax, 0);

  var eq = (parsed.eq && typeof parsed.eq === "object") ? parsed.eq : {};
  res.eqMin = clamp(eq.min, EQ_DEFAULT_MIN, 0, EQ_DEFAULT_MIN);
  res.eqMax = clamp(eq.max, 0, EQ_DEFAULT_MAX, EQ_DEFAULT_MAX);
  res.eqBass = clamp(eq.bass, res.eqMin, res.eqMax, 0);
  res.eqMid = clamp(eq.mid, res.eqMin, res.eqMax, 0);
  res.eqTreble = clamp(eq.treble, res.eqMin, res.eqMax, 0);

  res.sidetone = oneOf(SIDETONE_LEVELS, parsed.sidetone, "unknown");

  var prompts = (parsed.voice_prompts && typeof parsed.voice_prompts === "object") ? parsed.voice_prompts : {};
  res.voicePrompts = prompts.enabled !== false;
  res.voicePromptsLanguage = String(prompts.language || "");

  res.multipoint = parsed.multipoint === true;

  return res;
}

// ---------------------------------------------------------------------------
// Display names
// ---------------------------------------------------------------------------

// One-word summary of where the CNC slider sits, for the bar tooltip.
function cncLevelName(level, max) {
  if (level <= 0) return "Transparency";
  if (max !== undefined && level >= max) return "Max ANC";
  return "ANC " + level;
}

function eqBandLabel(band) {
  switch (band) {
    case "bass": return "Bass";
    case "mid": return "Mid";
    case "treble": return "Treble";
    default: return band;
  }
}

function sidetoneName(level) {
  switch (level) {
    case "off": return "Off";
    case "low": return "Low";
    case "medium": return "Medium";
    case "high": return "High";
    default: return "Unknown";
  }
}

function voicePromptsDescription(enabled, language) {
  if (!enabled) return "Spoken prompts are off";
  return language ? "Spoken prompts in " + language : "Spoken prompts on connect, battery and mode changes";
}

// Right-click cycle on the bar widget: max ANC -> transparency -> halfway ->
// max ANC. Three stops, like the headset's own noise-control button.
function cycleCncLevel(current, max) {
  var hi = max !== undefined && max > 0 ? max : CNC_DEFAULT_MAX;
  if (current >= hi) return 0;
  if (current <= 0) return Math.floor(hi / 2);
  return hi;
}

function formatBattery(level) {
  if (level === undefined || level === null || level < 0) return "—";
  return Math.round(level) + "%";
}

function levelFraction(level) {
  if (level === undefined || level === null || level < 0) return 0.0;
  return Math.max(0.0, Math.min(1.0, level / 100.0));
}

function batteryIcon(level) {
  if (level < 0) return "󰂃";
  if (level >= 95) return "󰁹";
  if (level >= 85) return "󰂂";
  if (level >= 75) return "󰂁";
  if (level >= 65) return "󰂀";
  if (level >= 55) return "󰁿";
  if (level >= 45) return "󰁾";
  if (level >= 35) return "󰁽";
  if (level >= 25) return "󰁼";
  if (level >= 15) return "󰁻";
  return "󰂎";
}

function elideError(text) {
  if (!text) return "";
  var cleaned = String(text).replace(/\s+/g, " ").trim();
  if (cleaned.length > MAX_ERROR_CHARS) {
    return cleaned.substring(0, ELIDED_ERROR_CHARS) + "…";
  }
  return cleaned;
}
