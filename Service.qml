// plugin/Service.qml
// Reactive state and command dispatch for Bose Noise Cancelling Headphones 700.
import QtQuick
import Quickshell
import Quickshell.Io
import "Model.js" as Model

Item {
  id: root

  property var settings: ({})

  readonly property string cliBinary: {
    var home = Quickshell.env("HOME")
    return home ? (home + "/.local/bin/bose-700-ctl") : "bose-700-ctl"
  }

  // Path to status.json ($XDG_STATE_HOME/bose-700/status.json)
  readonly property string statusPath: {
    var xdg = Quickshell.env("XDG_STATE_HOME")
    var home = Quickshell.env("HOME")
    var base = xdg ? xdg : (home ? home + "/.local/state" : "/tmp")
    return base + "/bose-700/status.json"
  }

  // ---------------------------------------------------------------------------
  // State
  //
  // `_real` is the last status the daemon wrote. `_pending` holds values the
  // user just chose, each with its own expiry, so a control reflects a click
  // immediately and falls back to the truth if the headset never confirms it.
  // Both objects are replaced rather than mutated so that bindings notice.
  // ---------------------------------------------------------------------------
  property var _real: Model.defaultStatus()
  property var _pending: ({})

  function _value(key) {
    return _pending.hasOwnProperty(key) ? _pending[key].value : _real[key]
  }

  function _setPending(key, value, ttlMs) {
    var next = Object.assign({}, _pending)
    next[key] = { value: value, until: Date.now() + (ttlMs || 4000) }
    _pending = next
    pendingTimer.start()
  }

  // Report-only fields: nothing the user sets directly.
  readonly property bool ok: _real.ok === true
  readonly property string lastErrorFromStatus: _real.lastError || ""
  readonly property bool schemaTooNew: _real.schemaTooNew === true
  readonly property bool connected: _real.connected === true
  readonly property string deviceName: _real.deviceName || ""
  readonly property string deviceModel: _real.deviceModel || ""
  readonly property string deviceAddress: _real.deviceAddress || ""
  readonly property int batteryLevel: _real.batteryLevel
  readonly property string firmwareVersion: _real.firmwareVersion || ""
  readonly property int cncMax: _real.cncMax || Model.CNC_DEFAULT_MAX
  readonly property int eqMin: _real.eqMin !== undefined ? _real.eqMin : Model.EQ_DEFAULT_MIN
  readonly property int eqMax: _real.eqMax !== undefined ? _real.eqMax : Model.EQ_DEFAULT_MAX
  readonly property string voicePromptsLanguage: _real.voicePromptsLanguage || ""

  // Settable fields: pending value if there is one, otherwise the daemon's.
  readonly property int cncLevel: _value("cncLevel")
  readonly property int eqBass: _value("eqBass")
  readonly property int eqMid: _value("eqMid")
  readonly property int eqTreble: _value("eqTreble")
  readonly property string sidetone: _value("sidetone")
  readonly property bool voicePrompts: _value("voicePrompts") === true
  readonly property bool multipoint: _value("multipoint") === true

  property string lastError: ""

  // Drops pending values once they expire; idle whenever nothing is pending.
  Timer {
    id: pendingTimer
    interval: 500
    repeat: true
    onTriggered: {
      var now = Date.now()
      var next = {}
      var kept = 0
      for (var key in root._pending) {
        if (root._pending[key].until > now) {
          next[key] = root._pending[key]
          kept++
        }
      }
      root._pending = next
      if (kept === 0) stop()
    }
  }

  // ---------------------------------------------------------------------------
  // Command dispatch: one bose-700-ctl process at a time, in order.
  // ---------------------------------------------------------------------------
  property var commandQueue: []

  function runCommand(args) {
    commandQueue.push([cliBinary].concat(args))
    dispatchNext()
  }

  function dispatchNext() {
    if (ctlProcess.running || commandQueue.length === 0) return
    ctlProcess.command = commandQueue.shift()
    ctlProcess.running = true
  }

  Process {
    id: ctlProcess
    running: false
    stdout: StdioCollector { id: ctlStdout; waitForEnd: true }
    stderr: StdioCollector { id: ctlStderr; waitForEnd: true }
    onExited: function(exitCode) {
      if (exitCode !== 0) {
        var err = String(ctlStderr.text || ctlStdout.text || "").trim()
        if (err) root.lastError = Model.elideError(err)
      }
      dispatchNext()
    }
  }

  // Reactive FileView watcher (no polling).
  FileView {
    id: fileView
    path: root.statusPath
    watchChanges: true
    atomicWrites: true
    printErrors: false
    onLoaded: {
      var content = typeof text === "function" ? text() : (fileView.text || "")
      root.applyStatus(content)
    }
    onLoadFailed: root.applyStatus("")
    onFileChanged: reload()
  }

  function applyStatus(raw) {
    var parsed = Model.parseStatus(raw)
    _real = parsed

    // A pending value the headset has now confirmed is no longer pending.
    var next = {}
    for (var key in _pending) {
      if (JSON.stringify(parsed[key]) !== JSON.stringify(_pending[key].value)) {
        next[key] = _pending[key]
      }
    }
    _pending = next
  }

  // ---------------------------------------------------------------------------
  // Setters
  // ---------------------------------------------------------------------------
  function setCncLevel(level) {
    var clamped = Model.clamp(level, 0, cncMax, 0)
    _setPending("cncLevel", clamped)
    runCommand(["cnc", String(clamped)])
  }

  // The headset takes the three EQ bands together; changing one resends all.
  function setEqBands(bass, mid, treble) {
    var b = Model.clamp(bass, eqMin, eqMax, 0)
    var m = Model.clamp(mid, eqMin, eqMax, 0)
    var t = Model.clamp(treble, eqMin, eqMax, 0)
    _setPending("eqBass", b)
    _setPending("eqMid", m)
    _setPending("eqTreble", t)
    runCommand(["eq", String(b), String(m), String(t)])
  }

  function setEqBand(band, value) {
    if (Model.EQ_BANDS.indexOf(band) === -1) return
    var b = eqBass, m = eqMid, t = eqTreble
    if (band === "bass") b = value
    else if (band === "mid") m = value
    else t = value
    setEqBands(b, m, t)
  }

  function setSidetone(level) {
    if (Model.SIDETONE_LEVELS.indexOf(level) === -1 || level === sidetone) return
    _setPending("sidetone", level)
    runCommand(["sidetone", level])
  }

  function setVoicePrompts(enabled) {
    _setPending("voicePrompts", enabled === true)
    runCommand(["prompts", enabled ? "on" : "off"])
  }

  function setMultipoint(enabled) {
    // Toggling multipoint can make the headset drop and re-establish links.
    _setPending("multipoint", enabled === true, 12000)
    runCommand(["multipoint", enabled ? "on" : "off"])
  }

  function reconnect() {
    runCommand(["reconnect"])
  }

  function cycleCnc() {
    setCncLevel(Model.cycleCncLevel(cncLevel, cncMax))
  }

  function refresh() {
    fileView.reload()
  }

  Component.onCompleted: fileView.reload()
}
