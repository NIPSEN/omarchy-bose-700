// plugin/Service.qml
// Reactive state and command dispatch for Bose Noise Cancelling Headphones 700.
//
// The widget runs inside the long-lived shell process, so it takes the
// narrowest path it can to the daemon: one UNIX socket, and nothing else.
//
//   - It starts no processes. There is no executable path to resolve, nothing
//     is looked up through PATH, and a replaced binary somewhere in the user's
//     path cannot be run from here.
//   - It reads no files. The daemon pushes its state over the same socket
//     after `subscribe`, so no state file is opened by the shell.
//   - The socket path is pinned to this login session's runtime directory
//     (/run/user/<uid>), which the kernel creates and only this user may
//     enter. Anything else, including an unset or relative XDG_RUNTIME_DIR,
//     is refused rather than fallen back on.
//   - Everything read is bounded before it is buffered. The parser hands over
//     raw chunks (an empty splitMarker), and each chunk is counted against
//     maxRxBytes before anything is appended or searched for a delimiter, so a
//     peer that never sends a newline cannot grow the shell's memory. A daemon
//     that stops answering within responseTimeoutMs is dropped as well.
//   - The peer is treated as untrusted input regardless. The socket lives in a
//     directory only this user can enter, and the service manager holds the
//     listening socket for the whole session (bose-700.socket), so the name is
//     never unbound between daemon restarts.
pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Io
import "Model.js" as Model

Item {
  id: root

  property var settings: ({})

  // ---------------------------------------------------------------------------
  // Connection to the daemon
  // ---------------------------------------------------------------------------

  // The per-user runtime directory, accepted only in its expected shape.
  readonly property string runtimeDir: {
    var dir = String(Quickshell.env("XDG_RUNTIME_DIR") || "")
    while (dir.length > 1 && dir.charAt(dir.length - 1) === "/") {
      dir = dir.substring(0, dir.length - 1)
    }
    return /^\/run\/user\/[0-9]+$/.test(dir) ? dir : ""
  }
  readonly property bool socketPathTrusted: runtimeDir !== ""
  readonly property string socketPath: socketPathTrusted ? runtimeDir + "/bose-700.sock" : ""

  readonly property int maxRxBytes: 65536        // a status line is well under 4 KiB
  readonly property int maxCommandBytes: 512
  readonly property int maxOutstanding: 32
  readonly property int responseTimeoutMs: 5000
  readonly property int maxBackoffMs: 10000

  property int _outstanding: 0
  property int _backoffMs: 1000
  property string _rx: ""            // bytes received since the last newline

  // ---------------------------------------------------------------------------
  // State
  //
  // `_real` is the last status the daemon sent. `_pending` holds values the
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
  // Socket
  //
  // Each attempt gets a brand new Socket. Quickshell keeps its own target
  // state, so re-setting `connected` on a socket whose attempt already failed
  // does nothing; recreating the object makes every retry a real attempt.
  // ---------------------------------------------------------------------------
  Component {
    id: linkComponent

    Socket {
      id: sock
      path: root.socketPath
      connected: true

      // An empty split marker delivers each chunk as it arrives instead of
      // buffering until a delimiter, which lets the byte limit be enforced
      // before anything is accumulated here.
      parser: SplitParser {
        splitMarker: ""
        onRead: function(data) { root._onChunk(data) }
      }

      // The socket connects while the Loader is still constructing it, so the
      // handlers pass themselves rather than going through `linkLoader.item`,
      // which is only published once construction finishes.
      onConnectionStateChanged: root._onLinkState(sock)
      // The parameter is a QLocalSocket error enum, and the reason does not
      // change what we do: back off and try again with a fresh socket.
      onError: root._onLinkLost("Cannot reach the Bose daemon")
    }
  }

  Loader {
    id: linkLoader
    active: false
    sourceComponent: linkComponent
  }

  readonly property bool linked: linkLoader.item ? linkLoader.item.connected === true : false

  function _write(text) {
    var sock = linkLoader.item
    if (!sock || !sock.connected) return false
    sock.write(text)
    sock.flush()
    return true
  }

  function _onLinkState(sock) {
    if (sock && sock.connected) {
      root._rx = ""
      root._outstanding = 0
      root._backoffMs = 1000
      root.lastError = ""
      // Ask for the state now and on every change, so nothing polls and no
      // file is read.
      sock.write("subscribe\n")
      sock.flush()
      handshake.restart()
    } else {
      root._onLinkLost("The Bose daemon is not running")
    }
  }

  function _onLinkLost(reason) {
    deadline.stop()
    handshake.stop()
    root._rx = ""
    root._outstanding = 0
    root._offline(reason)
    root._scheduleReconnect()
  }

  // A daemon that goes quiet is dropped rather than left holding the queue:
  // `handshake` covers the first status after subscribing, `deadline` covers
  // commands still waiting to be acknowledged.
  Timer {
    id: handshake
    interval: root.responseTimeoutMs
    repeat: false
    onTriggered: root._giveUp("The Bose daemon did not send its state")
  }

  Timer {
    id: deadline
    interval: root.responseTimeoutMs
    repeat: false
    onTriggered: root._giveUp("The Bose daemon stopped responding")
  }

  // Drops a wedged daemon: the socket object goes away, so nothing it might
  // still send can reach the shell, and a fresh attempt is scheduled.
  function _giveUp(reason) {
    root.lastError = reason
    root._onLinkLost(reason)
  }

  Timer {
    id: reconnectTimer
    interval: root._backoffMs
    repeat: false
    onTriggered: root._connect()
  }

  function _connect() {
    if (!root.socketPathTrusted) {
      root._offline("XDG_RUNTIME_DIR is not this session's runtime directory")
      return
    }
    if (root.linked) return
    linkLoader.active = false
    linkLoader.active = true
  }

  function _scheduleReconnect() {
    if (!root.socketPathTrusted) return
    // Tearing the socket down from inside its own signal handler is not safe,
    // so it happens once the current handler has returned.
    Qt.callLater(function() { linkLoader.active = false })
    reconnectTimer.interval = root._backoffMs
    root._backoffMs = Math.min(root._backoffMs * 2, root.maxBackoffMs)
    reconnectTimer.restart()
  }

  // Everything the widget shows falls back to "unknown" while there is no link.
  function _offline(reason) {
    var blank = Model.defaultStatus()
    blank.lastError = reason
    root._real = blank
    root._pending = ({})
  }

  // Raw bytes, straight off the socket. The limit covers the new chunk plus
  // whatever is already held, and is applied before the append and before any
  // search for a newline, so an endless line is cut off at maxRxBytes rather
  // than buffered.
  function _onChunk(data) {
    var chunk = String(data)
    if (root._rx.length + chunk.length > root.maxRxBytes) {
      root._giveUp("The Bose daemon sent an oversized reply")
      return
    }
    root._rx += chunk

    var cut
    while ((cut = root._rx.indexOf("\n")) !== -1) {
      var line = root._rx.substring(0, cut)
      root._rx = root._rx.substring(cut + 1)
      root._onLine(line)
    }
  }

  // Every line is one JSON object. A line carrying "schema" is a state push
  // (the subscribe handshake answer, a pushed change, or the reply to
  // `status`); anything else is the {"ok":...} acknowledgement of one command.
  function _onLine(data) {
    var line = String(data).trim()
    if (!line) return

    if (line.charAt(0) === "{" && line.indexOf("\"schema\":") !== -1) {
      // State lines are not acknowledged, so only the handshake timer is
      // cleared.
      handshake.stop()
      root._applyStatus(line)
      return
    }

    var ack = null
    try {
      ack = JSON.parse(line)
    } catch (_e) {
      ack = null
    }
    if (ack && ack.ok === false) {
      root.lastError = Model.elideError(ack.error || "command failed")
    }
    // Anything else is the {"ok":true} that acknowledges one command.
    if (root._outstanding > 0) root._outstanding--
    if (root._outstanding > 0) deadline.restart()
    else deadline.stop()
  }

  function _applyStatus(text) {
    var parsed = Model.parseStatus(text)
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
  // Command dispatch
  // ---------------------------------------------------------------------------
  // `expectsAck` is false for `status`, whose reply is a state line rather
  // than an acknowledgement.
  function runCommand(args, expectsAck) {
    if (!root.linked) {
      lastError = "The Bose daemon is not running"
      _connect()
      return false
    }
    if (_outstanding >= maxOutstanding) {
      lastError = "The Bose daemon is not keeping up"
      return false
    }
    var line = args.join(" ")
    if (line.length > maxCommandBytes || line.indexOf("\n") !== -1) {
      lastError = "Refusing to send a malformed command"
      return false
    }
    if (!_write(line + "\n")) {
      lastError = "The Bose daemon is not running"
      return false
    }
    if (expectsAck !== false) {
      _outstanding++
      deadline.restart()
    }
    return true
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
    if (root.linked) runCommand(["status"], false)
    else _connect()
  }

  Component.onCompleted: _connect()
}
