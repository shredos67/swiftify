#pragma once

#include <QtGlobal>
#include <algorithm>

namespace swiftify {

/// SMTC TimeSpan values are 100-nanosecond units, not milliseconds. Keep the
/// calculation independent of WinRT so its bounds/conversion are tested on all OSes.
struct MediaTimeline {
    qint64 end = 0;
    qint64 position = 0;

    static MediaTimeline fromMilliseconds(bool hasSong, quint32 engineDuration,
                                          int metadataDuration, quint32 livePosition)
    {
        if (!hasSong) return {};
        const qint64 duration = engineDuration > 0 ? qint64(engineDuration) : std::max(0, metadataDuration);
        return {duration * 10000, std::clamp(qint64(livePosition), qint64(0), duration) * 10000};
    }
};

} // namespace swiftify
