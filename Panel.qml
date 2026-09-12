// plugin/Panel.qml
// Omarchy bar widget and control panel for the Bose Noise Cancelling Headphones 700.
import QtQuick
import QtQuick.Controls
import Quickshell.Io
import qs.Commons
import qs.Ui
import "Model.js" as Model

Panel {
  id: root
  moduleName: "io.github.nipsen.omarchybose700"
  ipcTarget: "io.github.nipsen.omarchybose700"
  manageIpc: false

  readonly property color foreground: bar ? bar.foreground : Color.foreground
  readonly property color urgent: bar ? bar.urgent : Color.urgent
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property color muted: Qt.darker(root.foreground, 1.4)

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  Service {
    id: bose
    settings: root.settings
  }

  // ---------------------------------------------------------------------------
  // Tabs and keyboard navigation
  //
  // Each focusable section is either a toggle, a slider adjusted with h/l, or a
  // row/grid of choices with a cursor index. `cursorIndex` holds those indices;
  // it is replaced, never mutated, so that bindings reading it update.
  // ---------------------------------------------------------------------------
  property string currentTab: "sound"
  property string focusSection: "tabs"
  property bool cursorActive: false
  property var cursorIndex: ({})

  readonly property var tabs: ["sound", "device"]

  // Choice sections: their values (in display order) and grid width.
  readonly property var choiceSections: ({
    tabs: { values: root.tabs, cols: 2 },
    sidetone: { values: Model.SIDETONE_LEVELS, cols: 4 }
  })

  readonly property var sectionOrder: currentTab === "sound"
    ? ["tabs", "cnc", "eqBass", "eqMid", "eqTreble"]
    : ["tabs", "sidetone", "voicePrompts", "multipoint", "reconnect"]

  function idx(name) { return cursorIndex[name] || 0 }

  function setIdx(name, value) {
    var next = Object.assign({}, cursorIndex)
    next[name] = value
    cursorIndex = next
  }

  // Where the cursor sits in a choice section, or -1 when it is elsewhere.
  function cursorIn(name) {
    return root.cursorActive && root.focusSection === name ? idx(name) : -1
  }

  function syncCursorToState() {
    var current = {
      tabs: currentTab, sidetone: bose.sidetone
    }
    var next = {}
    for (var name in choiceSections) {
      var i = choiceSections[name].values.indexOf(current[name])
      next[name] = i !== -1 ? i : 0
    }
    cursorIndex = next
  }

  function showTab(tab) {
    if (root.tabs.indexOf(tab) === -1) return
    currentTab = tab
    setIdx("tabs", root.tabs.indexOf(tab))
    if (panelFlick) panelFlick.contentY = 0
  }

  onOpenedChanged: {
    if (opened) {
      cursorActive = true
      focusSection = "tabs"
      syncCursorToState()
      if (panelFlick) panelFlick.contentY = 0
      bose.refresh()
      Qt.callLater(function() { keyCatcher.forceActiveFocus() })
    }
  }

  function moveSection(dy) {
    var i = sectionOrder.indexOf(focusSection)
    if (i === -1) i = 0
    var next = i
    while (true) {
      next += (dy > 0 ? 1 : -1)
      if (next < 0 || next >= sectionOrder.length) return
      focusSection = sectionOrder[next]
      return
    }
  }

  function moveCursor(dx, dy) {
    cursorActive = true
    var choice = choiceSections[focusSection]

    if (dy !== 0) {
      // Multi-row grids: j/k walks within the grid before leaving it.
      if (choice && choice.cols < choice.values.length) {
        var target = idx(focusSection) + dy * choice.cols
        if (target >= 0 && target < choice.values.length) {
          setIdx(focusSection, target)
          return
        }
      }
      moveSection(dy)
      return
    }

    if (focusSection === "tabs") {
      showTab(root.tabs[Math.max(0, Math.min(root.tabs.length - 1, idx("tabs") + dx))])
    } else if (choice) {
      setIdx(focusSection, Math.max(0, Math.min(choice.values.length - 1, idx(focusSection) + dx)))
    } else if (focusSection === "cnc") {
      bose.setCncLevel(Model.clamp(bose.cncLevel + dx, 0, bose.cncMax, 0))
    } else if (focusSection === "eqBass" || focusSection === "eqMid" || focusSection === "eqTreble") {
      var band = focusSection === "eqBass" ? "bass" : (focusSection === "eqMid" ? "mid" : "treble")
      var current = band === "bass" ? bose.eqBass : (band === "mid" ? bose.eqMid : bose.eqTreble)
      bose.setEqBand(band, Model.clamp(current + dx, bose.eqMin, bose.eqMax, 0))
    }
  }

  function activateCursor() {
    var choice = choiceSections[focusSection]
    if (choice) {
      choose(focusSection, choice.values[idx(focusSection)])
      return
    }
    if (focusSection === "voicePrompts") bose.setVoicePrompts(!bose.voicePrompts)
    else if (focusSection === "multipoint") bose.setMultipoint(!bose.multipoint)
    else if (focusSection === "reconnect") bose.reconnect()
  }

  // One place that turns a choice into a command, for clicks and keys alike.
  function choose(section, value) {
    focusSection = section
    var values = choiceSections[section].values
    setIdx(section, Math.max(0, values.indexOf(value)))
    if (section === "tabs") showTab(value)
    else if (section === "sidetone") bose.setSidetone(value)
  }

  IpcHandler {
    target: root.ipcTarget
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): string { bose.refresh(); return "ok" }
    function cycleCnc(): string { bose.cycleCnc(); return String(bose.cncLevel) }
    function showTab(tab: string): string { root.showTab(tab); return root.currentTab }
  }

  // ---------------------------------------------------------------------------
  // Reusable pieces
  // ---------------------------------------------------------------------------

  // A row or grid of mutually exclusive choices built from stock Buttons.
  // Inline components cannot see this file's ids, so everything they need is
  // passed in: `cursor` is the index to draw the keyboard cursor on (-1 for
  // none) and `chosen` reports clicks back out.
  component ChoiceGrid: Grid {
    id: grid
    property var options: []          // [{ value, label, icon? }]
    property string current: ""
    property int cursor: -1
    property bool active: true
    property color foreground: Color.foreground
    property string fontFamily: Style.font.family
    property real fontSize: Style.font.caption
    signal chosen(string value)

    spacing: Style.space(6)
    opacity: active ? 1.0 : 0.4
    readonly property real cellWidth: (width - spacing * (columns - 1)) / columns

    Repeater {
      model: grid.options

      Button {
        required property var modelData
        required property int index
        width: grid.cellWidth
        text: modelData.label
        iconText: modelData.icon || ""
        iconSize: Style.font.title
        fontSize: grid.fontSize
        foreground: grid.foreground
        fontFamily: grid.fontFamily
        bordered: true
        enabled: grid.active
        selected: grid.current === modelData.value
        hasCursor: grid.cursor === index
        horizontalPadding: Style.space(4)
        verticalPadding: Style.space(6)
        onClicked: grid.chosen(modelData.value)
      }
    }
  }

  // Header text on the left, a value on the right.
  component HeaderRow: Item {
    id: headerRow
    property string title: ""
    property string value: ""
    property color foreground: Color.foreground
    property string fontFamily: Style.font.family
    height: Math.max(headerTitle.implicitHeight, headerValue.implicitHeight)

    PanelSectionHeader {
      id: headerTitle
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      text: headerRow.title
      foreground: headerRow.foreground
      fontFamily: headerRow.fontFamily
    }

    Text {
      id: headerValue
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      textFormat: Text.PlainText
      text: headerRow.value
      color: headerRow.foreground
      font.family: headerRow.fontFamily
      font.pixelSize: Style.font.caption
      font.bold: true
    }
  }

  // One EQ band: label, slider, value. Sends only on release; the value label
  // follows the drag so there is feedback without a flood of commands.
  component BandSlider: Item {
    id: band
    property string label: ""
    property int value: 0
    property int minimum: -10
    property int maximum: 10
    property var barRef: null
    property color foreground: Color.foreground
    property string fontFamily: Style.font.family
    signal committed(int value)
    height: bandSlider.implicitHeight > 0 ? Math.max(bandSlider.implicitHeight, bandLabel.implicitHeight) : Style.space(24)

    Text {
      id: bandLabel
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      width: Style.space(64)
      textFormat: Text.PlainText
      text: band.label
      color: band.foreground
      font.family: band.fontFamily
      font.pixelSize: Style.font.caption
    }

    PanelSlider {
      id: bandSlider
      anchors.left: bandLabel.right
      anchors.right: bandValue.left
      anchors.rightMargin: Style.space(8)
      anchors.verticalCenter: parent.verticalCenter
      minimum: band.minimum
      maximum: band.maximum
      step: 1
      integer: true
      value: band.value
      bar: band.barRef
      onReleased: function(v) { band.committed(Math.round(v)) }
    }

    Text {
      id: bandValue
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      width: Style.space(28)
      horizontalAlignment: Text.AlignRight
      textFormat: Text.PlainText
      readonly property int shown: Math.round(bandSlider.dragging ? bandSlider.liveValue : band.value)
      text: (shown > 0 ? "+" : "") + shown
      color: band.foreground
      font.family: band.fontFamily
      font.pixelSize: Style.font.caption
      font.bold: true
    }
  }

  component Caption: Text {
    width: parent ? parent.width : 0
    textFormat: Text.PlainText
    wrapMode: Text.WordWrap
    font.pixelSize: Style.font.caption
  }

  // ---------------------------------------------------------------------------
  // Bar widget button
  // ---------------------------------------------------------------------------
  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    labelVisible: false
    hasVisualContent: true
    fixedWidth: vertical ? -1 : (contentRow.implicitWidth + scaledHorizontalMargin * 2)
    tooltipText: bose.connected
      ? ((bose.deviceName || "Bose NC 700") + " (" + Model.cncLevelName(bose.cncLevel, bose.cncMax) + ", " + Model.formatBattery(bose.batteryLevel) + ")")
      : "Bose NC 700 (Disconnected)"

    Row {
      id: contentRow
      anchors.centerIn: parent
      spacing: Style.space(6)

      BoseIcon {
        anchors.verticalCenter: parent.verticalCenter
        iconSize: Style.space(14)
        connected: bose.connected
        batteryLevel: bose.batteryLevel
        color: bose.connected
          ? (button.active ? button.activeColor : button.foreground)
          : Qt.rgba(button.foreground.r, button.foreground.g, button.foreground.b, 0.4)
      }

      Text {
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.PlainText
        text: bose.connected ? Model.formatBattery(bose.batteryLevel) : "—"
        color: button.active ? button.activeColor : button.foreground
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
        renderType: Text.NativeRendering
        visible: !button.vertical
      }
    }

    onPressed: function(buttonCode) {
      if (buttonCode === Qt.RightButton) bose.cycleCnc()
      else root.toggle()
    }
  }

  // ---------------------------------------------------------------------------
  // Dropdown panel
  // ---------------------------------------------------------------------------
  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(400))
    // fittedContentHeight clamps this to the content and to smaller screens.
    contentHeight: panel.fittedContentHeight(panelColumn.implicitHeight, Style.space(640))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onMoveRequested: function(dx, dy) { root.moveCursor(dx, dy) }
      onActivateRequested: function() { root.activateCursor() }

      Flickable {
        id: panelFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: panelColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar {
          policy: panelColumn.implicitHeight > panelFlick.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        }

        Column {
          id: panelColumn
          width: parent.width
          spacing: Style.space(12)

          // -------------------------------------------------------------------
          // Header. An Item rather than a Row: the battery block is pinned to
          // the right edge, and a Row refuses horizontal anchors on its
          // children and then lays out nothing at all.
          // -------------------------------------------------------------------
          Item {
            width: parent.width
            height: Math.max(headerIcon.height, headerText.implicitHeight, batteryBlock.implicitHeight)

            BoseIcon {
              id: headerIcon
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
              iconSize: Style.space(28)
              connected: bose.connected
              batteryLevel: bose.batteryLevel
            }

            Column {
              id: headerText
              anchors.left: headerIcon.right
              anchors.leftMargin: Style.space(12)
              anchors.right: batteryBlock.left
              anchors.rightMargin: Style.space(12)
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(2)

              Text {
                width: parent.width
                textFormat: Text.PlainText
                text: bose.deviceName || "Bose NC 700"
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.title
                font.bold: true
                elide: Text.ElideRight
              }

              Row {
                spacing: Style.space(6)

                Rectangle {
                  anchors.verticalCenter: parent.verticalCenter
                  width: Style.space(7)
                  height: Style.space(7)
                  radius: width / 2
                  color: bose.connected ? "#2ecc71" : Qt.darker(root.foreground, 1.8)
                }

                Text {
                  anchors.verticalCenter: parent.verticalCenter
                  textFormat: Text.PlainText
                  text: bose.connected ? "Connected" : "Disconnected"
                  color: root.muted
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.caption
                }
              }
            }

            Column {
              id: batteryBlock
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(2)
              visible: bose.connected

              Row {
                anchors.right: parent.right
                spacing: Style.space(4)

                Text {
                  anchors.verticalCenter: parent.verticalCenter
                  text: Model.batteryIcon(bose.batteryLevel)
                  color: root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.subtitle
                }

                Text {
                  anchors.verticalCenter: parent.verticalCenter
                  textFormat: Text.PlainText
                  text: Model.formatBattery(bose.batteryLevel)
                  color: root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.subtitle
                  font.bold: true
                }
              }
            }
          }

          ChoiceGrid {
            width: parent.width
            columns: 2
            options: [{ value: "sound", label: "Sound" }, { value: "device", label: "Device" }]
            current: root.currentTab
            cursor: root.cursorIn("tabs")
            foreground: root.foreground
            fontFamily: root.fontFamily
            fontSize: Style.font.bodySmall
            onChosen: function(value) { root.choose("tabs", value) }
          }

          PanelSeparator { foreground: root.foreground }

          // ===================================================================
          // SOUND
          // ===================================================================
          Column {
            width: parent.width
            spacing: Style.space(12)
            visible: root.currentTab === "sound"

            // --- Noise cancelling ---------------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(6)

              HeaderRow {
                width: parent.width
                title: "NOISE CANCELLING"
                value: String(Math.round(cncSlider.dragging ? cncSlider.liveValue : bose.cncLevel)) + " / " + bose.cncMax
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              PanelSlider {
                id: cncSlider
                width: parent.width
                minimum: 0
                maximum: bose.cncMax
                step: 1
                integer: true
                value: bose.cncLevel
                bar: root.bar
                onReleased: function(v) {
                  root.focusSection = "cnc"
                  bose.setCncLevel(Math.round(v))
                }
              }

              Caption {
                text: "0 is full transparency; " + bose.cncMax + " is maximum noise cancelling."
                color: root.muted
                font.family: root.fontFamily
              }
            }

            PanelSeparator { foreground: root.foreground }

            // --- Equalizer ------------------------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(4)

              PanelSectionHeader {
                text: "EQUALIZER"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              Repeater {
                model: Model.EQ_BANDS

                BandSlider {
                  required property string modelData
                  width: parent.width
                  label: Model.eqBandLabel(modelData)
                  value: modelData === "bass" ? bose.eqBass : (modelData === "mid" ? bose.eqMid : bose.eqTreble)
                  minimum: bose.eqMin
                  maximum: bose.eqMax
                  barRef: root.bar
                  foreground: root.foreground
                  fontFamily: root.fontFamily
                  onCommitted: function(v) {
                    root.focusSection = modelData === "bass" ? "eqBass" : (modelData === "mid" ? "eqMid" : "eqTreble")
                    bose.setEqBand(modelData, v)
                  }
                }
              }
            }
          }

          // ===================================================================
          // DEVICE
          // ===================================================================
          Column {
            width: parent.width
            spacing: Style.space(12)
            visible: root.currentTab === "device"

            // --- Sidetone ------------------------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(8)

              PanelSectionHeader {
                text: "SIDETONE"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              ChoiceGrid {
                width: parent.width
                columns: 4
                options: Model.SIDETONE_LEVELS.map(function(s) {
                  return { value: s, label: Model.sidetoneName(s) }
                })
                current: bose.sidetone
                cursor: root.cursorIn("sidetone")
                foreground: root.foreground
                fontFamily: root.fontFamily
                fontSize: Style.font.bodySmall
                onChosen: function(value) { root.choose("sidetone", value) }
              }

              Caption {
                text: "How much of your own voice you hear during calls."
                color: root.muted
                font.family: root.fontFamily
              }
            }

            PanelSeparator { foreground: root.foreground }

            // --- Prompts and multipoint ----------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(8)

              Toggle {
                width: parent.width
                label: "Voice prompts"
                description: Model.voicePromptsDescription(bose.voicePrompts, bose.voicePromptsLanguage)
                checked: bose.voicePrompts
                hasCursor: root.cursorActive && root.focusSection === "voicePrompts"
                foreground: root.foreground
                fontFamily: root.fontFamily
                onClicked: {
                  root.focusSection = "voicePrompts"
                  bose.setVoicePrompts(!bose.voicePrompts)
                }
              }

              Caption {
                visible: bose.voicePromptsLanguage.length > 0
                text: "Prompt language: " + bose.voicePromptsLanguage + " (read-only; change it in the Bose app)"
                color: root.muted
                font.family: root.fontFamily
              }

              Toggle {
                width: parent.width
                label: "Multipoint"
                description: "Stay connected to two devices at once"
                checked: bose.multipoint
                hasCursor: root.cursorActive && root.focusSection === "multipoint"
                foreground: root.foreground
                fontFamily: root.fontFamily
                onClicked: {
                  root.focusSection = "multipoint"
                  bose.setMultipoint(!bose.multipoint)
                }
              }
            }

            PanelSeparator { foreground: root.foreground }

            // --- Reconnect -------------------------------------------------------
            Item {
              width: parent.width
              height: Math.max(reconnectText.implicitHeight, reconnectButton.implicitHeight)

              Caption {
                id: reconnectText
                anchors.left: parent.left
                anchors.right: reconnectButton.left
                anchors.rightMargin: Style.space(12)
                anchors.verticalCenter: parent.verticalCenter
                width: undefined
                text: "Drop and re-open the link to the headphones."
                color: root.muted
                font.family: root.fontFamily
              }

              Button {
                id: reconnectButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: Style.space(96)
                text: "Reconnect"
                fontSize: Style.font.bodySmall
                foreground: root.foreground
                fontFamily: root.fontFamily
                bordered: true
                hasCursor: root.cursorActive && root.focusSection === "reconnect"
                onClicked: {
                  root.focusSection = "reconnect"
                  bose.reconnect()
                }
              }
            }

            Caption {
              text: (bose.deviceName || "Bose NC 700") + (bose.firmwareVersion ? " · firmware " + bose.firmwareVersion : "")
              horizontalAlignment: Text.AlignHCenter
              color: root.muted
              font.family: root.fontFamily
            }
          }
        }
      }
    }
  }
}
