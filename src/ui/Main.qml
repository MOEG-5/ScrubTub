// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import itub.media

ApplicationWindow {
    id: window
    width: 1280
    height: 820
    visible: true
    title: qsTr("itub — Video Catalogue")
    color: "#141418"

    function statusText() {
        const m = catalogueModel
        if (m.scanState === "idle" || m.scanState === "complete")
            return qsTr("%1 videos").arg(m.count)
        return qsTr("%1 — discovered %2 · probed %3 · errors %4")
            .arg(m.scanState).arg(m.discovered).arg(m.probed).arg(m.errors)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            Layout.fillWidth: true
            RowLayout {
                anchors.fill: parent
                spacing: 8

                Button {
                    text: qsTr("Add folder")
                    onClicked: folderDialog.open()
                }
                Button {
                    text: qsTr("Pause")
                    onClicked: catalogue.pauseScanning()
                }
                Button {
                    text: qsTr("Resume")
                    onClicked: catalogue.resumeScanning()
                }
                Button {
                    text: qsTr("Cancel scan")
                    onClicked: catalogue.cancelScanning()
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: window.statusText()
                    color: "#c9c9d4"
                }
                CheckBox {
                    text: qsTr("Cached previews only")
                    checked: !hoverSession.enabled
                    onToggled: hoverSession.enabled = !checked
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("When checked, hovering uses only cached storyboard frames and never reads the original file.")
                }
            }
        }

        // Roots strip
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: rootRow.implicitHeight + 12
            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
            RowLayout {
                id: rootRow
                x: 6
                spacing: 6
                Repeater {
                    model: rootModel
                    delegate: Rectangle {
                        radius: 10
                        color: "#26262e"
                        implicitHeight: 30
                        implicitWidth: rootLabel.implicitWidth + removeButton.implicitWidth + 28
                        Label {
                            id: rootLabel
                            anchors.verticalCenter: parent.verticalCenter
                            x: 10
                            text: rootName + (rootStatus !== "ok" ? " (" + rootStatus + ")" : "")
                            color: rootStatus === "ok" ? "#e9e9f0" : "#ffb36b"
                        }
                        Button {
                            id: removeButton
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            flat: true
                            text: "✕"
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            ToolTip.text: qsTr("Remove folder from catalogue (media files are not touched)")
                            onClicked: catalogue.removeRoot(rootId)
                        }
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Central virtual grid
            GridView {
                id: grid
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: catalogueModel
                reuseItems: true
                cacheBuffer: 600
                clip: true
                pixelAligned: true
                keyNavigationEnabled: true
                cellWidth: cardSize
                cellHeight: Math.round(cardSize * 0.5625) + 66
                property int cardSize: 240

                boundsBehavior: Flickable.StopAtBounds

                delegate: VideoCard {
                    width: grid.cellWidth
                    height: grid.cellHeight
                }

                // Keyboard activation (§1): Enter opens the selected video
                // in the system default player; arrows navigate the grid.
                Keys.onReturnPressed: {
                    if (currentItem)
                        catalogue.openInDefaultPlayer(currentItem.videoId)
                }
                Keys.onEnterPressed: {
                    if (currentItem)
                        catalogue.openInDefaultPlayer(currentItem.videoId)
                }

                ScrollBar.vertical: ScrollBar { }

                Label {
                    anchors.centerIn: parent
                    visible: catalogueModel.count === 0
                    text: qsTr("No videos yet.\nAdd a folder to start discovery.")
                    horizontalAlignment: Text.AlignHCenter
                    color: "#8b8b96"
                }
            }

            // Details panel
            Rectangle {
                id: detailsPanel
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                color: "#1a1a20"
                visible: detailsVideoId >= 0

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    Label {
                        text: detailsName
                        wrapMode: Text.Wrap
                        color: "#f2f2f6"
                        font.pixelSize: 15
                        Layout.fillWidth: true
                    }
                    DetailRow { label: qsTr("Path"); value: detailsPath }
                    DetailRow { label: qsTr("Size"); value: detailsSizeBytes + " bytes" }
                    DetailRow { label: qsTr("Duration"); value: detailsDuration }
                    DetailRow { label: qsTr("Resolution"); value: detailsResolution }
                    DetailRow { label: qsTr("Codec"); value: detailsCodec }
                    DetailRow { label: qsTr("Views"); value: detailsViews }
                    DetailRow { label: qsTr("Status"); value: detailsAvailability + " / " + detailsProbeStatus }

                    Item { Layout.fillHeight: true }

                    Button {
                        Layout.fillWidth: true
                        text: qsTr("Open in default player")
                        onClicked: catalogue.openInDefaultPlayer(detailsVideoId)
                    }
                    Button {
                        Layout.fillWidth: true
                        text: qsTr("Precompute timeline preview")
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: qsTr("Generate the 24-frame cached storyboard for precise cached scrubbing.")
                        onClicked: catalogue.requestStoryboard(detailsVideoId)
                    }
                    // Keyboard-equivalent scrubbing (§6): a slider driving the
                    // same paused-player session as pointer hover.
                    Slider {
                        id: scrubSlider
                        Layout.fillWidth: true
                        from: 0
                        to: detailsDurationMs > 0 ? detailsDurationMs : 1
                        property bool engaged: false
                        onMoved: {
                            if (detailsVideoId > 0 && detailsDurationMs > 0) {
                                if (!engaged) {
                                    engaged = true
                                    hoverSession.engage(detailsVideoId, detailsRevision,
                                                        "", detailsDurationMs)
                                    // Path arrives via hoverSourceReady.
                                    catalogue.hoverEngage(detailsVideoId)
                                }
                                hoverSession.scrub(value)
                            }
                        }
                        onPressedChanged: if (!pressed) {
                            engaged = false
                            hoverSession.disengage()
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: qsTr("Clear rating")
                            onClicked: catalogue.setRating(detailsVideoId, 0)
                        }
                    }
                }
            }
        }

        Label {
            id: errorLabel
            Layout.fillWidth: true
            Layout.margins: 4
            color: "#ff8a80"
            font.pixelSize: 12
            text: ""
        }
    }

    FolderDialog {
        id: folderDialog
        onAccepted: catalogue.addRoot(selectedFolder)
    }

    // Selection data for the details panel
    property string detailsName: ""
    property string detailsPath: ""
    property string detailsSizeBytes: ""
    property string detailsDuration: ""
    property int detailsDurationMs: 0
    property int detailsRevision: 0
    property string detailsResolution: ""
    property string detailsCodec: ""
    property string detailsViews: ""
    property string detailsAvailability: ""
    property string detailsProbeStatus: ""
    property int detailsVideoId: -1

    function showDetails(videoId, name, path, sizeText, durationText, durationMs,
                         revision, resolution, codec, views, availability, probeStatus) {
        detailsVideoId = videoId
        detailsName = name
        detailsPath = path
        detailsSizeBytes = sizeText
        detailsDuration = durationText
        detailsDurationMs = durationMs
        detailsRevision = revision
        detailsResolution = resolution
        detailsCodec = codec
        detailsViews = views
        detailsAvailability = availability
        detailsProbeStatus = probeStatus
    }

    Connections {
        target: catalogue
        function onOperationFailed(message) { errorLabel.text = message }
        function onRootRejected(reason) { errorLabel.text = reason }
        function onRootAdded(root) {
            rootModel.append({ rootId: root.id, rootName: root.path, rootStatus: root.status })
        }
        function onRootRemoved(rootId) {
            for (let i = 0; i < rootModel.count; ++i) {
                if (rootModel.get(i).rootId === rootId) {
                    rootModel.remove(i)
                    return
                }
            }
        }
    }

    ListModel { id: rootModel }

    component DetailRow: RowLayout {
        property string label
        property string value
        Layout.fillWidth: true
        Label { text: parent.label; color: "#8b8b96"; Layout.preferredWidth: 90 }
        Label { text: parent.value; color: "#e9e9f0"; elide: Text.ElideMiddle; Layout.fillWidth: true }
    }
}
