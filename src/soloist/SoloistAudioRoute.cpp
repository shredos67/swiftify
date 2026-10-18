#include "soloist/SoloistAudioRoute.h"

#include <swiftify_core.h>

#include <QElapsedTimer>
#include <QLoggingCategory>

#include <pulse/pulseaudio.h>

Q_DECLARE_LOGGING_CATEGORY(lcSoloist)

namespace swiftify {

namespace {

const char *const kSinkName = "swiftify_soloist";

/// what a pulse callback reports back to the thread waiting on it
struct Reply {
    pa_threaded_mainloop *mainloop;
    bool done = false;
    bool found = false;
    quint32 index = PA_INVALID_INDEX;
    QByteArray name;
    QByteArray server;
};

void signal(Reply *reply)
{
    reply->done = true;
    pa_threaded_mainloop_signal(reply->mainloop, 0);
}

} // namespace

SoloistAudioRoute::SoloistAudioRoute(SwiftifyLiveBuffer *buffer, quint32 rate)
    : m_buffer(buffer)
    , m_rate(rate)
{
}

SoloistAudioRoute::~SoloistAudioRoute()
{
    stop();
}

QString SoloistAudioRoute::sinkName()
{
    return QString::fromLatin1(kSinkName);
}

template <typename Done>
bool SoloistAudioRoute::waitFor(Done done, int timeoutMs)
{
    // the wait itself has no timeout, so a timer wakes it once time is up
    pa_mainloop_api *api = pa_threaded_mainloop_get_api(m_mainloop);
    timeval deadline{};
    pa_timeval_add(pa_gettimeofday(&deadline), pa_usec_t(timeoutMs) * PA_USEC_PER_MSEC);
    pa_time_event *timer = api->time_new(api, &deadline, [](pa_mainloop_api *, pa_time_event *, const timeval *, void *self) {
        pa_threaded_mainloop_signal(static_cast<SoloistAudioRoute *>(self)->m_mainloop, 0);
    }, this);
    QElapsedTimer waited;
    waited.start();
    bool ok = true;
    while (!done()) {
        const pa_context_state_t state = pa_context_get_state(m_context);
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED || waited.elapsed() >= timeoutMs) {
            ok = false;
            break;
        }
        pa_threaded_mainloop_wait(m_mainloop);
    }
    api->time_free(timer);
    return ok;
}

void SoloistAudioRoute::onContextState(pa_context *, void *self)
{
    pa_threaded_mainloop_signal(static_cast<SoloistAudioRoute *>(self)->m_mainloop, 0);
}

void SoloistAudioRoute::onStreamState(pa_stream *, void *self)
{
    pa_threaded_mainloop_signal(static_cast<SoloistAudioRoute *>(self)->m_mainloop, 0);
}

void SoloistAudioRoute::onRead(pa_stream *stream, size_t, void *self)
{
    auto *route = static_cast<SoloistAudioRoute *>(self);
    const void *data = nullptr;
    size_t bytes = 0;
    while (pa_stream_readable_size(stream) > 0) {
        if (pa_stream_peek(stream, &data, &bytes) < 0 || bytes == 0) {
            return;
        }
        // a hole (no data) is skipped; the output pads it with silence
        if (data) {
            swiftify_live_buffer_push(route->m_buffer, static_cast<const float *>(data), bytes / sizeof(float));
        }
        pa_stream_drop(stream);
    }
}

bool SoloistAudioRoute::start()
{
    m_mainloop = pa_threaded_mainloop_new();
    if (!m_mainloop) {
        m_error = QStringLiteral("could not start a pulse mainloop");
        return false;
    }
    pa_proplist *properties = pa_proplist_new();
    pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME, "Swiftify");
    pa_proplist_sets(properties, PA_PROP_APPLICATION_ICON_NAME, "swiftify");
    m_context = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(m_mainloop), "Swiftify", properties);
    pa_proplist_free(properties);
    pa_context_set_state_callback(m_context, &SoloistAudioRoute::onContextState, this);
    if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0
        || pa_threaded_mainloop_start(m_mainloop) < 0) {
        m_error = QStringLiteral("could not reach the sound server");
        return false;
    }

    pa_threaded_mainloop_lock(m_mainloop);
    const auto fail = [this](const QString &message) {
        m_error = message;
        pa_threaded_mainloop_unlock(m_mainloop);
        return false;
    };
    if (!waitFor([this] { return pa_context_get_state(m_context) == PA_CONTEXT_READY; })) {
        return fail(QStringLiteral("could not connect to the sound server"));
    }

    const auto run = [this](pa_operation *operation, Reply &reply) {
        if (!operation) {
            return false;
        }
        const bool ok = waitFor([&] { return reply.done; });
        // one that timed out is cancelled, or its callback would still write
        // to the reply after it has gone off the stack
        if (!ok) {
            pa_operation_cancel(operation);
        }
        pa_operation_unref(operation);
        return ok;
    };

    // the default output before the sink exists: a new sink can be made the
    // default, and soloist's sink must never take the speakers' place
    Reply server{m_mainloop};
    run(pa_context_get_server_info(m_context, [](pa_context *, const pa_server_info *info, void *data) {
        auto *reply = static_cast<Reply *>(data);
        if (info && info->default_sink_name) {
            reply->name = info->default_sink_name;
        }
        if (info && info->server_name) {
            reply->server = info->server_name;
        }
        signal(reply);
    }, &server), server);
    // soloist only plays to a chosen sink through pipewire. under pulseaudio
    // alone it would play to the speakers while swiftify thought it routed
    if (!server.server.contains("PipeWire")) {
        return fail(QStringLiteral("the sound server isn't PipeWire (%1)").arg(QString::fromUtf8(server.server)));
    }

    Reply existing{m_mainloop};
    run(pa_context_get_sink_info_by_name(m_context, kSinkName, [](pa_context *, const pa_sink_info *info, int eol, void *data) {
        auto *reply = static_cast<Reply *>(data);
        if (info) {
            reply->found = true;
            reply->index = info->owner_module;
        }
        if (eol != 0) {
            signal(reply);
        }
    }, &existing), existing);

    // one left behind by a swiftify that didn't exit cleanly is made again,
    // so it runs at today's rate and goes when this one exits
    if (existing.found && existing.index != PA_INVALID_INDEX) {
        Reply unloaded{m_mainloop};
        run(pa_context_unload_module(m_context, existing.index, [](pa_context *, int success, void *data) {
            auto *reply = static_cast<Reply *>(data);
            reply->found = success != 0;
            signal(reply);
        }, &unloaded), unloaded);
        if (unloaded.found) {
            qCInfo(lcSoloist) << "Removed a leftover" << kSinkName << "sink";
            existing.found = false;
        }
    }

    if (!existing.found) {
        Reply loaded{m_mainloop};
        // at the output's rate too: spotify's 44.1kHz songs, played to
        // swiftify's 44.1kHz output, then need no resampling on the way
        const QByteArray arguments = QByteArrayLiteral("sink_name=") + kSinkName + QByteArrayLiteral(" rate=")
            + QByteArray::number(m_rate)
            + QByteArrayLiteral(" channels=2 sink_properties=device.description=\"Swiftify (Soloist)\"");
        const bool ok = run(pa_context_load_module(m_context, "module-null-sink", arguments.constData(),
                                                   [](pa_context *, uint32_t index, void *data) {
            auto *reply = static_cast<Reply *>(data);
            reply->index = index;
            signal(reply);
        }, &loaded), loaded);
        if (!ok || loaded.index == PA_INVALID_INDEX) {
            return fail(QStringLiteral("the sound server would not make a sink for Soloist"));
        }
        m_module = loaded.index;
        qCInfo(lcSoloist) << "Made the" << kSinkName << "sink";

        Reply after{m_mainloop};
        run(pa_context_get_server_info(m_context, [](pa_context *, const pa_server_info *info, void *data) {
            auto *reply = static_cast<Reply *>(data);
            if (info && info->default_sink_name) {
                reply->name = info->default_sink_name;
            }
            signal(reply);
        }, &after), after);
        if (after.name == kSinkName && !server.name.isEmpty() && server.name != kSinkName) {
            Reply restored{m_mainloop};
            run(pa_context_set_default_sink(m_context, server.name.constData(), [](pa_context *, int, void *data) {
                signal(static_cast<Reply *>(data));
            }, &restored), restored);
        }
    }

    pa_sample_spec spec{PA_SAMPLE_FLOAT32NE, m_rate, quint8(kChannels)};
    m_stream = pa_stream_new(m_context, "Soloist", &spec, nullptr);
    if (!m_stream) {
        return fail(QStringLiteral("could not record Soloist's sink"));
    }
    pa_stream_set_state_callback(m_stream, &SoloistAudioRoute::onStreamState, this);
    pa_stream_set_read_callback(m_stream, &SoloistAudioRoute::onRead, this);
    // small fragments: everything recorded is heard late by as much
    pa_buffer_attr attributes{};
    attributes.maxlength = quint32(-1);
    attributes.fragsize = quint32(pa_usec_to_bytes(10'000, &spec));
    const QByteArray monitor = QByteArray(kSinkName) + ".monitor";
    if (pa_stream_connect_record(m_stream, monitor.constData(), &attributes,
                                 pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE)) < 0
        || !waitFor([this] { return pa_stream_get_state(m_stream) == PA_STREAM_READY; })) {
        return fail(QStringLiteral("could not record Soloist's sink"));
    }
    pa_threaded_mainloop_unlock(m_mainloop);
    return true;
}

void SoloistAudioRoute::stop()
{
    if (!m_mainloop) {
        return;
    }
    pa_threaded_mainloop_lock(m_mainloop);
    if (m_stream) {
        pa_stream_set_read_callback(m_stream, nullptr, nullptr);
        pa_stream_set_state_callback(m_stream, nullptr, nullptr);
        pa_stream_disconnect(m_stream);
        pa_stream_unref(m_stream);
        m_stream = nullptr;
    }
    if (m_context && m_module != PA_INVALID_INDEX && pa_context_get_state(m_context) == PA_CONTEXT_READY) {
        Reply unloaded{m_mainloop};
        if (pa_operation *operation = pa_context_unload_module(m_context, m_module, [](pa_context *, int, void *data) {
                signal(static_cast<Reply *>(data));
            }, &unloaded)) {
            if (!waitFor([&] { return unloaded.done; }, 1000)) {
                pa_operation_cancel(operation);
            }
            pa_operation_unref(operation);
        }
        m_module = PA_INVALID_INDEX;
    }
    if (m_context) {
        pa_context_set_state_callback(m_context, nullptr, nullptr);
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
        m_context = nullptr;
    }
    pa_threaded_mainloop_unlock(m_mainloop);
    pa_threaded_mainloop_stop(m_mainloop);
    pa_threaded_mainloop_free(m_mainloop);
    m_mainloop = nullptr;
}

} // namespace swiftify
