// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Grid card: poster, metadata, rating stars, and the hover timeline
// (TECH_SPEC.md sections 1, 6, 10). Hover behavior: 120 ms dwell engages the
// shared paused-player session; pointer position maps to a requested time;
// cached storyboard tiles provide instant feedback in parallel; leaving
// returns to the poster and unloads the source.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import scrubtub.media

Item {
    id: card

    required property int videoId
    required property string name
    required property string path
    required property real sizeBytes
    required property string sizeText
    required property real durationMs
    required property string durationText
    required property int displayWidth
    required property int displayHeight
    required property string codec
    required property int rating
    required property real views
    required property string availability
    required property string probeStatus
    required property string probeError
    required property int revision
    required property string posterSource
    required property string atlasSource
    required property int index

    readonly property bool hoverActive: hoverArea.containsMouse && !grid.moving
    property var sampleTimes: []
    property bool atlasReady: false
    property bool liveFrameVisible: false
    property bool sessionEngaged: false
    property int hoverTileIndex: 0

    // Poster/atlas URLs come directly from the model (immutable per revision).

    function inspect() {
        if (videoId <= 0) return
        window.showDetails(videoId, name, path, sizeBytes, durationText,
                                   durationMs, revision,
                                   displayWidth > 0 ? displayWidth + "×" + displayHeight : "?",
                                   codec || "?", views, availability, probeStatus,
                                   probeError, posterSource, rating)
    }

    function formatTime(ms) {
        const s = Math.floor(ms / 1000)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }

    function pointerTarget() {
        if (contentArea.width <= 0 || durationMs <= 0)
            return 0
        const x = hoverArea.mapToItem(contentArea, hoverArea.mouseX, hoverArea.mouseY).x
        return Math.round(Math.min(1, Math.max(0, x / contentArea.width)) * durationMs)
    }

    function updateTarget() {
        const target = pointerTarget()
        let nearest = 0
        for (let i = 1; i < sampleTimes.length; ++i) {
            if (Math.abs(sampleTimes[i] - target) < Math.abs(sampleTimes[nearest] - target))
                nearest = i
        }
        hoverTileIndex = nearest
        if (sessionEngaged) {
            hoverSession.scrub(target)
        }
    }

    onHoverActiveChanged: {
        if (hoverActive) {
            dwellTimer.restart()
            if (window.cachedTimelineEnabled) {
                catalogue.requestSampleTimes(videoId)
            }
        } else {
            dwellTimer.stop()
            if (sessionEngaged)
                hoverSession.disengage()
            sessionEngaged = false
            liveFrameVisible = false
            sampleTimes = []
            atlasReady = false
        }
    }

    Timer {
        id: dwellTimer
        interval: 120 // §6 dwell before precise seeking engages
        onTriggered: {
            if (card.hoverActive) {
                catalogue.hoverEngage(card.videoId)
            }
        }
    }

    Rectangle {
        id: cardBody
        anchors.fill: parent
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        radius: 0
        color: "transparent"
        border.color: "transparent"
        border.width: 0

        // Poster / live-frame content area (16:9-ish)
        Rectangle {
            id: contentArea
            objectName: "thumbnailArea"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 0
            height: parent.height - 62
            radius: 6
            color: "#10120f"
            clip: true

            // Cached poster fallback; never a misleading timeline by itself.
            Image {
                id: posterImage
                anchors.fill: parent
                sourceSize.width: 320 // posters are generated at this size; resizing needs no reload
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                visible: !card.liveFrameVisible
                source: card.posterSource

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    visible: posterImage.status === Image.Null || posterImage.status === Image.Error
                    Label {
                        anchors.centerIn: parent
                        text: availability === "missing" ? qsTr("missing")
                             : probeStatus === "error" ? qsTr("error")
                             : probeStatus === "timeout" ? qsTr("timeout")
                             : qsTr("unprobed")
                        color: "#8b8b96"
                        font.pixelSize: 12
                    }
                }
            }

            // Live paused-player frame (one shared session; §6).
            Loader {
                anchors.fill: parent
                active: card.hoverActive && card.liveFrameVisible
                sourceComponent: HoverFrameItem {
                    frame: hoverSession.lastFrame
                }
            }

            Rectangle {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 8
                width: unavailableLabel.implicitWidth + 12
                height: 24
                radius: 4
                color: "#dc191b19"
                visible: card.hoverActive && hoverSession.enabled && hoverSession.status === "unavailable"
                Label { id: unavailableLabel; anchors.centerIn: parent; text: qsTr("Cached preview"); color: "#d3d4cc"; font.pixelSize: 11 }
            }

            // Cached timeline strip: nearest storyboard tile; instant feedback
            // with zero source reads in cached-only mode (§6).
            Item {
                id: timelineStrip
                anchors.fill: parent
                visible: card.hoverActive && durationMs > 0
                clip: true

                Image {
                    id: atlasImage
                    source: window.cachedTimelineEnabled && card.hoverActive ? card.atlasSource : ""
                    sourceSize.width: 0
                    asynchronous: true
                    visible: false
                    onStatusChanged: if (status === Image.Ready) card.atlasReady = true
                }

                // Clip the displayed atlas: sourceClipRect does not crop our
                // asynchronous image provider's returned texture.
                Item {
                    id: cachedFrame
                    anchors.centerIn: parent
                    property real tileW: atlasImage.implicitWidth / 5
                    property real tileH: atlasImage.implicitHeight
                        / Math.max(1, Math.ceil(card.sampleTimes.length / 5))
                    property real fit: Math.min(parent.width / Math.max(1, tileW),
                                                parent.height / Math.max(1, tileH))
                    width: tileW * fit
                    height: tileH * fit
                    clip: true
                    visible: card.hoverActive && card.atlasReady
                             && card.sampleTimes.length > 0 && !card.liveFrameVisible
                    Image {
                        source: atlasImage.source
                        asynchronous: true
                        width: atlasImage.implicitWidth * cachedFrame.fit
                        height: atlasImage.implicitHeight * cachedFrame.fit
                        x: -(card.hoverTileIndex % 5) * cachedFrame.width
                        y: -Math.floor(card.hoverTileIndex / 5) * cachedFrame.height
                    }
                }


            }
        }
        Rectangle {
            anchors.fill: contentArea
            color: "transparent"
            radius: 6
            border.width: 1
            border.color: grid.currentIndex === index ? "#ddbe8b" : card.hoverActive ? "#787c70" : "#31342e"
        }
        Rectangle {
            anchors.right: contentArea.right
            anchors.bottom: contentArea.bottom
            anchors.margins: 8
            width: timeLabel.implicitWidth + 12
            height: 24
            radius: 4
            color: "#e010120f"
            Label {
                id: timeLabel
                anchors.centerIn: parent
                text: card.hoverActive && card.liveFrameVisible && hoverSession.lastFramePtsMs >= 0
                    ? formatTime(hoverSession.lastFramePtsMs)
                    : card.hoverActive && card.sampleTimes.length > 0
                      ? formatTime(card.sampleTimes[card.hoverTileIndex]) + " ≈" : durationText
                color: "#eeeee9"
                font.pixelSize: 11
            }
        }
        Rectangle {
            anchors.left: contentArea.left
            anchors.bottom: contentArea.bottom
            width: card.hoverActive && card.durationMs > 0
                ? contentArea.width * Math.min(1, card.pointerTarget() / card.durationMs) : 0
            height: 3
            color: "#ddbe8b"
            visible: card.hoverActive
        }

        // Filename + summary
        Label {
            anchors.top: contentArea.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 10
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            text: name
            elide: Text.ElideRight
            color: "#eeeee9"
            font.pixelSize: 13
            font.weight: Font.Medium
        }
        Label {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.bottomMargin: 8
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            text: (displayHeight > 0 ? displayHeight + "p  ·  " : "") + sizeText
            color: "#a4a69e"
            font.pixelSize: 11
        }
        Label {
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.bottomMargin: 8
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            text: rating > 0 ? qsTr("%1 / 5").arg(rating) : ""
            color: "#a4a69e"
            font.pixelSize: 11
        }

    }

    // Pointer mapping (§6): u = clamp((x-left)/width, 0, 1), request u·D.
    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onPositionChanged: if (card.hoverActive) card.updateTarget()
        onClicked: (mouse) => {
            if (mouse.button === Qt.LeftButton || mouse.button === Qt.RightButton) {
                grid.currentIndex = index
                grid.forceActiveFocus()
                card.inspect()
                if (mouse.button === Qt.RightButton)
                    thumbnailMenu.popup()
            }
        }
        onDoubleClicked: (mouse) => {
            if (mouse.button === Qt.LeftButton) catalogue.openInDefaultPlayer(videoId)
        }

    }

    Menu {
        id: thumbnailMenu
        MenuItem { text: qsTr("Open file location"); onTriggered: catalogue.openFileLocation(card.videoId) }
        MenuItem { text: qsTr("Delete…"); onTriggered: { card.inspect(); window.confirmDelete() } }
    }

    ToolTip.visible: card.hoverActive && !sessionEngaged
    ToolTip.delay: 1200
    ToolTip.text: name

    Connections {
        target: hoverSession
        function onFrameReady(id, frameRevision, pts, frame) {
            if (card.hoverActive && card.sessionEngaged
                    && id === card.videoId && frameRevision === card.revision)
                card.liveFrameVisible = true
        }
        function onFrameChanged() {
            if (hoverSession.videoId !== card.videoId || hoverSession.lastFramePtsMs < 0)
                card.liveFrameVisible = false
        }
    }

    Connections {
        target: catalogue
        function onSampleTimesReady(videoIdParam, times) {
            if (window.cachedTimelineEnabled && videoIdParam === card.videoId && card.hoverActive) {
                card.sampleTimes = times
                card.updateTarget()
            }
        }
        function onHoverSourceReady(videoIdParam, revisionParam, path, durationMsParam) {
            if (videoIdParam !== card.videoId || !card.hoverActive)
                return
            // Engage the shared paused session here; empty path means the
            // file is not available and cached feedback carries on (§6).
            liveFrameVisible = false
            sessionEngaged = path !== "" && hoverSession.enabled
            hoverSession.engage(videoIdParam, revisionParam, path, durationMsParam,
                                card.pointerTarget())
        }
        function onCacheEntryChanged(changedVideoId, profile) {
            if (window.cachedTimelineEnabled && changedVideoId === card.videoId && profile.startsWith("sb-"))
                catalogue.requestSampleTimes(card.videoId)
        }
    }
}
