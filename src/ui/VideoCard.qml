// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Grid card: poster, metadata, rating stars, and the hover timeline
// (TECH_SPEC.md sections 1, 6, 10). Hover behavior: 120 ms dwell engages the
// shared paused-player session; pointer position maps to a requested time;
// cached storyboard tiles provide instant feedback in parallel; leaving
// returns to the poster and unloads the source.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import itub.media

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

    readonly property bool hoverActive: hoverArea.containsMouse
    property var sampleTimes: []
    property bool atlasReady: false
    property bool liveFrameVisible: false
    property int hoverTileIndex: 0

    // Poster/atlas URLs come directly from the model (immutable per revision).

    function formatTime(ms) {
        const s = Math.floor(ms / 1000)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }

    onHoverActiveChanged: {
        if (hoverActive) {
            dwellTimer.restart()
            catalogue.requestSampleTimes(videoId)
            // Lazy storyboard generation for cached scrubbing (§6).
            catalogue.requestStoryboard(videoId)
        } else {
            dwellTimer.stop()
            hoverSession.disengage()
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
                liveFrameVisible = hoverSession.enabled
            }
        }
    }

    Rectangle {
        id: cardBody
        anchors.fill: parent
        anchors.margins: 6
        radius: 6
        color: "#1e1e26"
        border.color: grid.currentIndex === index ? "#7fb2ff" : (availability === "missing" ? "#5a3a3a" : "transparent")
        border.width: grid.currentIndex === index ? 2 : 1

        // Poster / live-frame content area (16:9-ish)
        Rectangle {
            id: contentArea
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 8
            height: parent.height - 62
            radius: 4
            color: "#141418"
            clip: true

            // Cached poster fallback; never a misleading timeline by itself.
            Image {
                id: posterImage
                anchors.fill: parent
                sourceSize.width: contentArea.width
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

            // Status label for the precise session.
            Label {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 4
                visible: card.hoverActive && hoverSession.status !== "idle"
                text: hoverSession.status
                color: hoverSession.status === "imprecise" ? "#ffb36b"
                     : hoverSession.status === "unavailable" ? "#ff8a80" : "#7fd18b"
                font.pixelSize: 10
            }

            // Cached timeline strip: nearest storyboard tile; instant feedback
            // with zero source reads in cached-only mode (§6).
            Item {
                id: timelineStrip
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: card.hoverActive && card.atlasReady
                        && card.sampleTimes.length > 0 && durationMs > 0 ? 40 : 0
                clip: true

                Image {
                    id: atlasImage
                    source: card.hoverActive ? card.atlasSource : ""
                    sourceSize.width: 0
                    asynchronous: true
                    visible: false
                    onStatusChanged: if (status === Image.Ready) card.atlasReady = true
                }

                Image {
                    anchors.fill: parent
                    source: atlasImage.source
                    fillMode: Image.PreserveAspectFit
                    // Slice the atlas to the nearest cached sample (§6).
                    property int tileW: 320
                    property int tileH: displayWidth > 0
                        ? Math.round(320 * displayHeight / displayWidth) : 180
                    property int columns: 6
                    sourceClipRect: Qt.rect(
                        (card.hoverTileIndex % columns) * tileW,
                        Math.floor(card.hoverTileIndex / columns) * tileH,
                        tileW, tileH)
                    visible: card.hoverActive && card.atlasReady
                             && !card.liveFrameVisible
                }

                Label {
                    anchors.bottom: parent.bottom
                    anchors.right: parent.right
                    anchors.margins: 2
                    text: {
                        if (card.liveFrameVisible && hoverSession.lastFramePtsMs >= 0)
                            return formatTime(hoverSession.lastFramePtsMs)
                        if (card.sampleTimes.length > 0)
                            return formatTime(card.sampleTimes[card.hoverTileIndex]) + " ≈"
                        return "≈"
                    }
                    color: "#ffffff"
                    style: Text.Outline
                    styleColor: "#000000"
                    font.pixelSize: 11
                }

            }
        }

        // Filename + summary
        Label {
            anchors.top: contentArea.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 8
            text: name
            elide: Text.ElideRight
            color: "#f2f2f6"
            font.pixelSize: 12
        }
        Label {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.margins: 8
            text: durationText + " · " + sizeText
            color: "#9a9aa6"
            font.pixelSize: 11
        }
        Label {
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.margins: 8
            text: views > 0 ? qsTr("%1 views").arg(views) : ""
            color: "#9a9aa6"
            font.pixelSize: 11
        }

    }

    // Pointer mapping (§6): u = clamp((x-left)/width, 0, 1), request u·D.
    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor
        onPositionChanged: (mouse) => {
            const w = contentArea.width
            if (w <= 0 || durationMs <= 0)
                return
            const u = Math.min(1, Math.max(0, (mouse.x - contentArea.x) / w))
            // Nearest cached sample tile index.
            const n = card.sampleTimes.length
            if (n > 0)
                card.hoverTileIndex = Math.min(n - 1, Math.floor(u * n))
            if (liveFrameVisible)
                hoverSession.scrub(Math.round(u * durationMs))
        }
        onClicked: (mouse) => {
            if (mouse.button === Qt.LeftButton) {
                grid.currentIndex = index
                window.showDetails(videoId, name, path, sizeBytes, durationText,
                                   durationMs, revision,
                                   displayWidth > 0 ? displayWidth + "×" + displayHeight : "?",
                                   codec || "?", views, availability, probeStatus,
                                   probeError)
            }
        }
        onDoubleClicked: catalogue.openInDefaultPlayer(videoId)
        onEntered: {
            // Instant cached feedback before the dwell elapses (§6).
            if (sampleTimes.length === 0)
                catalogue.requestSampleTimes(videoId)
        }
        onExited: {
            hoverSession.disengage()
            liveFrameVisible = false
        }
    }

    // Accessible star controls; a click must not trigger playback (§10).
    // Declared after the card's MouseArea so the buttons receive the clicks;
    // anchored inside the card with a chip background for visibility.
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        width: starRow.implicitWidth + 8
        height: 22
        radius: 11
        color: "#cc141418"
        visible: card.hoverActive
        Row {
            id: starRow
            anchors.centerIn: parent
            spacing: 0
        Repeater {
            model: 5
            AbstractButton {
                required property int modelData
                width: 20
                height: 20
                Accessible.name: qsTr("Rate %1 of 5 stars").arg(modelData + 1)
                contentItem: Text {
                    text: modelData < card.rating ? "★" : "☆"
                    color: "#ffd166"
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: catalogue.setRating(videoId, modelData + 1)
            }
        }
        }
    }

    Connections {
        target: catalogue
        function onSampleTimesReady(videoIdParam, times) {
            if (videoIdParam === card.videoId)
                card.sampleTimes = times
        }
        function onHoverSourceReady(videoIdParam, revisionParam, path, durationMsParam) {
            if (videoIdParam !== card.videoId)
                return
            // Engage the shared paused session here; empty path means the
            // file is not available and cached feedback carries on (§6).
            hoverSession.engage(videoIdParam, revisionParam, path, durationMsParam)
            liveFrameVisible = path !== "" && hoverSession.enabled
        }
        function onCacheEntryChanged(changedVideoId, profile) {
            if (changedVideoId === card.videoId && profile.startsWith("sb-"))
                catalogue.requestSampleTimes(card.videoId)
        }
    }
}
