pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects
import Swiftify

// the immersive lyrics (FullscreenLyrics): large bold lines, the sung one
// bright and glowing in the cover's first colour, kept in the middle, the
// column fading out at its edges. clicking a synchronised line flashes it and
// seeks there.
Item {
    id: root

    property MaterialScheme scheme: PlayerStyle.darkScheme
    readonly property color glow: Qt.rgba(PlayerStyle.primary.r, PlayerStyle.primary.g, PlayerStyle.primary.b, 0.5)

    Column {
        anchors.centerIn: parent
        Reveal.shown: Player.loadingLyrics
        spacing: 14

        LoadingSpinner {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 36
            height: 36
            color: root.scheme.primary
            trackColor: Qt.rgba(1, 1, 1, 0.12)
        }
        StyledText {
            text: qsTr("Loading lyrics…")
            color: Qt.rgba(1, 1, 1, 0.7)
        }
    }

    EmptyState {
        anchors.centerIn: parent
        Reveal.shown: !Player.loadingLyrics && !Player.hasLyrics
        scheme: root.scheme
        symbol: "lyrics"
        title: qsTr("Lyrics Unavailable")
        description: Player.lyricsMessage.length > 0 ? Player.lyricsMessage : UiState.catchphrase
    }

    ListView {
        id: list
        objectName: "immersiveLyricsList"
        anchors.fill: parent
        Reveal.shown: !Player.loadingLyrics && Player.hasLyrics
        model: Player.lyrics
        spacing: 26
        topMargin: 30
        bottomMargin: 30
        leftMargin: 8
        rightMargin: 8
        // dragged past either end, it stretches and springs back
        boundsBehavior: Flickable.DragAndOvershootBounds
        cacheBuffer: 1200
        // the fade hides lines scrolled past the edge, the clip keeps them
        // from taking clicks meant for the top bar
        clip: true
        // a step moves every line at once; each then springs from where it
        // was to its new place, the ones nearest the sung line first, so the
        // column arrives as a wave rather than a block
        signal stepped(real delta)


        layer.enabled: true
        layer.effect: ShaderEffect {
            property variant source
            property real edge: 0.08
            fragmentShader: "qrc:/shaders/edge_fade.frag.qsb"
        }

        function follow(animated: bool) {
            const index = Player.activeLyricIndex
            if (index < 0) {
                return
            }
            const item = list.itemAtIndex(index)
            if (!item) {
                scroll.stop()
                list.positionViewAtIndex(index, ListView.Center)
                return
            }
            const target = Math.max(list.originY - list.topMargin,
                                    Math.min(item.y + item.height / 2 - list.height / 2,
                                             list.contentHeight + list.originY - list.height + list.bottomMargin))
            const delta = target - list.contentY
            scroll.stop()
            // with reduced motion the column scrolls there on a plain ease
            // out, every line together, rather than as a wave
            if (animated && Appearance.reduceMotion && !Appearance.performanceMode) {
                scroll.to = target
                scroll.start()
                return
            }
            list.contentY = target
            if (animated && !Appearance.reduceMotion) {
                list.stepped(delta)
            }
        }

        NumberAnimation {
            id: scroll
            target: list
            property: "contentY"
            duration: 420
            easing.type: Easing.OutCubic
        }

        Connections {
            target: Player
            function onActiveLyricIndexChanged() { list.follow(true) }
        }
        Component.onCompleted: Qt.callLater(list.follow, false)

        delegate: Item {
            id: line

            required property int index
            required property string words
            required property int startTimeMs
            readonly property bool active: !Player.lyricsSynchronized || Player.activeLyricIndex === index
            property bool highlighted: false
            property real glowStrength: active ? 1 : 0

            Behavior on glowStrength {
                NumberAnimation { duration: Appearance.duration(320); easing.type: Easing.InOutQuad }
            }

            width: list.width - list.leftMargin - list.rightMargin
            height: label.implicitHeight + 16

            property real lag: 0
            transform: Translate { y: line.lag }

            Connections {
                target: list
                function onStepped(delta: real) {
                    line.lag += delta
                    settle.restart()
                }
            }
            SequentialAnimation {
                id: settle
                // a gentle ripple: a short stagger and a small overshoot
                PauseAnimation { duration: Math.min(Math.abs(line.index - Player.activeLyricIndex), 8) * 16 }
                NumberAnimation { target: line; property: "lag"; to: 0; duration: 560; easing.type: Easing.OutBack; easing.overshoot: 0.6 }
            }

            // the sung line swells a touch, on a spring, or with reduced
            // motion on the plain ease out the column scrolls with
            property real springing: active ? 1 : 0.96
            property real eased: active ? 1 : 0.96
            scale: Appearance.reduceMotion ? eased : springing
            transformOrigin: Item.Left

            Behavior on springing {
                enabled: !Appearance.reduceMotion
                Bounce {}
            }
            Behavior on eased {
                enabled: Appearance.reduceMotion && !Appearance.performanceMode
                NumberAnimation { duration: 420; easing.type: Easing.OutCubic }
            }


            // the sung line sits on a scalloped pill
            WavyPill {
                objectName: "lyricHighlight"
                anchors.fill: parent
                cornerRadius: 32
                color: Qt.rgba(1, 1, 1, line.active && Player.lyricsSynchronized ? 0.07 : 0)

                Behavior on color {
                    ColorAnimation { duration: Appearance.duration(320); easing.type: Easing.InOutQuad }
                }
            }

            Rectangle {
                anchors.fill: parent
                radius: 20
                color: Qt.rgba(1, 1, 1, line.highlighted ? 0.13 : 0)

                Behavior on color {
                    ColorAnimation { duration: Appearance.duration(180); easing.type: Easing.OutQuad }
                }
            }

            StyledText {
                id: label
                objectName: "lyricText"
                width: parent.width
                y: 8
                leftPadding: 24
                rightPadding: 24
                topPadding: 14
                bottomPadding: 14
                text: line.words.length > 0 ? line.words : "♪"
                wrapMode: Text.Wrap
                font.pixelSize: 30
                font.weight: Font.Bold
                color: "white"
                opacity: line.active ? 1 : 0.28

                Behavior on opacity {
                    NumberAnimation { duration: Appearance.duration(320); easing.type: Easing.InOutQuad }
                }

                layer.enabled: line.glowStrength > 0.01
                layer.effect: MultiEffect {
                    shadowEnabled: true
                    shadowColor: root.glow
                    shadowOpacity: line.glowStrength
                    shadowBlur: 1
                    blurMax: 24
                    shadowHorizontalOffset: 0
                    shadowVerticalOffset: 0
                }
            }

            Timer {
                id: flash
                interval: 80
                onTriggered: line.highlighted = false
            }

            MouseArea {
                anchors.fill: parent
                enabled: Player.lyricsSynchronized
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    line.highlighted = true
                    flash.restart()
                    Player.seekToLyricLine(line.index)
                }
            }
        }

        footer: StyledText {
            leftPadding: 18
            topPadding: 46
            text: qsTr("Lyrics provided by ") + Player.lyricsProvider
            font.pixelSize: Appearance.fontSize.smaller
            color: Qt.rgba(1, 1, 1, 0.4)
        }
    }
}
