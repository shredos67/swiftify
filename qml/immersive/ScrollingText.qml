pragma ComponentBehavior: Bound
import QtQuick
import Swiftify

// text that scrolls horizontally when its contents overflow the available
// width instead of cutting off or eliding. when it fits, it remains still and
// follows horizontalAlignment (centered for titles, left-aligned for rows).
// with reduced motion or performance mode, it stays still and elides gracefully.
Item {
    id: root

    property string text: ""
    property color color: Appearance.colors.onSurface
    property font font
    property int horizontalAlignment: Text.AlignHCenter
    property real spacing: 48
    property real speed: 38
    property int pauseDuration: 2000
    property bool running: true
    property bool rounded: false
    property bool animateChange: false

    clip: true

    implicitWidth: sample.implicitWidth
    implicitHeight: sample.implicitHeight

    readonly property real textWidth: Math.ceil(sample.implicitWidth)
    readonly property real textHeight: Math.ceil(sample.implicitHeight)
    readonly property bool overflows: root.width > 0 && root.textWidth > root.width
    readonly property bool shouldScroll: root.overflows && root.running && root.visible && !Appearance.reduceMotion
    readonly property real scrollDistance: root.textWidth + root.spacing

    readonly property real targetEdge: Math.min(0.12, Math.max(0.04, root.width > 0 ? 24.0 / root.width : 0.08))
    readonly property real currentScroll: Math.max(0.0, -slider.x)
    readonly property real remainingScroll: Math.max(0.0, root.scrollDistance + slider.x)
    readonly property real edgeDistance: Math.min(currentScroll, remainingScroll)
    readonly property real fadeProgress: Math.min(1.0, edgeDistance / 24.0)
    readonly property real leftEdge: targetEdge * fadeProgress
    readonly property real rightEdge: targetEdge

    layer.enabled: root.shouldScroll
    layer.effect: ShaderEffect {
        property variant source
        property real leftEdge: root.leftEdge
        property real rightEdge: root.rightEdge
        fragmentShader: "qrc:/shaders/horizontal_edge_fade.frag.qsb"
    }

    StyledText {
        id: sample
        visible: false
        text: root.text
        font.family: root.font.family.length > 0 ? root.font.family : Appearance.fontFamily
        font.pixelSize: root.font.pixelSize > 0 ? root.font.pixelSize : Appearance.fontSize.small
        font.weight: root.font.weight
        font.variableAxes: root.rounded ? ({ "ROND": 100, "wdth": 110, "wght": 720 }) : ({})
    }

    Item {
        id: slider
        height: root.height > 0 ? root.height : root.textHeight
        width: Math.max(root.width, root.textWidth * 2 + root.spacing)

        StyledText {
            id: label1
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            font.family: sample.font.family
            font.pixelSize: sample.font.pixelSize
            font.weight: sample.font.weight
            font.variableAxes: sample.font.variableAxes
            color: root.color
            animateChange: root.animateChange
            width: (!root.shouldScroll && root.overflows) ? root.width : implicitWidth
            elide: (!root.shouldScroll && root.overflows) ? Text.ElideRight : Text.ElideNone
        }

        StyledText {
            id: label2
            x: root.textWidth + root.spacing
            anchors.verticalCenter: parent.verticalCenter
            visible: root.shouldScroll
            text: root.text
            font.family: sample.font.family
            font.pixelSize: sample.font.pixelSize
            font.weight: sample.font.weight
            font.variableAxes: sample.font.variableAxes
            color: root.color
        }
    }

    SequentialAnimation {
        id: scrollAnimation
        running: false
        loops: Animation.Infinite

        PauseAnimation {
            duration: root.pauseDuration
        }

        NumberAnimation {
            target: slider
            property: "x"
            from: 0
            to: -root.scrollDistance
            duration: Math.max(100, Math.round((root.scrollDistance / root.speed) * 1000))
            easing.type: Easing.Linear
        }

        PropertyAction {
            target: slider
            property: "x"
            value: 0
        }
    }

    function resetPosition() {
        scrollAnimation.stop()
        if (root.shouldScroll) {
            slider.x = 0
            scrollAnimation.restart()
        } else {
            slider.x = root.horizontalAlignment === Text.AlignHCenter
                ? Math.max(0, Math.round((root.width - root.textWidth) / 2))
                : 0
        }
    }

    onTextChanged: resetPosition()
    onWidthChanged: resetPosition()
    onShouldScrollChanged: resetPosition()
    onHorizontalAlignmentChanged: resetPosition()
    Component.onCompleted: resetPosition()
}
