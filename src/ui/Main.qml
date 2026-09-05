// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    visible: true
    title: qsTr("itub — Video Catalogue (milestone 0)")
    color: "#141418"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            Layout.fillWidth: true
            RowLayout {
                anchors.fill: parent
                spacing: 12
                Label {
                    text: qsTr("itub")
                    font.bold: true
                    font.pixelSize: 16
                }
                Label {
                    text: qsTr("%1 placeholder cards").arg(gridModel.count)
                    color: "#9a9aa6"
                }
                Item { Layout.fillWidth: true }
                Label { text: qsTr("Card size") }
                Slider {
                    id: sizeSlider
                    from: 120
                    to: 280
                    value: gridModel.cellSize
                    stepSize: 4
                    onValueChanged: gridModel.cellSize = value
                }
            }
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: gridModel
            reuseItems: true
            cacheBuffer: 400
            clip: true
            pixelAligned: true
            cellWidth: gridModel.cellSize
            cellHeight: Math.round(gridModel.cellSize * 0.5625) + 44

            delegate: Item {
                required property string name
                required property string duration
                required property string size
                required property string dimensions
                required property int hue
                required property int index

                width: grid.cellWidth
                height: grid.cellHeight

                Rectangle {
                    id: card
                    anchors.fill: parent
                    anchors.margins: 6
                    radius: 6
                    color: Qt.hsla(model.hue / 360.0, 0.45, 0.28, 1.0)
                    border.color: grid.currentIndex === index ? "#7fb2ff" : "transparent"
                    border.width: 2

                    Rectangle {
                        // Poster placeholder rectangle, 16:9 area
                        anchors {
                            top: parent.top
                            left: parent.left
                            right: parent.right
                            margins: 8
                        }
                        height: parent.height - 44
                        radius: 4
                        color: Qt.hsla(model.hue / 360.0, 0.5, 0.42, 1.0)

                        Label {
                            anchors.centerIn: parent
                            text: model.dimensions
                            color: "#e9e9f0"
                            font.pixelSize: 14
                        }
                    }

                    Label {
                        anchors {
                            bottom: parent.bottom
                            left: parent.left
                            right: parent.right
                            margins: 8
                        }
                        text: model.name
                        elide: Text.ElideRight
                        color: "#f2f2f6"
                        font.pixelSize: 13
                    }

                    Label {
                        anchors {
                            bottom: parent.bottom
                            right: parent.right
                            margins: 8
                        }
                        text: model.duration + " · " + model.size
                        color: "#c9c9d4"
                        font.pixelSize: 11
                    }
                }

                TapHandler {
                    onTapped: grid.currentIndex = index
                }
            }

            ScrollBar.vertical: ScrollBar { }
        }

        Label {
            Layout.fillWidth: true
            Layout.margins: 6
            text: qsTr("Milestone 0 — virtual grid smoke test; catalogue, search, and previews arrive in later milestones.")
            color: "#8b8b96"
            font.pixelSize: 12
        }
    }
}
