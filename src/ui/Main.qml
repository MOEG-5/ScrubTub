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
                TextField {
                    id: searchField
                    Layout.preferredWidth: 260
                    placeholderText: qsTr("Search filenames, paths, tags…")
                    selectByMouse: true
                    onTextChanged: searchDebounce.restart()
                    color: searchError !== "" ? "#ff8a80" : "#f2f2f6"
                }
                ComboBox {
                    id: sortCombo
                    model: [qsTr("Date added"), qsTr("Name"), qsTr("Size"),
                            qsTr("Duration"), qsTr("Resolution"), qsTr("Rating"),
                            qsTr("Views"), qsTr("Modified"), qsTr("Last opened")]
                    onActivated: {
                        sortState.userSort = true
                        searchDebounce.restart()
                    }
                }
                Button {
                    text: sortState.sortDescending ? "↓" : "↑"
                    flat: true
                    onClicked: {
                        sortState.sortDescending = !sortState.sortDescending
                        sortState.userSort = true
                        searchDebounce.restart()
                    }
                }
                Button {
                    text: qsTr("Filters")
                    flat: true
                    onClicked: filtersPanel.visible = !filtersPanel.visible
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

        // Filters panel (§1): categories AND-combined, inclusive bounds.
        Rectangle {
            id: filtersPanel
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? filterRow.implicitHeight + 16 : 0
            color: "#1c1c24"
            visible: false

            RowLayout {
                id: filterRow
                anchors.fill: parent
                anchors.margins: 8
                spacing: 10

                ColumnLayout {
                    spacing: 2
                    Label { text: qsTr("Resolution"); color: "#8b8b96"; font.pixelSize: 10 }
                    ComboBox {
                        id: resolutionCombo
                        Layout.preferredWidth: 104
                        font.pixelSize: 12
                        model: ["any", "480", "720", "1080", "1440", "2160"]
                        onActivated: searchDebounce.restart()
                    }
                }
                ColumnLayout {
                    spacing: 2
                    Label { text: qsTr("Rating"); color: "#8b8b96"; font.pixelSize: 10 }
                    ComboBox {
                        id: ratingCombo
                        Layout.preferredWidth: 104
                        font.pixelSize: 12
                        model: [qsTr("any"), qsTr("unrated"), qsTr("rated"), "★1", "★2", "★3", "★4", "★5"]
                        onActivated: searchDebounce.restart()
                    }
                }
                ColumnLayout {
                    spacing: 2
                    Label { text: qsTr("Availability"); color: "#8b8b96"; font.pixelSize: 10 }
                    ComboBox {
                        id: availabilityCombo
                        Layout.preferredWidth: 120
                        font.pixelSize: 12
                        model: ["any", "available", "unprobed", "missing", "unavailable"]
                        onActivated: searchDebounce.restart()
                    }
                }
                ColumnLayout {
                    spacing: 2
                    Label { text: qsTr("Min size MiB"); color: "#8b8b96"; font.pixelSize: 10 }
                    TextField { id: sizeMinField; Layout.preferredWidth: 90; font.pixelSize: 12; selectByMouse: true; onTextEdited: searchDebounce.restart() }
                }
                ColumnLayout {
                    spacing: 2
                    Label { text: qsTr("Max size MiB"); color: "#8b8b96"; font.pixelSize: 10 }
                    TextField { id: sizeMaxField; Layout.preferredWidth: 90; font.pixelSize: 12; selectByMouse: true; onTextEdited: searchDebounce.restart() }
                }
                ColumnLayout {
                    spacing: 2
                    Label { text: qsTr("Min s"); color: "#8b8b96"; font.pixelSize: 10 }
                    TextField { id: durationMinField; Layout.preferredWidth: 70; font.pixelSize: 12; selectByMouse: true; onTextEdited: searchDebounce.restart() }
                }
                ColumnLayout {
                    spacing: 2
                    Label { text: qsTr("Max s"); color: "#8b8b96"; font.pixelSize: 10 }
                    TextField { id: durationMaxField; Layout.preferredWidth: 70; font.pixelSize: 12; selectByMouse: true; onTextEdited: searchDebounce.restart() }
                }
                ColumnLayout {
                    spacing: 2
                    Label { text: qsTr("Min views"); color: "#8b8b96"; font.pixelSize: 10 }
                    TextField { id: viewsMinField; Layout.preferredWidth: 70; font.pixelSize: 12; selectByMouse: true; onTextEdited: searchDebounce.restart() }
                }
                Button {
                    Layout.alignment: Qt.AlignBottom
                    text: qsTr("Reset")
                    onClicked: filtersReset += 1
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

                    Label { text: qsTr("Tags"); color: "#8b8b96" }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 4
                        Repeater {
                            model: detailsTags
                            delegate: Rectangle {
                                required property var modelData
                                width: chipRow.implicitWidth + 12
                                height: 22
                                radius: 11
                                color: modelData.origin === "manual" ? "#2b3a55" : "#26262e"
                                border.color: modelData.suppressed ? "#5a3a3a" : "transparent"
                                Row {
                                    id: chipRow
                                    anchors.centerIn: parent
                                    spacing: 4
                                    Label {
                                        text: modelData.label
                                        color: modelData.suppressed ? "#777" : "#e9e9f0"
                                        font.pixelSize: 11
                                    }
                                    Label {
                                        text: modelData.origin === "manual" ? "M"
                                              : modelData.origin === "technical" ? "T" : "A"
                                        color: "#6f6f7a"
                                        font.pixelSize: 9
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.RightButton
                                    onClicked: {
                                        if (modelData.origin === "manual")
                                            catalogue.removeTag(detailsVideoId, modelData.tagId)
                                        else
                                            catalogue.suppressAutoTag(detailsVideoId, modelData.tagId)
                                    }
                                }
                                ToolTip.visible: chipHover.containsMouse
                                ToolTip.delay: 400
                                ToolTip.text: qsTr("Right-click: %1").arg(
                                    modelData.origin === "manual"
                                        ? qsTr("remove manual tag")
                                        : qsTr("suppress automatic tag"))
                                MouseArea {
                                    id: chipHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: tagField
                            Layout.fillWidth: true
                            placeholderText: qsTr("Add tag")
                            font.pixelSize: 12
                            onAccepted: {
                                if (text.trim() !== "" && detailsVideoId > 0) {
                                    catalogue.addManualTag(detailsVideoId, text.trim())
                                    text = ""
                                }
                            }
                        }
                    }
                    RowLayout {
                        Button {
                            flat: true
                            text: qsTr("Regenerate automatic tags")
                            onClicked: catalogue.regenerateAutoTags(detailsVideoId)
                        }
                        Button {
                            flat: true
                            text: qsTr("Reset suppressed")
                            onClicked: catalogue.resetSuppressions()
                        }
                    }

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

        // Search validation and result count (visible, never silent — §8).
        Label {
            id: searchStatus
            Layout.fillWidth: true
            Layout.margins: 4
            color: searchError !== "" ? "#ff8a80" : "#8b8b96"
            font.pixelSize: 12
            text: searchError !== "" ? searchError
                 : (resultCount >= 0 ? qsTr("%1 results").arg(resultCount) : "")
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

    // Query state and debounce (§8: 50 ms).
    property string searchError: ""
    property int resultCount: -1
    property int filtersReset: 0
    QtObject {
        id: sortState
        property bool sortDescending: false
        property bool userSort: false
    }
    Timer {
        id: searchDebounce
        interval: 50
        onTriggered: window.runSearch()
    }
    onFiltersResetChanged: {
        resolutionCombo.currentIndex = 0
        ratingCombo.currentIndex = 0
        availabilityCombo.currentIndex = 0
        sizeMinField.text = ""
        sizeMaxField.text = ""
        durationMinField.text = ""
        durationMaxField.text = ""
        viewsMinField.text = ""
        searchDebounce.restart()
    }

    function ratingModeFor(choice) {
        if (choice === qsTr("unrated")) return 1
        if (choice === qsTr("rated")) return 3
        if (choice.startsWith("★")) return 2
        return 0
    }
    function ratingValueFor(choice) {
        if (choice.startsWith("★")) return choice.length - 1
        return 0
    }
    function runSearch() {
        const spec = {
            text: searchField.text,
            sortKey: ["added","name","size","duration","resolution","rating","views","mtime","lastOpened"][sortCombo.currentIndex],
            sortDescending: sortState.sortDescending,
            userSort: sortState.userSort
        }
        const preset = resolutionCombo.currentText
        if (preset !== "any") spec.resolutionPreset = preset
        const rm = ratingModeFor(ratingCombo.currentText)
        if (rm === 1) spec.ratingMode = 1
        else if (rm === 2) { spec.ratingMode = 2; spec.ratingValue = ratingValueFor(ratingCombo.currentText) }
        else if (rm === 3) spec.ratingMode = 3
        if (availabilityCombo.currentText !== "any")
            spec.availability = [availabilityCombo.currentText]
        const minMiB = parseFloat(sizeMinField.text)
        if (!isNaN(minMiB)) spec.sizeMin = Math.round(minMiB * 1048576)
        const maxMiB = parseFloat(sizeMaxField.text)
        if (!isNaN(maxMiB)) spec.sizeMax = Math.round(maxMiB * 1048576)
        const minS = parseFloat(durationMinField.text)
        if (!isNaN(minS)) spec.durationMinMs = Math.round(minS * 1000)
        const maxS = parseFloat(durationMaxField.text)
        if (!isNaN(maxS)) spec.durationMaxMs = Math.round(maxS * 1000)
        const minViews = parseInt(viewsMinField.text)
        if (!isNaN(minViews)) spec.viewsMin = minViews
        catalogue.search(spec)
    }

    Connections {
        target: catalogue
        function onSearchCompleted(generation, orderedIds, validationError) {
            searchError = validationError
            resultCount = orderedIds.length
            catalogueModel.setOrder(orderedIds, true)
        }
    }

    // Details-panel tag state
    property var detailsTags: []
    Connections {
        target: catalogue
        function onTagsReady(videoIdParam, tags) {
            if (videoIdParam === detailsVideoId)
                detailsTags = tags
        }
        function onTagListChanged() {
            if (detailsVideoId > 0)
                catalogue.requestTags(detailsVideoId)
        }
    }
    function showDetailsWithTags(videoId) {
        detailsTags = []
        catalogue.requestTags(videoId)
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
        detailsTags = []
        catalogue.requestTags(videoId)
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
