#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>

#include <memory>

class QWindow;

namespace swiftify {

class PlaybackController;
class Theme;

class WindowsIntegration : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    WindowsIntegration(PlaybackController *player, Theme *theme, QObject *parent = nullptr);
    ~WindowsIntegration() override;

    static void initializeProcess();
    static void setNativeTitleBar(QWindow *window, bool enabled);
    void setWindow(QWindow *window);
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    void handleMediaButton(int button);

private:
    struct Native;

    void attachWindow();
    void styleWindow();
    void initializeTaskbar();
    void updateButtons();
    void updateProgress();
    void initializeMediaControls();
    void updateMediaControls();
    void updateMediaMetadata();
    void updateMediaTimeline();

    PlaybackController *m_player;
    Theme *m_theme;
    QPointer<QWindow> m_window;
    std::unique_ptr<Native> m_native;
};

} // namespace swiftify
