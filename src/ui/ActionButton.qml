// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl

Button {
    id: control
    property string iconName: ""
    property bool primary: false
    property bool selected: false
    property bool alignLeft: false
    implicitHeight: 36
    implicitWidth: Math.max(text.length ? 80 : 36, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: 12
    rightPadding: 12
    spacing: 9
    font.pixelSize: 13
    icon.source: iconName ? Qt.resolvedUrl("icons/" + iconName + ".svg") : ""
    icon.width: 17
    icon.height: 17
    icon.color: primary ? "#191918" : selected ? "#e2c18c" : enabled ? "#d1d0cc" : "#787974"
    palette.buttonText: primary ? "#191918" : selected ? "#e2c18c" : "#d1d0cc"
    contentItem: IconLabel {
        icon: control.icon
        text: control.text
        font: control.font
        spacing: control.spacing
        display: control.display
        mirrored: control.mirrored
        alignment: control.alignLeft ? Qt.AlignLeft : Qt.AlignCenter
        color: control.palette.buttonText
    }
    background: Rectangle {
        radius: 6
        color: control.primary ? (control.down ? "#b99b6c" : "#ddbe8b")
             : control.selected ? "#343027"
             : control.down ? "#373834"
             : control.hovered ? "#2e302d" : control.flat ? "transparent" : "#252724"
        border.width: control.activeFocus ? 1 : 0
        border.color: "#ddbe8b"
        opacity: control.enabled ? 1 : 0.45
        Behavior on color { ColorAnimation { duration: 90 } }
    }
    ToolTip.visible: hovered && text === "" && Accessible.name !== ""
    ToolTip.text: Accessible.name
    ToolTip.delay: 600
}
