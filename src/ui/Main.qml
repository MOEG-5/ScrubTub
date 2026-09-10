// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import scrubtub.media

ApplicationWindow {
    id: window
    width: 1440
    height: 900
    visible: true
    title: qsTr("ScrubTub")
    minimumWidth: 720
    minimumHeight: 500
    font.pixelSize: 13
    color: "#191b19"
    palette.window: "#191b19"
    palette.windowText: "#eeeee9"
    palette.base: "#20221f"
    palette.alternateBase: "#202429"
    palette.text: "#eeeee9"
    palette.button: "#292b27"
    palette.buttonText: "#eeeee9"
    palette.highlight: "#ddbe8b"
    palette.highlightedText: "#191b19"
    palette.mid: "#343a41"
    palette.dark: "#101214"
    palette.light: "#3b434c"
    palette.placeholderText: "#9d9f97"

    Shortcut { sequences: [StandardKey.Quit]; onActivated: window.close() }
    Shortcut { sequence: "Ctrl+F"; onActivated: { sidebar.visible = true; searchField.forceActiveFocus() } }
    Shortcut { sequence: "Ctrl+B"; onActivated: sidebar.visible = !sidebar.visible }
    Shortcut { sequence: "Ctrl+,"; onActivated: settingsDialog.open() }
    Component.onCompleted: searchDebounce.restart()
    onActiveFocusItemChanged: {
        revealFocusedItem(filterScroll)
        revealFocusedItem(detailsScroll)
    }
    function revealFocusedItem(scrollView) {
        const focused = window.activeFocusItem
        if (!focused || !scrollView) return
        let ancestor = focused.parent
        while (ancestor && ancestor !== scrollView) ancestor = ancestor.parent
        if (!ancestor) return
        const flick = scrollView.contentItem
        const y = focused.mapToItem(flick, 0, 0).y
        const delta = y < 8 ? y - 8 : Math.max(0, y + focused.height + 8 - flick.height)
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + delta))
    }

    readonly property bool processing: catalogueModel.scanState === "enumerating"
        || catalogueModel.scanState === "probing" || catalogueModel.scanState === "previews"
    function statusText() {
        const m = catalogueModel
        if (m.scanState === "enumerating") return qsTr("Finding videos…")
        if (m.scanState === "paused") return qsTr("Paused · %1 left").arg(m.remaining + m.previewsRemaining)
        if (m.scanState === "cancelled") return qsTr("Stopped · %1 left").arg(m.remaining + m.previewsRemaining)
        if (m.remaining > 0) return qsTr("%1 videos left").arg(m.remaining)
        if (m.previewsRemaining > 0) return qsTr("Preparing previews · %1 left").arg(m.previewsRemaining)
        if (m.failed > 0) return qsTr("%1 need attention").arg(m.failed)
        if (m.scanState === "missing") return qsTr("Folder unavailable")
        if (window.processing) return qsTr("Finishing previews…")
        return ""
    }

    readonly property bool cachedTimelineEnabled: true

    property string libraryView: "all"
    property int selectedRoot: -1
    property string selectedRootName: ""
    readonly property string libraryTitle: selectedRoot >= 0 ? selectedRootName
        : libraryView === "rated" ? qsTr("Rated videos")
        : libraryView === "unrated" ? qsTr("Unrated videos")
        : libraryView === "unavailable" ? qsTr("Unavailable videos") : qsTr("All videos")

    function selectLibrary(view, rootId, rootName) {
        libraryView = view
        selectedRoot = rootId
        selectedRootName = rootName
        detailsVideoId = -1
        hoverSession.disengage()
        searchDebounce.restart()
    }

    component InputField: TextField {
        implicitHeight: 36
        leftPadding: 12
        rightPadding: 12
        color: "#eeeee9"
        placeholderTextColor: "#9d9f97"
        selectByMouse: true
        background: Rectangle {
            radius: 6
            color: "#20221f"
            border.color: parent.activeFocus ? "#ddbe8b" : "#373a34"
        }
    }
    component SelectBox: ComboBox {
        id: combo
        implicitHeight: 36
        leftPadding: 12
        rightPadding: 30
        background: Rectangle {
            radius: 6
            color: combo.hovered ? "#2e302b" : "#252724"
            border.width: combo.activeFocus ? 1 : 0
            border.color: "#ddbe8b"
        }
        indicator: Image {
            source: "icons/chevron.svg"
            width: 15; height: 15
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
        }
    }


    readonly property var libraryChoices: {
        let choices = [ {label: qsTr("All videos"), view: "all", root: -1},
                        {label: qsTr("Rated videos"), view: "rated", root: -1},
                        {label: qsTr("Unrated videos"), view: "unrated", root: -1},
                        {label: qsTr("Unavailable videos"), view: "unavailable", root: -1} ]
        for (let i = 0; i < rootModel.count; ++i) {
            const root = rootModel.get(i)
            choices.push({label: root.rootName.replace(/\\/g, "/").split("/").filter(Boolean).pop() || root.rootName,
                          view: "all", root: root.rootId})
        }
        return choices
    }
    property int detailsTab: 0
    property bool detailsExpanded: true

    SplitView {
        id: workspace
        anchors.fill: parent
        orientation: Qt.Horizontal
        handle: Rectangle {
            implicitWidth: 6
            color: SplitHandle.pressed ? "#ddbe8b" : SplitHandle.hovered ? "#787c70" : "#20221e"
        }
        Rectangle {
            id: sidebar
            objectName: "sidebar"
            SplitView.preferredWidth: 264
            SplitView.minimumWidth: 232
            SplitView.maximumWidth: Math.min(480, window.width - 420)
            color: "#141613"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    SelectBox {
                        id: libraryCombo
                        objectName: "librarySelector"
                        Layout.fillWidth: true
                        model: window.libraryChoices
                        textRole: "label"
                        font.pixelSize: 21
                        font.weight: Font.DemiBold
                        leftPadding: 0
                        background: Rectangle { color: "transparent"; radius: 6; border.width: libraryCombo.activeFocus ? 1 : 0; border.color: "#ddbe8b" }
                        currentIndex: Math.max(0, window.libraryChoices.findIndex(c => c.root === window.selectedRoot && c.view === window.libraryView))
                        Accessible.name: qsTr("Library or folder")
                        onActivated: {
                            const choice = window.libraryChoices[index]
                            window.selectLibrary(choice.view, choice.root, choice.label)
                        }
                    }
                    ActionButton { objectName: "hideSidebar"; iconName: "sidebar"; flat: true; Accessible.name: qsTr("Hide sidebar (Ctrl+B)"); onClicked: sidebar.visible = false }
                }
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        RowLayout {
                            Layout.fillWidth: true
                            visible: statusLabel.text.length > 0
                            spacing: 6
                            Image {
                                source: "icons/refresh.svg"
                                Layout.preferredWidth: 14
                                Layout.preferredHeight: 14
                                visible: window.processing
                                opacity: 0.75
                                RotationAnimator on rotation {
                                    from: 0; to: 360; duration: 1400
                                    loops: Animation.Infinite
                                    running: window.processing && window.visible
                                }
                                Accessible.ignored: true
                            }
                            Label {
                                id: statusLabel
                                objectName: "processingStatus"
                                text: window.statusText()
                                color: "#a4a69e"; font.pixelSize: 12
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }
                        }
                        Label {
                            objectName: "processedCount"
                            text: qsTr("%1 videos ready").arg(catalogueModel.processed)
                            color: "#a4a69e"; font.pixelSize: 12
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                    ActionButton {
                        objectName: "sortButton"
                        iconName: "sort"
                        flat: true
                        Accessible.name: qsTr("Sort videos")
                        onClicked: sortMenu.popup()
                        Menu {
                            id: sortMenu
                            MenuItem { text: qsTr("Sort by"); enabled: false }
                            Repeater {
                                model: [qsTr("Date added"), qsTr("Name"), qsTr("Size"), qsTr("Duration"), qsTr("Resolution"), qsTr("Rating"), qsTr("Views"), qsTr("Modified"), qsTr("Last opened")]
                                MenuItem {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    checkable: true
                                    checked: sortState.sortIndex === index
                                    onTriggered: { sortState.sortIndex = index; sortState.userSort = true; searchDebounce.restart(); saveSettings() }
                                }
                            }
                            MenuSeparator { }
                            MenuItem {
                                text: qsTr("Descending")
                                checkable: true
                                checked: sortState.sortDescending
                                onTriggered: { sortState.sortDescending = !sortState.sortDescending; sortState.userSort = true; searchDebounce.restart(); saveSettings() }
                            }
                        }
                    }
                }
                ScrollView {
                    id: filterScroll
                    objectName: "filterScroll"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ColumnLayout {
                        width: filterScroll.availableWidth
                        spacing: 16
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label { text: qsTr("Fuzzy search"); color: "#eeeee9" }
                            InputField {
                                id: searchField
                                objectName: "catalogueSearch"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Name, folder or tag")
                                Accessible.name: qsTr("Fuzzy search catalogue")
                                onTextChanged: searchDebounce.restart()
                            }
                            Label { text: qsTr("Tolerates typos · Ctrl F"); color: "#a4a69e"; font.pixelSize: 11 }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label { text: qsTr("Tags"); color: "#eeeee9" }
                            InputField {
                                id: includeTagsField
                                objectName: "includeTagsFilter"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Include tags")
                                Accessible.name: qsTr("Include all tags, separated by commas")
                                onTextChanged: searchDebounce.restart()
                            }
                            InputField {
                                id: excludeTagsField
                                objectName: "excludeTagsFilter"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Exclude tags")
                                Accessible.name: qsTr("Exclude tags, separated by commas")
                                onTextChanged: searchDebounce.restart()
                            }
                            Label { text: qsTr("Separate tags with commas"); color: "#a4a69e"; font.pixelSize: 11 }
                        }
                        RangeFilter {
                            id: durationRange
                            objectName: "durationFilter"
                            Layout.fillWidth: true
                            title: qsTr("Duration (seconds)")
                            scaleMaximum: 120
                            onEdited: searchDebounce.restart()
                        }
                        RangeFilter {
                            id: sizeRange
                            objectName: "sizeFilter"
                            Layout.fillWidth: true
                            title: qsTr("File size (MiB)")
                            scaleMaximum: 1024
                            onEdited: searchDebounce.restart()
                        }
                        RangeFilter {
                            id: ratingRange
                            objectName: "ratingFilter"
                            Layout.fillWidth: true
                            title: qsTr("Rating")
                            suffix: qsTr("0 includes unrated videos")
                            scaleMaximum: 5
                            unlimited: false
                            integerOnly: true
                            onEdited: searchDebounce.restart()
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label { text: qsTr("Resolution"); color: "#a4a69e" }
                            SelectBox {
                                id: resolutionCombo
                                Layout.fillWidth: true
                                Accessible.name: qsTr("Resolution")
                                model: [qsTr("Any resolution"), "480", "720", "1080", "1440", "2160"]
                                onActivated: searchDebounce.restart()
                            }
                            InputField {
                                id: viewsMinField
                                Layout.fillWidth: true
                                placeholderText: qsTr("Minimum views")
                                Accessible.name: qsTr("Minimum views")
                                validator: IntValidator { bottom: 0 }
                                onTextEdited: searchDebounce.restart()
                            }
                        }
                    }
                }
                ActionButton { objectName: "resetFilters"; text: qsTr("Reset search and filters"); iconName: "refresh"; flat: true; Layout.fillWidth: true; onClicked: filtersReset += 1 }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2d3029" }
                RowLayout {
                    Layout.fillWidth: true
                    ActionButton { text: qsTr("Add folder"); iconName: "plus"; Layout.fillWidth: true; onClicked: folderDialog.open() }
                    ActionButton { text: qsTr("Settings"); flat: true; onClicked: settingsDialog.open() }
                }
            }
        }
        ColumnLayout {
            SplitView.fillWidth: true
            spacing: 0
            GridView {
                id: grid
                objectName: "videoGrid"
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: catalogueModel
                reuseItems: true
                cacheBuffer: 600
                clip: true
                pixelAligned: true
                keyNavigationEnabled: true
                activeFocusOnTab: true
                onActiveFocusChanged: {
                    if (activeFocus) {
                        if (currentIndex < 0 && count > 0) currentIndex = 0
                        if (currentItem) currentItem.inspect()
                    }
                }
                onCurrentItemChanged: if (activeFocus && currentItem) currentItem.inspect()
                currentIndex: -1
                cellWidth: width / Math.max(width >= 420 ? 2 : 1, Math.floor(width / cardSize))
                cellHeight: Math.round(cellWidth * 0.625) + 70
                property int cardSize: 280

                boundsBehavior: Flickable.StopAtBounds
                maximumFlickVelocity: 6500
                flickDeceleration: 2200
                MouseArea {
                    parent: grid
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    // Unlike WheelHandler's blocking event point, MouseArea
                    // forwards rejected events. Leave touchpad gestures to Qt.
                    scrollGestureEnabled: false
                    onWheel: event => {
                        if (event.modifiers !== Qt.NoModifier) {
                            event.accepted = false
                            return
                        }
                        // A mouse wheel may report angles, pixels, or both.
                        // Feed every form into the same coasting motion.
                        const ticks = event.angleDelta.y !== 0 ? event.angleDelta.y / 120 : event.pixelDelta.y / 40
                        if (ticks === 0) { event.accepted = false; return }
                        // Scale travel, rather than velocity, with partial wheel
                        // ticks so high-resolution wheels don't fall below the
                        // native flick velocity threshold.
                        const impulse = Math.sign(ticks) * Math.sqrt(Math.abs(ticks)) * 900
                        const carry = grid.flicking && impulse * grid.verticalVelocity < 0 ? -grid.verticalVelocity : 0
                        grid.flick(0, Math.max(-grid.maximumFlickVelocity, Math.min(grid.maximumFlickVelocity, carry + impulse)))
                        event.accepted = true
                    }
                }
                Keys.onEscapePressed: { window.detailsVideoId = -1; currentIndex = -1; hoverSession.disengage() }

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

                ColumnLayout {
                    anchors.centerIn: parent
                    visible: catalogueModel.count === 0
                    spacing: 14
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: rootModel.count === 0 ? qsTr("Your archive starts here") : qsTr("No matching videos")
                        font.pixelSize: 26
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: rootModel.count === 0
                            ? qsTr("Add a folder to browse, scrub, and organise your collection.")
                            : qsTr("Try another search or adjust your filters.")
                        color: "#a4a69e"
                    }
                    ActionButton {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Add folder")
                        visible: rootModel.count === 0
                        onClicked: folderDialog.open()
                    }
                }
            }

            Rectangle { visible: window.detailsVideoId >= 0 && window.detailsExpanded; Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2d3029" }
            RowLayout {
                objectName: "detailsTabs"
                visible: window.detailsVideoId >= 0 && window.detailsExpanded
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.preferredHeight: 48
                spacing: 4
                Repeater {
                    model: [qsTr("Details"), qsTr("Tags"), qsTr("File info")]
                    ActionButton {
                        required property int index
                        required property string modelData
                        text: modelData
                        objectName: "detailsTab" + index
                        flat: true
                        enabled: window.detailsVideoId >= 0
                        selected: window.detailsExpanded && window.detailsVideoId >= 0 && window.detailsTab === index
                        onClicked: { window.detailsTab = index; window.detailsExpanded = true }
                    }
                }
                Item { Layout.fillWidth: true }
                ActionButton {
                    visible: window.detailsVideoId >= 0 && window.detailsExpanded
                    text: qsTr("Hide panel")
                    flat: true
                    onClicked: { window.detailsExpanded = false; hoverSession.disengage() }
                }
            }
            Rectangle {
                id: detailsPanel
                objectName: "detailsPanel"
                Layout.fillWidth: true
                Layout.preferredHeight: window.height < 760 ? 220 : 256
                color: "#20221e"
                visible: detailsVideoId >= 0 && window.detailsExpanded
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 24
                    ColumnLayout {
                        Layout.preferredWidth: window.width < 1100 ? 176 : 240
                        Layout.fillHeight: true
                        spacing: 4
                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Image {
                                anchors.fill: parent
                                source: window.detailsPosterSource
                                sourceSize.width: 320
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                            }
                            HoverFrameItem {
                                anchors.fill: parent
                                visible: scrubSlider.engaged && hoverSession.videoId === detailsVideoId && hoverSession.lastFramePtsMs >= 0
                                frame: hoverSession.lastFrame
                            }
                        }
                    Slider {
                        id: scrubSlider
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Video preview timeline")
                        focusPolicy: Qt.StrongFocus
                        stepSize: 1000
                        from: 0
                        to: detailsDurationMs > 0 ? detailsDurationMs : 1
                        property bool engaged: false
                        onMoved: {
                            if (detailsVideoId > 0 && detailsDurationMs > 0) {
                                if (!engaged || hoverSession.videoId !== detailsVideoId) {
                                    engaged = true
                                    catalogue.hoverEngage(detailsVideoId)
                                } else {
                                    hoverSession.scrub(value)
                                }
                            }
                        }
                        onActiveFocusChanged: if (!activeFocus && engaged) {
                            engaged = false
                            if (hoverSession.videoId === detailsVideoId)
                                hoverSession.disengage()
                        }
                        Connections {
                            target: catalogue
                            function onHoverSourceReady(id, revision, path, duration) {
                                if (scrubSlider.engaged && id === detailsVideoId)
                                    hoverSession.engage(id, revision, path, duration,
                                                        Math.round(scrubSlider.value))
                            }
                        }
                    }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 8
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: detailsName
                                elide: Text.ElideMiddle
                                color: "#eeeee9"
                                font.pixelSize: 15
                                Layout.fillWidth: true
                                ToolTip.visible: nameHover.hovered
                                ToolTip.text: text
                                HoverHandler { id: nameHover }
                            }
                            ActionButton { text: qsTr("Open video"); iconName: "play"; primary: true; onClicked: catalogue.openInDefaultPlayer(detailsVideoId) }
                        }
                        ScrollView {
                            id: detailsScroll
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            contentWidth: availableWidth
                            clip: true
                            ColumnLayout {
                                width: detailsScroll.availableWidth
                                spacing: 8
                                ColumnLayout {
                                    visible: window.detailsTab === 0
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label { text: [detailsDuration, detailsResolution, formatIec(Number(detailsSizeBytes))].join(" · "); color: "#a4a69e"; Layout.fillWidth: true; wrapMode: Text.Wrap }
                                    RowLayout {
                                        spacing: 4
                                        Label { text: qsTr("Rating"); color: "#a4a69e"; Layout.rightMargin: 12 }
                                        Repeater {
                                            model: 5
                                            ActionButton {
                                                required property int index
                                                implicitWidth: 30; implicitHeight: 32
                                                leftPadding: 6; rightPadding: 6
                                                iconName: "star"
                                                icon.color: index < window.detailsRating ? "#ddbe8b" : "#96998f"
                                                flat: true
                                                Accessible.name: qsTr("Rate %1 of 5 stars").arg(index + 1)
                                                onClicked: { catalogue.setRating(detailsVideoId, index + 1); window.detailsRating = index + 1 }
                                            }
                                        }
                                    }
                                    Label { text: qsTr("%1 views").arg(detailsViews); color: "#a4a69e" }
                    ActionButton {
                        text: qsTr("Video options…")
                        flat: true
                        onClicked: videoMenu.popup()
                        Menu {
                            id: videoMenu
                            MenuItem { text: qsTr("Regenerate automatic tags"); onTriggered: catalogue.regenerateAutoTags(detailsVideoId) }
                            MenuItem { text: qsTr("Reset suppressed tags"); onTriggered: catalogue.resetSuppressions() }
                            MenuItem { text: qsTr("Clear rating"); onTriggered: { catalogue.setRating(detailsVideoId, 0); window.detailsRating = 0 } }
                            MenuSeparator { }
                            MenuItem { text: qsTr("Move to Trash…"); onTriggered: window.confirmDelete() }
                        }
                    }

                                }
                                ColumnLayout {
                                    visible: window.detailsTab === 1
                                    Layout.fillWidth: true
                                    spacing: 8
                    Flow {
                        Layout.fillWidth: true
                        spacing: 4
                        Repeater {
                            model: detailsTags
                            delegate: ActionButton {
                                required property var modelData
                                objectName: "tagAction"
                                implicitHeight: 28
                                leftPadding: 8; rightPadding: 8
                                text: modelData.label + (modelData.suppressed ? qsTr(" (suppressed)") : "")
                                font.pixelSize: 11
                                palette.buttonText: modelData.suppressed ? "#a4a69e" : "#eeeee9"
                                Accessible.name: qsTr("Tag %1: open actions").arg(text)
                                onClicked: tagMenu.popup()
                                TapHandler { acceptedButtons: Qt.RightButton; onTapped: tagMenu.popup() }
                                Menu {
                                    id: tagMenu
                                    MenuItem {
                                        text: modelData.origin === "manual" ? qsTr("Remove tag") : qsTr("Suppress automatic tag")
                                        enabled: !modelData.suppressed
                                        onTriggered: {
                                            if (modelData.origin === "manual") catalogue.removeTag(detailsVideoId, modelData.tagId)
                                            else catalogue.suppressAutoTag(detailsVideoId, modelData.tagId)
                                        }
                                    }
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        InputField {
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

                                }
                                ColumnLayout {
                                    visible: window.detailsTab === 2
                                    Layout.fillWidth: true
                                    spacing: 6
                                    DetailRow { label: qsTr("Path"); value: detailsPath }
                                    GridLayout {
                                        columns: width > 650 ? 3 : 2
                                        Layout.fillWidth: true
                                        columnSpacing: 24
                                        rowSpacing: 6
                                        DetailRow { label: qsTr("Size"); value: formatIec(Number(detailsSizeBytes)) }
                                        DetailRow { label: qsTr("Duration"); value: detailsDuration }
                                        DetailRow { label: qsTr("Display size"); value: detailsResolution }
                                        DetailRow { label: qsTr("Codec"); value: detailsCodec }
                                        DetailRow { label: qsTr("Coded size"); value: fileInfo.codedWidth > 0 ? fileInfo.codedWidth + "×" + fileInfo.codedHeight : "?" }
                                        DetailRow { label: qsTr("Rotation"); value: fileInfo.rotation !== undefined && fileInfo.rotation !== null ? fileInfo.rotation + "°" : "?" }
                                        DetailRow { label: qsTr("Modified"); value: formatDate(fileInfo.modified) }
                                        DetailRow { label: qsTr("Added"); value: formatDate(fileInfo.added) }
                                        DetailRow { label: qsTr("Last opened"); value: formatDate(fileInfo.lastOpened) }
                                        DetailRow { label: qsTr("Views"); value: detailsViews }
                                        DetailRow { label: qsTr("Availability"); value: detailsAvailability }
                                        DetailRow { label: qsTr("Probe"); value: detailsProbeStatus }
                                    }
                                    Label { text: detailsProbeError; visible: text !== ""; wrapMode: Text.Wrap; color: "#edaaa0"; Layout.fillWidth: true }
                                }
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 28
                Layout.rightMargin: 28
                Layout.preferredHeight: 36
                ActionButton { objectName: "showSidebar"; visible: !sidebar.visible; iconName: "sidebar"; text: qsTr("Show sidebar"); flat: true; onClicked: sidebar.visible = true }
                Label { id: searchStatus; text: searchError; color: "#edaaa0"; elide: Text.ElideRight; Layout.fillWidth: true }
                Label { text: qsTr("Hover to scrub · Double-click to open"); color: "#a4a69e"; font.pixelSize: 11 }
                Slider {
                    Layout.preferredWidth: 92
                    from: 220; to: 380; stepSize: 20
                    value: grid.cardSize
                    Accessible.name: qsTr("Thumbnail size")
                    onMoved: { grid.cardSize = value; saveSettings() }
                }
            }
            Label { id: errorLabel; visible: text !== ""; text: ""; color: "#edaaa0"; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.margins: 12 }
        }
    }

    Dialog {
        id: settingsDialog
        title: qsTr("Settings")
        padding: 24
        background: Rectangle { color: "#20221e"; radius: 10; border.color: "#4d5046" }
        anchors.centerIn: parent
        width: 560
        height: Math.min(window.height - 60, 680)
        modal: false
        standardButtons: Dialog.Close
        onOpened: catalogue.requestCacheUsage()
        ScrollView {
            anchors.fill: parent
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: 12
                Label { text: qsTr("Browsing"); font.pixelSize: 17; font.weight: Font.DemiBold }
                CheckBox {
                    text: qsTr("Cached previews only")
                    checked: !hoverSession.enabled
                    onToggled: { hoverSession.enabled = !checked; saveSettings() }
                }
                CheckBox {
                    id: startupRefreshCheck
                    text: qsTr("Check folders on startup")
                    checked: true
                    onToggled: saveSettings()
                }
                RowLayout {
                    Label { text: qsTr("Thumbnail size") }
                    Slider {
                        from: 200; to: 360; stepSize: 20
                        value: grid.cardSize
                        Accessible.name: qsTr("Thumbnail size")
                        onMoved: { grid.cardSize = value; saveSettings() }
                    }
                }
                Label { text: qsTr("Folders"); font.pixelSize: 17; font.weight: Font.DemiBold }
        // Roots strip
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(160, rootRow.implicitHeight + 12)
            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
            ColumnLayout {
                id: rootRow
                x: 6
                spacing: 6
                Repeater {
                    model: rootModel
                    delegate: Rectangle {
                        radius: 10
                        color: "#30332b"
                        implicitHeight: 30
                        implicitWidth: 488
                        Label {
                            id: rootLabel
                            anchors.verticalCenter: parent.verticalCenter
                            x: 10
                            width: 350
                            elide: Text.ElideMiddle
                            text: rootName + (rootStatus !== "ok" ? " (" + rootStatus + ")" : "")
                            color: rootStatus === "ok" ? "#eeeee9" : "#ffb36b"
                        }
                        ActionButton {
                            id: rescanButton
                            Accessible.name: qsTr("Rescan folder %1").arg(rootName)
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            anchors.rightMargin: 44
                            flat: true
                            iconName: "refresh"
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            ToolTip.text: qsTr("Force rescan: re-check every file and retry failed probes")
                            onClicked: catalogue.rescanRoot(rootId, true)
                        }
                        ActionButton {
                            id: removeButton
                            Accessible.name: qsTr("Remove folder %1 from catalogue").arg(rootName)
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            flat: true
                            iconName: "close"
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
                    ActionButton { text: qsTr("Add folder"); onClicked: folderDialog.open() }
                    ActionButton { text: qsTr("Pause scan"); onClicked: catalogue.pauseScanning() }
                    ActionButton { text: qsTr("Resume"); onClicked: catalogue.resumeScanning() }
                    ActionButton { text: qsTr("Cancel scan"); onClicked: catalogue.cancelScanning() }
                }
                Label { text: qsTr("Catalogue & storage"); font.pixelSize: 17; font.weight: Font.DemiBold }
                Label { text: qsTr("Preview cache: %1").arg(formatIec(cacheBytes)); color: "#a4a69e" }
                ActionButton { text: qsTr("Export catalogue backup…"); onClicked: exportDialog.open() }
                ActionButton { text: qsTr("Restore catalogue from backup…"); onClicked: importDialog.open() }
                ActionButton { text: qsTr("Clear generated previews…"); onClicked: clearDialog.open() }
                ActionButton { text: qsTr("About and licenses"); onClicked: aboutDialog.open() }
            }
        }
    }

    Dialog {
        id: aboutDialog
        title: qsTr("About ScrubTub")
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 520)
        padding: 24
        modal: true
        standardButtons: Dialog.Close
        background: Rectangle { color: "#20221e"; radius: 10; border.color: "#4d5046" }
        ColumnLayout {
            width: parent.width
            spacing: 12
            Label { text: qsTr("ScrubTub %1").arg(Qt.application.version); font.pixelSize: 20 }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Copyright © 2026 the ScrubTub authors. Licensed under GNU GPL version 3. You may use, modify and redistribute it under that license. Provided without warranty.")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Uses Qt under LGPL/GPL terms, FFmpeg, mpv and other open-source components. Full notices are included with this download. Corresponding source and build materials accompany each release.")
            }
            ActionButton {
                text: qsTr("Open licenses & notices")
                onClicked: Qt.openUrlExternally(licenseDirectory)
            }
        }
    }

    FolderDialog {
        id: folderDialog
        onAccepted: catalogue.addRoot(selectedFolder)
    }

    function formatIec(bytes) {
        if (bytes < 0)
            return "?"
        const mib = 1048576
        const gib = 1024 * mib
        if (bytes >= gib)
            return (bytes / gib).toFixed(2) + " GiB"
        return (bytes / mib).toFixed(1) + " MiB"
    }

    Timer {
        interval: 2000
        running: settingsDialog.visible
        repeat: true
        onTriggered: catalogue.requestCacheUsage()
    }
    property var fileInfo: ({})
    function formatDate(value) {
        return value > 0 ? new Date(value).toLocaleString(Qt.locale(), Locale.ShortFormat) : "—"
    }
    function confirmDelete() {
        trashDialog.videoId = detailsVideoId
        trashDialog.fileName = detailsName
        trashDialog.filePath = detailsPath
        trashDialog.fileSize = formatIec(Number(detailsSizeBytes))
        trashDialog.open()
    }
    property real cacheBytes: -1
    Connections {
        target: catalogue
        function onFileInfoReady(videoId, info) {
            if (videoId === detailsVideoId) {
                fileInfo = info
                if (info.path) detailsPath = info.path
            }
            if (trashDialog.visible && videoId === trashDialog.videoId && info.path)
                trashDialog.filePath = info.path
        }
        function onCacheUsageReady(bytes) { cacheBytes = bytes }
        function onBackupImported() {
            errorLabel.text = ""
            searchStatus.text = qsTr("Catalogue restored from backup")
        }
        function onTrashResult(videoIdParam, ok, reason) {
            if (!ok)
                errorLabel.text = reason
        }
        function onSettingsReady(settings) {
            grid.cardSize = settings.cardSize
            sortState.sortDescending = settings.sortDescending
            hoverSession.enabled = !settings.cachedOnly
            startupRefreshCheck.checked = settings.startupRefresh
            if (settings.sortKey !== "") {
                const keys = ["added","name","size","duration","resolution","rating","views","mtime","lastOpened"]
                const idx = keys.indexOf(settings.sortKey)
                if (idx >= 0)
                    sortState.sortIndex = idx
            }
        }
    }
    function saveSettings() {
        catalogue.saveUiSettings({
            cardSize: grid.cardSize,
            sortKey: ["added","name","size","duration","resolution","rating","views","mtime","lastOpened"][sortState.sortIndex],
            sortDescending: sortState.sortDescending,
            cachedOnly: !hoverSession.enabled,
            startupRefresh: startupRefreshCheck.checked
        })
    }

    // Explicit Trash: names the full paths, count, and size; requires
    // confirmation for that selection (§3).
    Dialog {
        id: trashDialog
        anchors.centerIn: parent
        property int videoId: -1
        property string fileName: ""
        property string filePath: ""
        property string fileSize: ""
        title: qsTr("Delete?")
        modal: true
        standardButtons: Dialog.Yes | Dialog.No
        width: 480
        ColumnLayout {
            width: parent.width
            Label {
                text: trashDialog.videoId < 0
                    ? qsTr("Select a video first.")
                    : qsTr("Move %1 (%2) to the system Trash?\nPath: %3")
                        .arg(trashDialog.fileName).arg(trashDialog.fileSize).arg(trashDialog.filePath)
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                text: qsTr("The file is moved to the platform Trash; annotations are kept. There is no permanent-delete fallback.")
                wrapMode: Text.Wrap
                color: "#ffb36b"
                font.pixelSize: 11
                Layout.fillWidth: true
            }
        }
        onAccepted: {
            if (trashDialog.videoId >= 0) {
                hoverSession.disengage()
                catalogue.trashVideos([trashDialog.videoId])
            }
        }
    }

    FileDialog {
        id: exportDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "db"
        nameFilters: [qsTr("Catalogue backups (*.db)")]
        onAccepted: catalogue.exportBackup(selectedFile)
    }
    FileDialog {
        id: importDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Catalogue backups (*.db *.sqlite *.sqlite3)"), qsTr("All files (*)")]
        onAccepted: catalogue.importBackup(selectedFile)
    }
    Dialog {
        id: clearDialog
        anchors.centerIn: parent
        width: Math.min(window.width - 48, 480)
        title: qsTr("Clear previews")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label {
            text: qsTr("Delete all generated thumbnails and preview frames? They will be rebuilt in the background. Ratings, tags, and view counts are kept.")
            wrapMode: Text.Wrap
        }
        onAccepted: catalogue.clearPreviews()
    }

    // Query state and debounce (§8: 50 ms).
    property string searchError: ""
    property int resultCount: -1
    property int filtersReset: 0
    QtObject {
        id: sortState
        property int sortIndex: 0
        property bool sortDescending: false
        property bool userSort: false
    }
    Timer {
        id: searchDebounce
        interval: 50
        onTriggered: window.runSearch()
    }
    onFiltersResetChanged: {
        searchField.clear()
        includeTagsField.clear()
        excludeTagsField.clear()
        resolutionCombo.currentIndex = 0
        durationRange.reset()
        sizeRange.reset()
        ratingRange.reset()
        viewsMinField.clear()
        searchDebounce.restart()
    }

    function querySpec() {
        if (!durationRange.valid || !sizeRange.valid || !ratingRange.valid) return null
        const spec = {
            text: searchField.text,
            includeAllTags: includeTagsField.text.split(",").map(t => t.trim()).filter(Boolean),
            excludeTags: excludeTagsField.text.split(",").map(t => t.trim()).filter(Boolean),
            sortKey: ["added","name","size","duration","resolution","rating","views","mtime","lastOpened"][sortState.sortIndex],
            sortDescending: sortState.sortDescending,
            rootId: selectedRoot,
            userSort: sortState.userSort
        }
        if (resolutionCombo.currentIndex > 0) spec.resolutionPreset = resolutionCombo.currentText
        if (sizeRange.minimum > 0) spec.sizeMin = Math.round(sizeRange.minimum * 1048576)
        if (sizeRange.maximum >= 0) spec.sizeMax = Math.round(sizeRange.maximum * 1048576)
        if (durationRange.minimum > 0) spec.durationMinMs = Math.round(durationRange.minimum * 1000)
        if (durationRange.maximum >= 0) spec.durationMaxMs = Math.round(durationRange.maximum * 1000)
        if (ratingRange.minimum > 0) spec.ratingMin = ratingRange.minimum
        if (ratingRange.maximum < 5) spec.ratingMax = ratingRange.maximum
        if (viewsMinField.text !== "") spec.viewsMin = parseInt(viewsMinField.text)
        if (libraryView === "rated") spec.ratingMode = 3
        if (libraryView === "unrated") spec.ratingMode = 1
        if (libraryView === "unavailable")
            spec.availability = ["missing", "unavailable"]
        return spec
    }
    function runSearch() {
        const spec = querySpec()
        if (!spec) {
            searchError = qsTr("Check the highlighted filter ranges.")
            return
        }
        catalogue.search(spec)
    }

    Connections {
        target: catalogueModel
        function onProgressChanged() {
            if (catalogueModel.scanState === "complete") searchDebounce.restart()
        }
    }
    Connections {
        target: catalogue
        function onFilterBoundsReady(rootId, durationMs, sizeBytes) {
            if (rootId !== window.selectedRoot) return
            durationRange.scaleMaximum = Math.max(1, Math.ceil(durationMs / 1000))
            sizeRange.scaleMaximum = Math.max(1, Math.ceil(sizeBytes / 1048576))
        }
        function onSearchCompleted(generation, orderedIds, validationError) {
            searchError = validationError
            resultCount = orderedIds.length
            catalogueModel.setOrder(orderedIds, true)
            if (orderedIds.indexOf(detailsVideoId) < 0) detailsVideoId = -1
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
        fileInfo = ({})
        catalogue.requestFileInfo(videoId)
        catalogue.requestTags(videoId)
    }

    // Selection data for the details panel
    property string detailsPosterSource: ""
    property int detailsRating: 0
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
    property string detailsProbeError: ""
    property int detailsVideoId: -1

    function showDetails(videoId, name, path, sizeText, durationText, durationMs,
                         revision, resolution, codec, views, availability, probeStatus,
                         probeError, posterSource, rating) {
        scrubSlider.engaged = false
        detailsExpanded = true
        detailsVideoId = videoId
        detailsPosterSource = posterSource || ""
        detailsRating = rating || 0
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
        detailsProbeError = probeError
        detailsTags = []
        fileInfo = ({})
        catalogue.requestFileInfo(videoId)
        catalogue.requestTags(videoId)
    }

    Connections {
        target: catalogue
        function onOperationFailed(message) { errorLabel.text = message }
        function onRootRejected(reason) { errorLabel.text = reason }
        function onRootAdded(root) {
            rootModel.append({ rootId: root.id, rootName: root.path, rootStatus: root.status })
            searchDebounce.restart()
        }
        function onRootRemoved(rootId) {
            detailsVideoId = -1
            hoverSession.disengage()
            searchDebounce.restart()
            for (let i = 0; i < rootModel.count; ++i) {
                if (rootModel.get(i).rootId === rootId) {
                    if (selectedRoot === rootId) window.selectLibrary("all", -1, "")
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
        Label { text: parent.label; color: "#a4a69e"; Layout.preferredWidth: 90 }
        Label {
            text: parent.value
            color: "#eeeee9"
            elide: Text.ElideMiddle
            Layout.fillWidth: true
            ToolTip.visible: valueHover.hovered && truncated
            ToolTip.text: text
            HoverHandler { id: valueHover }
        }
    }
}
