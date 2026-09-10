// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: control
    property string title
    property string suffix: ""
    property real scaleMaximum: 100
    property bool unlimited: true
    property real stepSize: 1
    property bool integerOnly: false
    property alias minimumText: minimumField.text
    property alias maximumText: maximumField.text
    readonly property real minimum: minimumText.trim() === "" ? 0 : Number(minimumText)
    readonly property real maximum: maximumText.trim() === "" ? (unlimited ? -1 : scaleMaximum) : Number(maximumText)
    readonly property bool valid: isFinite(minimum) && isFinite(maximum) && minimum >= 0
        && (maximum >= minimum || (unlimited && maximumText.trim() === ""))
        && (unlimited || (maximum <= scaleMaximum && minimum <= scaleMaximum))
        && (!integerOnly || (Number.isInteger(minimum) && Number.isInteger(maximum)))
    signal edited()
    spacing: 6

    function reset() { minimumText = ""; maximumText = "" }

    Label { text: control.title; color: "#eeeee9"; font.pixelSize: 13 }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Repeater {
            model: [qsTr("Min"), qsTr("Max")]
            Label { required property string modelData; text: modelData; color: "#a4a69e"; font.pixelSize: 11; Layout.fillWidth: true; Layout.preferredWidth: 1 }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        TextField {
            id: minimumField
            objectName: "minimumField"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            implicitHeight: 32
            placeholderText: "0"
            selectByMouse: true
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            Accessible.name: qsTr("Minimum %1").arg(control.title)
            onTextEdited: control.edited()
            background: Rectangle { radius: 6; color: "#20221f"; border.color: !control.valid ? "#edaaa0" : minimumField.activeFocus ? "#ddbe8b" : "#373a34" }
        }
        TextField {
            id: maximumField
            objectName: "maximumField"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            implicitHeight: 32
            placeholderText: control.unlimited ? qsTr("Any") : String(control.scaleMaximum)
            selectByMouse: true
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            Accessible.name: qsTr("Maximum %1").arg(control.title)
            onTextEdited: control.edited()
            background: Rectangle { radius: 6; color: "#20221f"; border.color: !control.valid ? "#edaaa0" : maximumField.activeFocus ? "#ddbe8b" : "#373a34" }
        }
    }
    RangeSlider {
        id: slider
        objectName: "rangeSlider"
        Layout.fillWidth: true
        implicitHeight: 26
        from: 0
        to: control.valid ? Math.max(control.scaleMaximum, control.minimum, control.maximum) : control.scaleMaximum
        stepSize: control.stepSize
        snapMode: RangeSlider.SnapAlways
        first.value: control.valid ? control.minimum : 0
        second.value: control.valid && control.maximum >= 0 ? control.maximum : to
        first.onMoved: { control.minimumText = String(Math.round(first.value * 100) / 100); control.edited() }
        second.onMoved: { control.maximumText = control.unlimited && second.value === to ? "" : String(Math.round(second.value * 100) / 100); control.edited() }
        first.handle: Rectangle {
            x: slider.leftPadding + slider.first.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + (slider.availableHeight - height) / 2
            width: 14; height: 14; radius: 7
            color: slider.first.pressed ? "#ddbe8b" : "#eeeee9"
            border.width: slider.first.handle.activeFocus ? 2 : 1
            border.color: slider.first.handle.activeFocus ? "#ddbe8b" : "#141613"
            Accessible.name: qsTr("Minimum %1").arg(control.title)
        }
        second.handle: Rectangle {
            x: slider.leftPadding + slider.second.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + (slider.availableHeight - height) / 2
            width: 14; height: 14; radius: 7
            color: slider.second.pressed ? "#ddbe8b" : "#eeeee9"
            border.width: slider.second.handle.activeFocus ? 2 : 1
            border.color: slider.second.handle.activeFocus ? "#ddbe8b" : "#141613"
            Accessible.name: qsTr("Maximum %1").arg(control.title)
        }
        background: Rectangle {
            x: slider.leftPadding + 7
            y: slider.topPadding + slider.availableHeight / 2 - 2
            width: slider.availableWidth - 14
            height: 4; radius: 2; color: "#373a34"
            Rectangle { x: slider.first.position * parent.width; width: (slider.second.position - slider.first.position) * parent.width; height: 4; radius: 2; color: "#ddbe8b" }
        }
    }
    Label {
        Layout.fillWidth: true
        visible: !control.valid || control.suffix !== ""
        text: control.valid ? control.suffix : qsTr("Enter a valid range, min ≤ max.")
        color: control.valid ? "#a4a69e" : "#edaaa0"
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }
}
