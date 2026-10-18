pragma ComponentBehavior: Bound
import QtQuick
import Swiftify

// Playing Next in the immersive player (FullscreenQueue): the playing song,
// then the rest. when the queue steps on, the rows follow one after another
// (StaggeredListMotion): each starts a little later than the one above, up to
// the twelfth, so the column arrives as a wave rather than a block.
Item {
    id: root

    property MaterialScheme scheme: PlayerStyle.darkScheme

    // StaggeredListMotion's numbers
    readonly property int settleMs: 420
    readonly property int stepMs: 30
    readonly property int depth: 12

    EmptyState {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 80
        Reveal.shown: !Player.hasSong && Player.upNext.count === 0
        scheme: root.scheme
        symbol: "queue_music"
        title: qsTr("Queue Empty")
        description: UiState.catchphrase
    }

    ListView {
        id: list
        anchors.fill: parent
        Reveal.shown: Player.hasSong || Player.upNext.count > 0
        model: Player.upNext
        spacing: 10
        topMargin: 30
        bottomMargin: 30
        leftMargin: 8
        rightMargin: 8
        boundsBehavior: Flickable.StopAtBounds
        // the fade hides lines scrolled past the edge, the clip keeps them
        // from taking clicks meant for the top bar
        clip: true

        layer.enabled: true
        layer.effect: ShaderEffect {
            property variant source
            property real edge: 0.08
            fragmentShader: "qrc:/shaders/edge_fade.frag.qsb"
        }

        header: Item {
            width: list.width - list.leftMargin - list.rightMargin
            height: Player.hasSong ? current.implicitHeight + list.spacing : 0

            ImmersiveQueueRow {
                id: current
                width: parent.width
                Reveal.shown: Player.hasSong
                song: Player.currentSong
                current: true
                scheme: root.scheme
            }
        }

        delegate: ImmersiveQueueRow {
            required property int index
            width: list.width - list.leftMargin - list.rightMargin
            song: Player.upNext.at(index)
            scheme: root.scheme
            onClicked: Player.playFromQueue(song)
        }

        displaced: Transition {
            enabled: !Appearance.performanceMode
            SequentialAnimation {
                PauseAnimation { duration: Math.min(Math.max(ViewTransition.index, 0), root.depth) * root.stepMs }
                NumberAnimation {
                    property: "y"
                    duration: root.settleMs
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: Appearance.curves.expressiveDefaultSpatial
                }
            }
        }
        add: Transition {
            enabled: !Appearance.performanceMode
            SequentialAnimation {
                PauseAnimation { duration: Math.min(Math.max(ViewTransition.index, 0), root.depth) * root.stepMs }
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: root.settleMs; easing.type: Easing.OutCubic }
            }
        }
        remove: Transition {
            enabled: !Appearance.performanceMode
            NumberAnimation { property: "opacity"; to: 0; duration: 180; easing.type: Easing.OutCubic }
        }
    }
}
