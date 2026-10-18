pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects
import Swiftify

// the immersive player's left side (FullscreenNowPlaying): the cover with its
// glow, inside the ring visualizer when that is on, then the song, the seek
// line in the cover's colour, the transport around a white play button, and
// the volume.
Item {
    id: root

    required property ImmersiveLayout layout
    property real metadataWidth: 300
    /// the panel is coming or going, so sizes ease rather than jump
    property bool animateLayout: false
    property MaterialScheme scheme: PlayerStyle.darkScheme

    readonly property bool showsRing: Settings.visualizersEnabled && Settings.immersiveVisualizer && Settings.circularVisualizer
    /// round inside the ring, or round on its own with visualizers off
    readonly property bool roundCover: showsRing || ((!Settings.visualizersEnabled || !Settings.immersiveVisualizer) && Settings.roundArtwork)
    readonly property bool compactHorizontal: layout.contentHeight < 460
    property real artworkSize: compactHorizontal ? Math.min(240, Math.max(0, layout.contentHeight - 16), width * 0.36)
                                                 : layout.artworkSize
    readonly property real innerSize: showsRing ? artworkSize * ring.innerRadiusRatio : artworkSize
    readonly property real artworkRadius: Math.max(18, innerSize * 0.055)
    /// the song the title and artist show, which over a crossfade changes
    /// once the old one has faded out
    property song shownSong

    Component.onCompleted: shownSong = Player.currentSong

    Connections {
        target: Player
        function onCurrentSongChanged() {
            if (Player.crossfadeMs > 0 && !Appearance.reduceMotion) {
                metadataFade.stop()
                metadataOut.duration = Player.crossfadeMs / 2
                metadataIn.duration = Player.crossfadeMs / 2
                metadataFade.restart()
            } else {
                metadataFade.stop()
                root.shownSong = Player.currentSong
            }
        }
    }

    SequentialAnimation {
        id: metadataFade
        NumberAnimation { id: metadataOut; target: metadata; property: "opacity"; to: 0; easing.type: Easing.InSine }
        ScriptAction { script: root.shownSong = Player.currentSong }
        NumberAnimation { id: metadataIn; target: metadata; property: "opacity"; to: 1; easing.type: Easing.OutSine }
        onStopped: {
            if (metadata.opacity < 1) {
                metadata.opacity = 1
                root.shownSong = Player.currentSong
            }
        }
    }

    component Time: StyledText {
        tabularNumbers: true
        font.pixelSize: Appearance.fontSize.smallest + 1
        font.weight: Font.Medium
        color: Qt.rgba(1, 1, 1, 0.78)
        style: Text.Raised
        styleColor: Qt.rgba(0, 0, 0, 0.45)
    }

    component Control: TransportButton {
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        scheme: root.scheme
        size: root.compactHorizontal ? 36 : 40
        iconSize: 24
        fillWhenToggled: false
        iconColor: toggled ? root.scheme.primary : "white"
        // a button with a shape has it for its state layer
        colBackgroundHover: hasShape ? "transparent" : Qt.rgba(1, 1, 1, 0.1)
        colBackgroundToggled: "transparent"
        colBackgroundToggledHover: hasShape ? "transparent" : Qt.rgba(1, 1, 1, 0.1)
    }

    Behavior on artworkSize {
        enabled: root.animateLayout
        ElementMove {}
    }

    implicitWidth: compactHorizontal ? artworkSize + 24 + Math.max(220, details.implicitWidth)
                                     : Math.max(artworkSize, metadataWidth, transport.implicitWidth)
    implicitHeight: content.implicitHeight

    Item {
        id: content
        width: root.width
        implicitHeight: root.compactHorizontal ? Math.max(root.artworkSize, details.implicitHeight)
                                                : root.artworkSize + root.layout.contentSpacing + details.implicitHeight

        Item {
            id: artwork
            objectName: "immersiveArtwork"
            x: root.compactHorizontal ? 0 : (content.width - width) / 2
            y: root.compactHorizontal ? (content.height - height) / 2 : 0
            width: root.artworkSize
            height: root.artworkSize
            // a new song's cover pops in with a spring, or sweeps in over a crossfade
            transform: Scale {
                id: pop
                origin.x: artwork.width / 2
                origin.y: artwork.height / 2
            }

            Connections {
                target: Player
                function onCurrentSongChanged() {
                    if (!Appearance.performanceMode && Player.crossfadeMs === 0) {
                        popIn.restart()
                    }
                }
            }
            NumberAnimation {
                id: popIn
                target: pop
                properties: "xScale,yScale"
                from: 0.9
                to: 1
                duration: 640
                easing.type: Easing.OutBack
                easing.overshoot: 2
            }

            ArtworkGlow {
                anchors.centerIn: parent
                width: root.innerSize
                height: root.innerSize
                intensity: 0.42
                glowRadius: 44
                cornerRadius: root.artworkRadius
                circular: root.roundCover
                primary: PlayerStyle.primary
                secondary: PlayerStyle.secondary
            }

            SpectrumView {
                id: ring
                anchors.fill: parent
                Reveal.shown: root.showsRing
                kind: SpectrumView.Ring
                wave: Settings.visualizerShape === "wave"
                sensitivity: Settings.visualizerSensitivity
                active: Player.playing
                // the bars rise from under the cover and are cut along its
                // squiggle line, so they start where the line is
                cutWaves: cover.showsSquiggle ? cover.squiggleWaves : 0
                cutRadius: cover.squiggleMiddle
                cutAmplitude: cover.squiggleAmplitude
                cutPhase: cover.squigglePhase
            }

            ArtworkImage {
                id: cover
                anchors.centerIn: parent
                width: root.innerSize
                height: root.innerSize
                radius: root.artworkRadius
                circular: root.roundCover
                // its edge squiggles with the music, in the cover's colour
                squiggleFollowsPlayback: true
                squiggleColor: PlayerStyle.primary
                source: Player.currentSong.artworkUrl
                crossfade: true
                revealDuration: Player.crossfadeMs
            }

            Toast {
                id: phrasePill
                anchors.centerIn: parent
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton
                acceptedModifiers: Qt.AltModifier
                onTapped: phrasePill.show(UiState.catchphrase)
            }
        }

        Column {
            id: details
            objectName: "immersiveDetails"
            x: root.compactHorizontal ? root.artworkSize + 24 : 0
            y: root.compactHorizontal ? (content.height - height) / 2 : root.artworkSize + root.layout.contentSpacing
            width: root.compactHorizontal ? Math.max(0, content.width - x) : content.width
            spacing: root.compactHorizontal ? 8 : root.layout.contentSpacing

            Column {
                id: metadata
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(root.metadataWidth, details.width)
                spacing: root.compactHorizontal ? 2 : 5

                layer.enabled: true
                layer.effect: MultiEffect {
                    shadowEnabled: true
                    shadowColor: PlayerStyle.primary
                    shadowOpacity: 0.12
                    shadowBlur: 1
                    blurMax: 18
                    shadowHorizontalOffset: 0
                    shadowVerticalOffset: 0
                }

                ScrollingText {
                    objectName: "immersiveTitle"
                    width: parent.width
                    height: root.compactHorizontal ? 52 : root.layout.titleHeight
                    text: root.shownSong.name
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: root.compactHorizontal ? 24 : root.layout.titleFontSize
                    font.weight: Font.Bold
                    color: "white"
                    rounded: true
                    animateChange: !metadataFade.running
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 5

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, metadata.width - more.width - 5)
                        text: root.shownSong.artists
                        elide: Text.ElideRight
                        font.pixelSize: Appearance.fontSize.larger
                        font.weight: Font.Medium
                        color: Qt.rgba(1, 1, 1, 0.62)
                    }
                    IconButton {
                        id: more
                        anchors.verticalCenter: parent.verticalCenter
                        scheme: root.scheme
                        symbol: "more_horiz"
                        size: 28
                        iconSize: 20
                        iconColor: Qt.rgba(1, 1, 1, 0.62)
                        colBackgroundHover: Qt.rgba(1, 1, 1, 0.1)
                        tooltip: qsTr("More")
                        onClicked: SongMenus.open(Player.currentSong, false)
                    }
                }

                StyledText {
                    width: parent.width
                    visible: root.shownSong.albumName.length > 0
                    text: root.shownSong.albumName
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    font.pixelSize: Appearance.fontSize.smallie
                    color: Qt.rgba(1, 1, 1, 0.4)
                }
            }

            Item {
                id: progress
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(details.width, root.compactHorizontal ? details.width : Math.max(320, root.artworkSize + 80))
                height: 18


                Time {
                    id: elapsed
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 42
                    horizontalAlignment: Text.AlignRight
                    text: Format.playbackTime(seek.displayPositionMs)
                }
                SeekBar {
                    id: seek
                    objectName: "immersiveSeek"
                    anchors.left: elapsed.right
                    anchors.leftMargin: 12
                    anchors.right: remainingTime.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    height: 18
                    scheme: root.scheme
                    followsCrossfade: true
                    activeColor: PlayerStyle.primary
                    trackColor: Qt.rgba(1, 1, 1, 0.2)
                }
                Time {
                    id: remainingTime
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 48
                    text: "−" + Format.playbackTime(Math.max(seek.durationMs - seek.displayPositionMs, 0))
                }
            }

            Row {
                id: transport
                objectName: "immersiveTransport"
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: root.compactHorizontal ? Math.max(8, Math.min(28, (details.width - 196) / 4)) : 28


                Control {
                    effect: TransportButton.Bounce
                    symbol: "shuffle"
                    toggledShape: "clover4"
                    toggled: Player.shuffle
                    enabled: Player.hasSong
                    tooltip: qsTr("Shuffle")
                    onClicked: Player.toggleShuffle()
                }
                Control {
                    effect: TransportButton.NudgeLeft
                    symbol: "skip_previous"
                    hoverShape: "arrow"
                    hoverAngle: -90
                    iconSize: 30
                    alwaysFilled: true
                    enabled: Player.canSkipBackward
                    tooltip: qsTr("Previous song")
                    onClicked: Player.skipBackward()
                }

                // play is the soft cookie that springs into a turning scalloped
                // one while music plays, in white here
                Item {
                    anchors.verticalCenter: parent.verticalCenter
                    width: root.compactHorizontal ? 52 : 60
                    height: width

                    SoftShadow {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: width / 2
                        blur: 15
                        color: Qt.rgba(PlayerStyle.primary.r, PlayerStyle.primary.g, PlayerStyle.primary.b, 0.34)
                    }
                    PlayButton {
                        anchors.fill: parent
                        enabled: Player.hasSong
                        color: "white"
                        hoverColor: "#ececec"
                        iconColor: "black"
                        iconSize: 32
                        onClicked: Player.togglePlayback()
                    }
                }

                Control {
                    effect: TransportButton.NudgeRight
                    symbol: "skip_next"
                    hoverShape: "arrow"
                    hoverAngle: 90
                    iconSize: 30
                    alwaysFilled: true
                    enabled: Player.canSkipForward
                    tooltip: qsTr("Next song")
                    onClicked: Player.skipForward()
                }
                Control {
                    effect: TransportButton.Bounce
                    symbol: Player.repeatMode === Player.RepeatMode.One ? "repeat_one" : "repeat"
                    toggledShape: Player.repeatMode === Player.RepeatMode.One ? "cookie4" : "cookie6"
                    toggled: Player.repeatMode !== Player.RepeatMode.Off
                    enabled: Player.hasSong
                    tooltip: Player.repeatMode === Player.RepeatMode.One ? qsTr("Repeat song") : (Player.repeatMode === Player.RepeatMode.All ? qsTr("Repeat queue") : qsTr("Repeat off"))
                    onClicked: Player.cycleRepeatMode()
                }
            }

            // the speaker mutes; the slider is locked while the sleep timer fades
            Row {
                id: volume
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    scheme: root.scheme
                    symbol: Player.volume <= 0.01 ? "volume_off" : (Player.volume <= 0.5 ? "volume_down" : "volume_up")
                    size: 30
                    iconSize: 20
                    iconColor: Qt.rgba(1, 1, 1, 0.78)
                    colBackgroundHover: Qt.rgba(1, 1, 1, 0.1)
                    tooltip: Player.volume <= 0.01 ? qsTr("Unmute") : qsTr("Mute")
                    onClicked: Player.toggleMute()
                }
                StyledSlider {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(Math.max(0, progress.width - 40), 300)
                    scheme: root.scheme
                    trackThickness: 8
                    handleLength: 20
                    showStopIndicator: false
                    showValueTip: true
                    activeColor: "white"
                    inactiveColor: Qt.rgba(1, 1, 1, 0.2)
                    enabled: !Player.volumeManagedBySleepTimer
                    value: Player.volume
                    onMoved: Player.setVolume(value)
                }
            }
        }
    }
}
