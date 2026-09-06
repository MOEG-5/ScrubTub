// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    visible: true
    title: qsTr("itub — Video Catalogue (milestone 1)")
    color: "#141418"

    function statusText() {
        if (catalogueModel.scanState === "idle")
            return qsTr("Idle")
        return catalogueModel.scanState + qsTr(" — discovered %1 · probed %2 · errors %3")
            .arg(catalogueModel.discovered).arg(catalogueModel.probed)
            .arg(catalogueModel.errors)
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
                    text: qsTr("Cancel")
                    onClicked: catalogue.cancelScanning()
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: window.statusText()
                    color: "#c9c9d4"
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
                            onClicked: catalogue.removeRoot(rootId)
                        }
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: catalogueModel
            clip: true
            reuseItems: true

            delegate: Rectangle {
                width: list.width
                height: 44
                color: index % 2 === 0 ? "#1a1a20" : "#1e1e26"

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 10
                    Label {
                        Layout.preferredWidth: 340
                        text: model.name
                        elide: Text.ElideRight
                        color: "#f2f2f6"
                    }
                    Label {
                        Layout.preferredWidth: 200
                        text: model.path
                        elide: Text.ElideMiddle
                        color: "#8b8b96"
                        font.pixelSize: 12
                    }
                    Label {
                        Layout.preferredWidth: 80
                        text: model.sizeText
                        color: "#c9c9d4"
                    }
                    Label {
                        Layout.preferredWidth: 70
                        text: model.durationText
                        color: "#c9c9d4"
                    }
                    Label {
                        Layout.preferredWidth: 90
                        text: model.resolution
                        color: "#c9c9d4"
                    }
                    Label {
                        Layout.preferredWidth: 60
                        text: model.codec
                        color: "#c9c9d4"
                    }
                    Label {
                        Layout.preferredWidth: 40
                        text: model.views
                        color: "#c9c9d4"
                    }
                    Button {
                        text: "−"
                        flat: true
                        onClicked: catalogue.setRating(model.videoId,
                                                       Math.max(0, model.rating - 1))
                    }
                    Label {
                        Layout.preferredWidth: 52
                        text: model.rating === 0 ? qsTr("unrated")
                                                 : "★".repeat(model.rating)
                        color: "#ffd166"
                    }
                    Button {
                        text: "+"
                        flat: true
                        onClicked: catalogue.setRating(model.videoId,
                                                       Math.min(5, model.rating + 1))
                    }
                    Label {
                        Layout.fillWidth: true
                        text: model.availability + "/" + model.probeStatus
                        color: model.probeStatus === "ok" ? "#7fd18b"
                             : model.probeStatus === "error" ? "#ff8a80"
                             : model.probeStatus === "timeout" ? "#ffb36b"
                             : "#9a9aa6"
                        font.pixelSize: 12
                    }
                }
            }

            ScrollBar.vertical: ScrollBar { }

            Label {
                anchors.centerIn: parent
                visible: catalogueModel.count === 0
                text: qsTr("No videos yet — add a folder to start discovery.")
                color: "#8b8b96"
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

    Connections {
        target: catalogue
        function onOperationFailed(message) { errorLabel.text = message }
        function onRootRejected(reason) { errorLabel.text = reason }
    }

    ListModel {
        id: rootModel
    }

    Connections {
        target: catalogue
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
}
