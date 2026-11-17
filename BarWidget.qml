import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui

BarWidget {
  id: root
  moduleName: "tilesgrid"

  // Static map written by the tilesgrid Hyprland toggle.
  readonly property string mapPath:
    (Quickshell.env("XDG_STATE_HOME") || Quickshell.env("HOME") + "/.local/state")
    + "/tilesgrid/map.conf"
  readonly property string pluginDir:
    Quickshell.env("HOME") + "/.config/omarchy/plugins/tilesgrid"
  property bool installStarted: false

  // First enable runs the idempotent installer: build the native plugin,
  // install the workspace-manager shadow/toggle, and replace the stock
  // workspace widget. Subsequent shell starts exit immediately when current.
  Component.onCompleted: {
    if (root.installStarted) return
    root.installStarted = true
    Quickshell.execDetached([root.pluginDir + "/install.sh", "--ensure"])
  }

  property int pages: 3
  property int stride: 100
  property int fallback: 30
  property var entries: []

  readonly property var monitor: {
    var window = root.QsWindow.window
    var screen = window ? window.screen : null
    return screen ? Hyprland.monitorFor(screen) : Hyprland.focusedMonitor
  }
  readonly property string monitorName: root.monitor ? String(root.monitor.name || "") : ""
  readonly property string monitorDescription: root.monitor ? String(root.monitor.description || "") : ""
  readonly property int activeId:
    root.monitor && root.monitor.activeWorkspace ? root.monitor.activeWorkspace.id : -1

  readonly property int base: {
    for (var i = 0; i < entries.length; i++) {
      var match = String(entries[i].match || "")
      if (!match) continue
      if (root.monitorDescription.indexOf(match) !== -1 || root.monitorName.indexOf(match) !== -1)
        return entries[i].base
    }
    return root.fallback
  }

  readonly property var recovered: recoveredIds()
  readonly property int currentPage: {
    var page = mapPage(root.activeId)
    if (page >= 0) return page

    var index = root.recovered.indexOf(root.activeId)
    if (index >= 0) return root.pages + Math.floor(index / 9)

    return 0
  }
  readonly property bool recoveredPage: root.currentPage >= root.pages
  readonly property string pageLabel:
    root.recoveredPage
      ? "R" + (root.currentPage - root.pages + 1)
      : String(root.currentPage + 1)
  readonly property bool hasRecovered: root.recovered.length > 0

  function readMap(text) {
    var next = { pages: 3, stride: 100, fallback: 30, entries: [] }
    var lines = String(text || "").split("\n")
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i].trim()
      if (!line || line.charAt(0) === "#") continue

      var eq = line.indexOf("=")
      if (eq < 0) continue

      var key = line.slice(0, eq).trim()
      var value = line.slice(eq + 1).trim()
      if (key === "pages") next.pages = parseInt(value, 10) || 3
      else if (key === "stride") next.stride = parseInt(value, 10) || 100
      else if (key === "fallback") next.fallback = parseInt(value, 10) || 30
      else if (key === "monitor") {
        var bar = value.lastIndexOf("|")
        if (bar > 0) {
          next.entries.push({
            match: value.slice(0, bar).trim(),
            base: parseInt(value.slice(bar + 1), 10) || 0
          })
        }
      }
    }

    root.pages = next.pages
    root.stride = next.stride
    root.fallback = next.fallback
    root.entries = next.entries
  }

  function mapPage(id) {
    if (id <= 0 || root.base < 0) return -1
    var rel = id - root.base - 1
    if (rel < 0) return -1

    var page = Math.floor(rel / root.stride)
    if (page >= root.pages) return -1
    if ((rel % root.stride) >= 9) return -1
    return page
  }

  function workspaceById(id) {
    var values = Hyprland.workspaces.values
    for (var i = 0; i < values.length; i++) {
      if (values[i].id === id) return values[i]
    }
    return null
  }

  function occupied(id) {
    var values = Hyprland.toplevels.values
    for (var i = 0; i < values.length; i++) {
      var workspace = values[i].workspace
      if (workspace && workspace.id === id) return true
    }
    return false
  }

  function recoveredIds() {
    var ids = []
    var values = Hyprland.workspaces.values
    for (var i = 0; i < values.length; i++) {
      var workspace = values[i]
      if (!workspace || workspace.id <= 0) continue
      if (!workspace.monitor || workspace.monitor.name !== root.monitorName) continue
      if (root.mapPage(workspace.id) !== -1) continue
      if (!root.occupied(workspace.id)) continue
      ids.push(workspace.id)
    }
    ids.sort(function(left, right) { return left - right })
    return ids
  }

  function pageTiles() {
    var tiles = []
    var page = root.currentPage

    if (page < root.pages) {
      for (var i = 1; i <= 9; i++) {
        var id = root.base + page * root.stride + i
        tiles.push({
          id: id,
          label: String(i),
          occupied: root.occupied(id),
          active: id === root.activeId
        })
      }
      return tiles
    }

    var sub = page - root.pages
    var recovered = root.recovered
    for (var j = 0; j < 9; j++) {
      var index = sub * 9 + j
      var recoveredId = index < recovered.length ? recovered[index] : -1
      tiles.push({
        id: recoveredId,
        label: String(j + 1),
        occupied: recoveredId > 0,
        active: recoveredId === root.activeId
      })
    }
    return tiles
  }

  function dispatch(expression) {
    if (root.bar) root.bar.run("hyprctl eval " + Util.shellQuote(expression))
  }

  function focusTile(id) {
    if (id <= 0) return
    root.dispatch("hl.plugin.tilesgrid.focus_id(" + id + ", \"" + root.monitorName + "\")")
  }

  FileView {
    path: root.mapPath
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.readMap(text())
    onLoadFailed: root.readMap("")
  }

  readonly property real trailingGap: root.vertical ? 0 : Style.spaceReal(1.5)

  implicitWidth: row.implicitWidth + root.trailingGap
  implicitHeight: row.implicitHeight

  RowLayout {
    id: row
    anchors.fill: parent
    anchors.rightMargin: root.trailingGap
    spacing: Style.space(1)

    GridLayout {
      columns: root.vertical ? 1 : 9
      columnSpacing: root.vertical ? 0 : Style.space(0.5)
      rowSpacing: root.vertical ? Style.space(1) : 0

      Repeater {
        model: root.pageTiles()

        WidgetButton {
          required property var modelData

          bar: root.bar
          text: modelData.id > 0 ? modelData.label : ""
          active: modelData.active
          dimmed: !modelData.occupied && !modelData.active
          interactive: modelData.id > 0
          pressable: modelData.id > 0
          tooltipText: modelData.id > 0
            ? "tile " + modelData.label + " (workspace " + modelData.id + ")"
            : ""
          horizontalMargin: 6
          verticalPadding: 6
          fixedWidth: root.vertical ? root.barSize : Style.space(20)
          fixedHeight: root.barSize
          onPressed: function() { root.focusTile(modelData.id) }
        }
      }
    }

    WidgetButton {
      bar: root.bar
      text: root.recoveredPage ? root.pageLabel : root.pageLabel + "/" + root.pages
      active: root.recoveredPage
      interactive: false
      pressable: false
      horizontalMargin: 4
      verticalPadding: 6
      fixedHeight: root.barSize
      tooltipText: "tilesgrid page"
    }

    WidgetButton {
      bar: root.bar
      text: "R"
      visible: root.hasRecovered && !root.recoveredPage
      interactive: false
      pressable: false
      dimmed: true
      horizontalMargin: 4
      verticalPadding: 6
      fixedHeight: root.barSize
      tooltipText: "Recovered workspaces exist"
    }
  }
}
