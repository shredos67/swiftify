pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T
import Swiftify

// a song in the immersive queue (FullscreenQueueRow): its cover, name and
// artists, and a speaker beside the one playing. the others play on click.
T.AbstractButton {
    id: root

    property song song
    property bool current: false
    property MaterialScheme scheme: PlayerStyle.darkScheme

    enabled: !current
    hoverEnabled: true
    // click focus would let other mouse buttons grab and press it
    focusPolicy: Qt.TabFocus
    padding: 8
    leftPadding: 12
    rightPadding: 12
    implicitWidth: 300
    implicitHeight: 54 + topPadding + bottomPadding

    background: Rectangle {
        radius: 14
        color: Qt.rgba(1, 1, 1, root.current ? 0.07 : (root.down ? 0.08 : (root.hovered ? 0.04 : 0)))

        Behavior on color {
            ElementMoveFastColor {}
        }
    }

    contentItem: Item {
        ArtworkImage {
            id: art
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 54
            height: 54
            radius: 8
            source: root.song.artworkUrl
        }

        Column {
            anchors.left: art.right
            anchors.leftMargin: 13
            anchors.right: speaker.visible ? speaker.left : parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            ScrollingText {
                width: parent.width
                text: root.song.name
                horizontalAlignment: Text.AlignLeft
                font.pixelSize: Appearance.fontSize.small
                font.weight: root.current ? Font.Bold : Font.DemiBold
                color: root.current ? "white" : Qt.rgba(1, 1, 1, 0.84)
                running: root.current || root.hovered
            }
            StyledText {
                width: parent.width
                text: root.song.artists
                elide: Text.ElideRight
                font.pixelSize: Appearance.fontSize.smallie
                color: Qt.rgba(1, 1, 1, 0.6)
            }
        }

        MaterialSymbol {
            id: speaker
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            Reveal.shown: root.current
            text: "volume_up"
            fill: 1
            iconSize: 20
            color: root.scheme.primary
        }
    }

    HoverHandler {
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
}
