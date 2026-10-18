use std::{
    io,
    path::PathBuf,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    time::{Duration, Instant},
};

use librespot::{
    core::{
        SpotifyId, SpotifyUri, authentication::Credentials, cache::Cache, config::SessionConfig,
        session::Session,
    },
    metadata::{Lyrics, lyrics::SyncType},
    playback::{
        config::{Bitrate, PlayerConfig, VolumeCtrl},
        mixer::{self, Mixer, MixerConfig, VolumeGetter},
        player::{Player, PlayerEvent, PlayerEventChannel},
    },
    protocol::authentication::AuthenticationType,
};
use serde::Serialize;
use tokio::{sync::watch, time::timeout};

mod artist;
mod dsp;
mod file_playback;
pub mod live_input;
pub mod local;
mod output;
mod playlist;
mod spectrum;

use dsp::{EQ_BAND_COUNT, SharedDsp};
use rodio::Source;
use spectrum::{OutputHealth, SharedSpectrum, SpectrumSink};

/// the output files play through. replaceable, not a oncelock: ios stops the audio unit behind
/// this stream whenever another app interrupts us, and a stopped unit never restarts
/// itself.
static LOCAL_AUDIO_SINK: Mutex<Option<Arc<output::LocalOutput>>> = Mutex::new(None);

/// empties the shared file sink, keeping it for the next file:
/// replacing it would open (and leak) another output stream
fn silence_cached_local_sink() {
    let Ok(cached) = LOCAL_AUDIO_SINK.lock() else {
        return;
    };
    if let Some(sink) = cached.as_ref() {
        sink.stop();
        sink.clear();
    }
}

fn stop_cached_local_sink() {
    let Ok(mut cached) = LOCAL_AUDIO_SINK.lock() else {
        return;
    };
    let Some(sink) = cached.take() else {
        return;
    };
    sink.stop();
    sink.clear();
}

#[cfg(feature = "capi")]
pub mod capi;
#[cfg(feature = "swift")]
mod swift_ffi;

/// the bands the equalizer is built from, so the ui can label its own sliders
/// from the filters that actually run rather than a copy of the list.
pub fn equalizer_frequencies() -> Vec<f32> {
    dsp::EQ_FREQUENCIES.to_vec()
}

pub fn core_version() -> String {
    env!("CARGO_PKG_VERSION").to_owned()
}

struct SpotifyState {
    session: Option<Session>,
    player: Option<PlayerSlot>,
    standby_player: Option<PlayerSlot>,
    mixer: Option<Arc<dyn Mixer>>,
    volume: f32,
    stream_bitrate: Bitrate,
    /// where streamed songs are kept, and the most it may hold in bytes
    audio_cache: Option<(String, u64)>,
    local_playback: Option<file_playback::LocalPlayback>,
    /// the output sink files play through. the backing outputstream is leaked so the
    /// sink keeps a live device connection without breaking send on the state.
    local_sink: Option<Arc<output::LocalOutput>>,
    /// a live buffer playing through the file output, and what stops it
    live: Option<(Arc<live_input::LiveBuffer>, Arc<AtomicBool>)>,
}

impl Default for SpotifyState {
    fn default() -> Self {
        Self {
            session: None,
            player: None,
            standby_player: None,
            mixer: None,
            volume: 0.75,
            stream_bitrate: Bitrate::default(),
            audio_cache: None,
            local_playback: None,
            local_sink: None,
            live: None,
        }
    }
}

#[derive(Clone)]
struct PlayerSlot {
    player: Arc<Player>,
    gain: Arc<AtomicU64>,
    active: Arc<AtomicBool>,
}

impl PlayerSlot {
    fn set_gain(&self, gain: f64) {
        self.gain.store(gain.clamp(0.0, 1.0).to_bits(), Ordering::Relaxed);
    }
}

struct PlayerVolume {
    base: Box<dyn VolumeGetter + Send>,
    gain: Arc<AtomicU64>,
}

impl VolumeGetter for PlayerVolume {
    fn attenuation_factor(&self) -> f64 {
        self.base.attenuation_factor()
            * f64::from_bits(self.gain.load(Ordering::Relaxed)).clamp(0.0, 1.0)
    }
}

#[derive(Default)]
struct PlaybackTimeline {
    position_ms: u32,
    duration_ms: u32,
    is_playing: bool,
    end_of_track: bool,
    observed_at: Option<Instant>,
}

impl PlaybackTimeline {
    /// how long the timeline will keep extrapolating between position updates.
    const STALL_TIMEOUT: Duration = Duration::from_secs(6);

    fn reset(&mut self) {
        *self = Self::default();
    }

    /// true once the player has claimed to be playing for longer than it can
    /// go without reporting a position.
    fn is_stalled(&self) -> bool {
        self.is_playing
            && self
                .observed_at
                .is_some_and(|observed_at| observed_at.elapsed() >= Self::STALL_TIMEOUT)
    }

    fn observe_position(&mut self, position_ms: u32, is_playing: Option<bool>) {
        self.position_ms = position_ms;
        if let Some(is_playing) = is_playing {
            self.is_playing = is_playing;
        }
        self.observed_at = Some(Instant::now());
    }

    fn current_position_ms(&self) -> u32 {
        let elapsed_ms = if self.is_playing {
            self.observed_at
                .map(|observed_at| {
                    observed_at.elapsed().min(Self::STALL_TIMEOUT).as_millis() as u32
                })
                .unwrap_or(0)
        } else {
            0
        };
        let position_ms = self.position_ms.saturating_add(elapsed_ms);

        if self.duration_ms == 0 {
            position_ms
        } else {
            position_ms.min(self.duration_ms)
        }
    }
}

/// what a play request that a newer one replaced ends with
/// (SWIFTIFY_CORE_SUPERSEDED in the c header). never shown: the front end has
/// withdrawn it already, or asks again for the song it still wants.
pub const SUPERSEDED: &str = "the request was replaced by a newer one";

pub struct SpotifyCore {
    state: Mutex<SpotifyState>,
    /// bumped by every request that changes what plays: each song asked for,
    /// an output rebuilt, a session replaced. a request keeps the number it
    /// started with, and once that is no longer the latest it gives up
    /// without touching either output: requests overlap on the runtime's
    /// workers, and one that finished switching after a newer one had played
    /// its song over the newer one
    playback_generation: watch::Sender<u64>,
    /// held while a request switches the outputs over, so two never
    /// interleave: one clearing the file sink between another's clearing
    /// and its starting left both songs playing
    switching: Mutex<()>,
    spectrum: SharedSpectrum,
    dsp: SharedDsp,
    timeline: Arc<Mutex<PlaybackTimeline>>,
    local_playback_active: Arc<AtomicBool>,
    /// shared with every sink this core builds, streaming and files alike.
    output_health: OutputHealth,
}

impl Default for SpotifyCore {
    fn default() -> Self {
        Self::new()
    }
}

impl SpotifyCore {
    pub fn new() -> Self {
        let _ = env_logger::try_init();

        Self {
            state: Mutex::new(SpotifyState::default()),
            playback_generation: watch::Sender::new(0),
            switching: Mutex::new(()),
            spectrum: SharedSpectrum::new(),
            dsp: SharedDsp::new(),
            timeline: Arc::new(Mutex::new(PlaybackTimeline::default())),
            local_playback_active: Arc::new(AtomicBool::new(false)),
            output_health: OutputHealth::new(),
        }
    }

    fn make_player_slot(
        &self,
        session: &Session,
        mixer: &Arc<dyn Mixer>,
        config: PlayerConfig,
        initially_active: bool,
        initial_gain: f64,
    ) -> PlayerSlot {
        let gain = Arc::new(AtomicU64::new(initial_gain.clamp(0.0, 1.0).to_bits()));
        let active = Arc::new(AtomicBool::new(initially_active));
        let volume = PlayerVolume {
            base: mixer.get_soft_volume(),
            gain: Arc::clone(&gain),
        };
        let output_volume: Box<dyn VolumeGetter + Send> = Box::new(PlayerVolume {
            base: mixer.get_soft_volume(),
            gain: Arc::clone(&gain),
        });
        let spectrum = self.spectrum.clone();
        let dsp = self.dsp.clone();
        let health = self.output_health.clone();
        let player = Player::new(config, session.clone(), Box::new(volume), move || {
            Box::new(SpectrumSink::new(spectrum, output_volume, dsp, health))
        });
        tokio::spawn(monitor_player_events(
            player.get_player_event_channel(),
            Arc::clone(&self.timeline),
            Arc::clone(&self.local_playback_active),
            Arc::clone(&active),
        ));
        PlayerSlot {
            player,
            gain,
            active,
        }
    }

    pub async fn connect(&self, access_token: String, client_id: String) -> Result<(), String> {
        self.connect_returning_login(access_token, client_id)
            .await
            .map(|_| ())
    }

    /// connects as `connect` does, and completes with what the session can
    /// log in with again without a token (librespot's reusable credentials),
    /// as json. keeping it means reconnecting never spends the oauth refresh
    /// token, which spotify rotates and revokes once an old one is reused
    pub async fn connect_returning_login(
        &self,
        access_token: String,
        client_id: String,
    ) -> Result<String, String> {
        if access_token.trim().is_empty() {
            return Err("access token cannot be empty".to_owned());
        }
        self.connect_with(Credentials::with_access_token(access_token), client_id)
            .await
    }

    /// connects with a login `connect_returning_login` gave, and completes
    /// with the one to keep from then on
    pub async fn connect_stored(&self, login: String, client_id: String) -> Result<String, String> {
        let credentials: Credentials = serde_json::from_str(&login)
            .map_err(|error| format!("the saved playback login is damaged: {error}"))?;
        if credentials.auth_type != AuthenticationType::AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS
            || credentials.username.as_deref().is_none_or(str::is_empty)
            || credentials.auth_data.is_empty()
        {
            return Err("the saved playback login is incomplete".to_owned());
        }
        self.connect_with(credentials, client_id).await
    }

    async fn connect_with(
        &self,
        credentials: Credentials,
        client_id: String,
    ) -> Result<String, String> {
        let client_id = client_id.trim();
        if client_id.is_empty() {
            return Err("spotify client ID cannot be empty".to_owned());
        }

        let session_config = SessionConfig {
            client_id: client_id.to_owned(),
            ..SessionConfig::default()
        };

        self.output_health.clear();
        // the songs streamed are kept (still encrypted, as spotify sends them)
        // so playing one again does not fetch it again; the oldest go first
        // once the cache is full. a cache that cannot be made is done without
        let audio_cache = self.lock_state()?.audio_cache.clone();
        let cache = audio_cache.and_then(|(path, limit)| {
            Cache::new(None, None, Some(path.as_str()), Some(limit))
                .map_err(|error| eprintln!("spotify-core: no song cache: {error}"))
                .ok()
        });
        let session = Session::new(session_config, cache);
        session
            .connect(credentials, false)
            .await
            .map_err(|error| format!("failed to connect to spotify: {error}"))?;
        let login = serde_json::to_string(&Credentials {
            username: Some(session.username()),
            auth_type: AuthenticationType::AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS,
            auth_data: session.auth_data(),
        })
        .map_err(|error| format!("could not keep the playback login: {error}"))?;

        let mixer_builder =
            mixer::find(None).ok_or_else(|| "no librespot volume mixer is available".to_owned())?;
        let mixer = mixer_builder(MixerConfig::default())
            .map_err(|error| format!("failed to create volume mixer: {error}"))?;
        let stream_bitrate = self.lock_state()?.stream_bitrate;
        let player_config = PlayerConfig {
            position_update_interval: Some(Duration::from_secs(1)),
            bitrate: stream_bitrate,
            ..PlayerConfig::default()
        };
        let player = self.make_player_slot(&session, &mixer, player_config.clone(), true, 1.0);
        let standby = self.make_player_slot(&session, &mixer, player_config, false, 0.0);

        // a request still waiting on the old player would keep it alive, and
        // its song would play beside whatever the new one plays
        self.begin_playback_request();
        let (previous, old_standby) = {
            let _switching = self.lock_switching();
            let mut state = self.lock_state()?;
            mixer.set_volume((state.volume * VolumeCtrl::MAX_VOLUME as f32).round() as u16);
            state.session = Some(session);
            state.mixer = Some(mixer);
            (state.player.replace(player), state.standby_player.replace(standby))
        };
        // drop outside the state lock because this may wait on the player thread
        if let Some(previous) = previous {
            previous.player.stop();
            drop(previous);
        }
        if let Some(old_standby) = old_standby {
            old_standby.player.stop();
        }

        Ok(login)
    }

    pub fn disconnect(&self) {
        self.begin_playback_request();
        self.clear_local_playback();
        let players = self.lock_state().ok().map(|mut state| {
            let players = (state.player.take(), state.standby_player.take());
            state.session = None;
            state.mixer = None;
            state.local_sink = None;
            players
        });
        if let Some((active, standby)) = players {
            if let Some(active) = active {
                active.player.pause();
            }
            if let Some(standby) = standby {
                standby.player.pause();
            }
        }
        self.spectrum.set_enabled(false);
        self.reset_timeline();
    }

    /// one gain per band, in decibels, in the order equalizer_frequencies
    /// reports. applied to streaming and file playback alike.
    pub fn set_equalizer(&self, gains_db: Vec<f32>) -> Result<(), String> {
        if gains_db.len() != EQ_BAND_COUNT {
            return Err(format!(
                "the equalizer takes {EQ_BAND_COUNT} gains, not {}",
                gains_db.len()
            ));
        }
        let mut settings = self.dsp.get();
        for (band, gain) in settings.gains_db.iter_mut().zip(gains_db) {
            *band = gain;
        }
        self.dsp.set(settings);
        Ok(())
    }

    /// tempo is a duration factor: 0.5 takes twice as long. preserves_pitch
    /// off is the slowed or nightcore sound, where the recording is simply
    /// played at another rate.
    pub fn set_playback_shaping(
        &self,
        tempo: f32,
        pitch_semitones: f32,
        preserves_pitch: bool,
    ) -> Result<(), String> {
        let mut settings = self.dsp.get();
        settings.tempo = tempo;
        settings.pitch_semitones = pitch_semitones;
        settings.preserves_pitch = preserves_pitch;
        self.dsp.set(settings);
        Ok(())
    }

    /// checks if the librespot session can still be used
    pub fn session_is_valid(&self) -> bool {
        let state = match self.lock_state() {
            Ok(state) => state,
            Err(_) => return false,
        };
        match &state.session {
            Some(session) => !session.is_invalid(),
            None => false,
        }
    }

    /// whether the audio device has stopped taking what we hand it.
    pub fn audio_output_stalled(&self) -> bool {
        if self.output_health.is_stalled() {
            return true;
        }
        // file playback has its own clock, driven by what the device
        // actually consumed, so a stalled local sink shows up as a position
        // that stops moving rather than as a quiet timeline.
        if self.local_playback_active.load(Ordering::Acquire) {
            return false;
        }
        self.timeline
            .lock()
            .map(|timeline| timeline.is_stalled())
            .unwrap_or(false)
    }

    /// rebuilds every audio output this core owns, keeping the spotify session.
    pub async fn restart_audio_output(&self) -> Result<(), String> {
        // whatever was loading belongs to the output being thrown away
        self.begin_playback_request();
        self.output_health.clear();
        self.clear_local_playback();
        stop_cached_local_sink();
        self.lock_state()?.local_sink = None;

        let (session, mixer, stream_bitrate) = {
            let state = self.lock_state()?;
            (
                state.session.clone(),
                state.mixer.clone(),
                state.stream_bitrate,
            )
        };
        let (Some(session), Some(mixer)) = (session, mixer) else {
            // nothing streaming to rebuild. the file sink above is enough.
            return Ok(());
        };

        // drop outside the state lock because a stalled output may take time
        // to stop. stopped, not paused: a request still waiting on it keeps it
        // alive a little longer, and a paused load would start if played
        let previous = {
            let mut state = self.lock_state()?;
            (state.player.take(), state.standby_player.take())
        };
        if let Some(previous) = previous.0 {
            previous.player.stop();
            drop(previous);
        }
        if let Some(previous) = previous.1 {
            previous.player.stop();
            drop(previous);
        }

        let player_config = PlayerConfig {
            position_update_interval: Some(Duration::from_secs(1)),
            bitrate: stream_bitrate,
            ..PlayerConfig::default()
        };
        let player = self.make_player_slot(&session, &mixer, player_config.clone(), true, 1.0);
        let standby = self.make_player_slot(&session, &mixer, player_config, false, 0.0);
        let mut state = self.lock_state()?;
        state.player = Some(player);
        state.standby_player = Some(standby);
        self.reset_timeline();
        Ok(())
    }

    /// the number a new request that changes what plays starts with. taken
    /// as the request is made, in the order requests are made, rather than
    /// when its future first runs, which two workers may do the other way
    /// round
    pub fn begin_playback_request(&self) -> u64 {
        let mut ticket = 0;
        self.playback_generation.send_modify(|generation| {
            *generation += 1;
            ticket = *generation;
        });
        ticket
    }

    fn is_latest_request(&self, ticket: u64) -> bool {
        *self.playback_generation.borrow() == ticket
    }

    pub async fn play_track(&self, spotify_uri: String) -> Result<(), String> {
        let ticket = self.begin_playback_request();
        self.play_track_as(spotify_uri, ticket).await
    }

    /// plays a streamed track for the request numbered `ticket`
    /// (begin_playback_request). a newer request ends it with SUPERSEDED, at
    /// once and without touching the outputs if it has not switched them yet.
    pub async fn play_track_as(&self, spotify_uri: String, ticket: u64) -> Result<(), String> {
        let uri = Self::parse_playable_uri(&spotify_uri)?;
        let mut newer = self.playback_generation.subscribe();
        if !self.is_latest_request(ticket) {
            return Err(SUPERSEDED.to_owned());
        }
        let (active, standby) = self.player_slots()?;
        let player = Arc::clone(&active.player);
        let mut events = player.get_player_event_channel();

        {
            let _switching = self.lock_switching();
            if !self.is_latest_request(ticket) {
                return Err(SUPERSEDED.to_owned());
            }
            self.clear_local_playback();
            // the shared sink may hold a track from before a reset, which this
            // core does not know of and would leave playing under the stream
            silence_cached_local_sink();
            self.spectrum.clear();
            active.set_gain(1.0);
            if let Some(standby) = &standby {
                standby.set_gain(0.0);
                standby.player.stop();
            }
            self.reset_timeline();
            player.load(uri.clone(), true, 0);
        }

        loop {
            let event = tokio::select! {
                event = timeout(Duration::from_secs(30), events.recv()) => event
                    .map_err(|_| "timed out while loading the Spotify track".to_owned())?
                    .ok_or_else(|| "the librespot player stopped unexpectedly".to_owned())?,
                _ = newer.wait_for(|generation| *generation != ticket) => {
                    self.release_if_orphaned(&player);
                    return Err(SUPERSEDED.to_owned());
                }
            };
            if !self.is_latest_request(ticket) {
                self.release_if_orphaned(&player);
                return Err(SUPERSEDED.to_owned());
            }

            match event {
                PlayerEvent::Playing { track_id, .. } if track_id == uri => return Ok(()),
                PlayerEvent::Unavailable { track_id, .. } if track_id == uri => {
                    // previous error was incredibly misleading, so i changed it to a generic error
                    return Err("Spotify could not load this track.".to_owned());
                }
                PlayerEvent::Stopped { track_id, .. } if track_id == uri => {
                    return Err("the Spotify track stopped before playback began".to_owned());
                }
                _ => {}
            }
        }
    }

    /// prepares the active player and, when crossfade is enabled, the standby
    /// player so the next track is ready for a normal start or a handoff.
    pub fn preload_track(&self, spotify_uri: String, preload_standby: bool) -> Result<(), String> {
        let uri = Self::parse_playable_uri(&spotify_uri)?;
        let (active, standby) = self.player_slots()?;
        active.player.preload(uri.clone());
        if preload_standby && let Some(standby) = standby {
            standby.player.preload(uri);
        }
        Ok(())
    }

    /// loads the next song on the standby player's output and starts an
    /// equal-power fade between the two players once it is ready.
    pub async fn crossfade_track_as(
        self: &Arc<Self>,
        spotify_uri: String,
        duration_ms: u32,
        ticket: u64,
    ) -> Result<(), String> {
        let uri = Self::parse_playable_uri(&spotify_uri)?;
        let mut newer = self.playback_generation.subscribe();
        if !self.is_latest_request(ticket) {
            return Err(SUPERSEDED.to_owned());
        }
        if self.local_active()? {
            return Err("crossfade is unavailable for local-file playback".to_owned());
        }
        let (outgoing, incoming) = {
            let state = self.lock_state()?;
            (
                state.player.clone().ok_or_else(|| "Spotify player is not initialized".to_owned())?,
                state.standby_player.clone().ok_or_else(|| "standby Spotify player is not initialized".to_owned())?,
            )
        };
        let mut events = incoming.player.get_player_event_channel();

        {
            let _switching = self.lock_switching();
            if !self.is_latest_request(ticket) {
                return Err(SUPERSEDED.to_owned());
            }
            self.clear_local_playback();
            incoming.set_gain(0.0);
            incoming.player.load(uri.clone(), true, 0);
        }

        let mut duration = 0;
        loop {
            let event = tokio::select! {
                event = timeout(Duration::from_secs(30), events.recv()) => event
                    .map_err(|_| "timed out while loading the Spotify track".to_owned())?
                    .ok_or_else(|| "the standby Spotify player stopped unexpectedly".to_owned())?,
                _ = newer.wait_for(|generation| *generation != ticket) => {
                    incoming.set_gain(0.0);
                    incoming.player.stop();
                    return Err(SUPERSEDED.to_owned());
                }
            };
            if !self.is_latest_request(ticket) {
                incoming.set_gain(0.0);
                incoming.player.stop();
                return Err(SUPERSEDED.to_owned());
            }
            match event {
                PlayerEvent::TrackChanged { audio_item } => duration = audio_item.duration_ms,
                PlayerEvent::Playing { track_id, position_ms, .. } if track_id == uri => {
                    let mut state = self.lock_state()?;
                    if !state.player.as_ref().is_some_and(|slot| Arc::ptr_eq(&slot.player, &outgoing.player))
                        || !state.standby_player.as_ref().is_some_and(|slot| Arc::ptr_eq(&slot.player, &incoming.player))
                    {
                        incoming.set_gain(0.0);
                        incoming.player.stop();
                        return Err(SUPERSEDED.to_owned());
                    }
                    outgoing.active.store(false, Ordering::Release);
                    incoming.active.store(true, Ordering::Release);
                    state.player = Some(incoming.clone());
                    state.standby_player = Some(outgoing.clone());
                    self.spectrum.clear();
                    if let Ok(mut timeline) = self.timeline.lock() {
                        timeline.reset();
                        timeline.duration_ms = duration;
                        timeline.observe_position(position_ms, Some(true));
                    }
                    break;
                }
                PlayerEvent::Unavailable { track_id, .. } if track_id == uri => {
                    incoming.set_gain(0.0);
                    incoming.player.stop();
                    // previous error was incredibly misleading, so i changed it to a generic error
                    return Err("Spotify could not load this track.".to_owned());
                }
                PlayerEvent::Stopped { track_id, .. } if track_id == uri => {
                    incoming.set_gain(0.0);
                    incoming.player.stop();
                    return Err("the Spotify track stopped before playback began".to_owned());
                }
                _ => {}
            }
        }

        let fade_ms = duration_ms.clamp(500, 12_000);
        let core = Arc::clone(self);
        tokio::spawn(async move {
            let started = Instant::now();
            let fade_duration = Duration::from_millis(u64::from(fade_ms));
            loop {
                if !core.is_latest_request(ticket) {
                    incoming.set_gain(1.0);
                    outgoing.set_gain(0.0);
                    outgoing.player.stop();
                    return;
                }
                let progress = (started.elapsed().as_secs_f64() / fade_duration.as_secs_f64()).clamp(0.0, 1.0);
                let angle = progress * std::f64::consts::FRAC_PI_2;
                outgoing.set_gain(angle.cos());
                incoming.set_gain(angle.sin());
                if progress >= 1.0 {
                    break;
                }
                tokio::time::sleep(Duration::from_millis(20)).await;
            }
            outgoing.set_gain(0.0);
            incoming.set_gain(1.0);
            outgoing.player.stop();
        });
        Ok(())
    }

    pub fn load_track(&self, spotify_uri: String) -> Result<(), String> {
        self.clear_local_playback();
        let uri = Self::parse_playable_uri(&spotify_uri)?;

        self.spectrum.clear();
        self.reset_timeline();
        self.player()?.load(uri, true, 0);
        Ok(())
    }

    pub fn play(&self) -> Result<(), String> {
        if self.local_active()? {
            let sink = self.resolve_live_sink()?;
            if let Ok(mut timeline) = self.timeline.lock() {
                let position = timeline.position_ms;
                timeline.observe_position(position, Some(true));
            }
            sink.play();
            return Ok(());
        }
        self.player()?.play();
        Ok(())
    }

    /// pauses both outputs, not only the one in use: a stream still loading
    /// behind a file, or the other way round, must not start up
    /// and play over it
    pub fn pause(&self) -> Result<(), String> {
        let local = self.local_active()?;
        let (active, standby, sink) = {
            let state = self.lock_state()?;
            (state.player.clone(), state.standby_player.clone(), state.local_sink.clone())
        };
        if local && let Ok(mut timeline) = self.timeline.lock() {
            let position = timeline.position_ms;
            timeline.observe_position(position, Some(false));
        }
        if let Some(sink) = sink {
            sink.pause();
        }
        if let Some(active) = active {
            active.player.pause();
        } else if !local {
            return Err("there is no Spotify player to pause".to_owned());
        }
        if let Some(standby) = standby {
            standby.player.pause();
        }
        self.spectrum.clear();
        Ok(())
    }

    pub fn seek(&self, position_ms: u32) -> Result<(), String> {
        if self.local_active()? {
            return self.local_seek(position_ms);
        }
        self.player()?.seek(position_ms);
        if let Ok(mut timeline) = self.timeline.lock() {
            timeline.observe_position(position_ms, None);
        }
        Ok(())
    }

    pub fn playback_position_ms(&self) -> u32 {
        if let Ok(state) = self.lock_state()
            && let Some(local) = &state.local_playback
        {
            let duration = local.duration_ms.min(u32::MAX as u64) as u32;
            return local.position_ms().min(duration);
        }
        self.timeline
            .lock()
            .map(|timeline| timeline.current_position_ms())
            .unwrap_or(0)
    }

    pub fn playback_duration_ms(&self) -> u32 {
        if let Ok(state) = self.lock_state()
            && let Some(local) = &state.local_playback
        {
            return local.duration_ms.min(u32::MAX as u64) as u32;
        }
        self.timeline
            .lock()
            .map(|timeline| timeline.duration_ms)
            .unwrap_or(0)
    }

    pub fn take_end_of_track(&self) -> bool {
        self.timeline
            .lock()
            .map(|mut timeline| std::mem::take(&mut timeline.end_of_track))
            .unwrap_or(false)
    }

    pub fn is_playing_local(&self) -> bool {
        self.lock_state()
            .map(|state| state.local_playback.is_some())
            .unwrap_or(false)
    }

    /// spotify serves 96, 160 or 320 kbps. anything else is refused rather
    /// than silently rounded, so a bad value is visible at the call site.
    fn bitrate(kbps: u32) -> Result<Bitrate, String> {
        match kbps {
            96 => Ok(Bitrate::Bitrate96),
            160 => Ok(Bitrate::Bitrate160),
            320 => Ok(Bitrate::Bitrate320),
            other => Err(format!("unsupported bitrate: {other} kbps")),
        }
    }

    /// applies to the next player, since librespot fixes the bitrate when the
    /// player is built. reconnecting playback picks it up.
    pub fn set_stream_quality(&self, kbps: u32) -> Result<(), String> {
        let bitrate = Self::bitrate(kbps)?;
        self.lock_state()?.stream_bitrate = bitrate;
        Ok(())
    }

    /// keeps streamed songs in `path`, up to `limit_bytes`; an empty path
    /// keeps none. like the bitrate, it applies the next time playback connects
    pub fn set_audio_cache(&self, path: String, limit_bytes: u64) -> Result<(), String> {
        self.lock_state()?.audio_cache = (!path.trim().is_empty()).then_some((path, limit_bytes));
        Ok(())
    }

    /// plays what is pushed into `buffer` from now on, through the equalizer
    /// and the spectrum, until something else plays or `stop_live` is called.
    /// a buffer already playing keeps playing undisturbed.
    pub fn play_live(
        &self,
        buffer: Arc<live_input::LiveBuffer>,
        rate: u32,
        channels: u32,
    ) -> Result<(), String> {
        let sink = self.resolve_live_sink()?;
        {
            let state = self.lock_state()?;
            if state.local_playback.is_none()
                && state
                    .live
                    .as_ref()
                    .is_some_and(|(playing, cancel)| Arc::ptr_eq(playing, &buffer) && !cancel.load(Ordering::Relaxed))
                && !sink.empty()
            {
                drop(state);
                sink.play();
                return Ok(());
            }
        }
        let _switching = self.lock_switching();
        // takes over from a file, and from librespot, which must not start
        // a song it was still loading over this
        self.begin_playback_request();
        self.clear_local_playback();
        if let Ok((active, standby)) = self.player_slots() {
            active.player.stop();
            if let Some(standby) = standby {
                standby.set_gain(0.0);
                standby.player.stop();
            }
        }
        let cancel = Arc::new(AtomicBool::new(false));
        buffer.clear();
        let source = live_input::LiveSource::new(
            Arc::clone(&buffer),
            rate,
            channels,
            Arc::clone(&cancel),
            self.dsp.clone(),
            self.spectrum.clone(),
        );
        sink.stop();
        sink.clear();
        sink.append(source);
        sink.play();
        if let Some((_, previous)) = self.lock_state()?.live.replace((buffer, cancel)) {
            previous.store(true, Ordering::Relaxed);
        }
        Ok(())
    }

    /// the rate the file output runs at, opening it if it is not yet: live
    /// audio recorded at it plays without being resampled. 0 if there is none
    pub fn output_rate(&self) -> u32 {
        self.resolve_live_sink().map(|sink| sink.rate()).unwrap_or(0)
    }

    /// stops playing a live buffer, if one is
    pub fn stop_live(&self) {
        let Ok(live) = self.lock_state().map(|mut state| state.live.take()) else {
            return;
        };
        if let Some((_, cancel)) = live {
            cancel.store(true, Ordering::Relaxed);
            if let Ok(sink) = self.resolve_live_sink() {
                sink.stop();
                sink.clear();
            }
            self.spectrum.clear();
        }
    }

    /// plays a music file of the person's own, from anywhere on disk
    pub async fn play_file(&self, path: String) -> Result<(), String> {
        let ticket = self.begin_playback_request();
        self.play_file_as(path, ticket).await
    }

    pub async fn play_file_as(&self, path: String, ticket: u64) -> Result<(), String> {
        if !self.is_latest_request(ticket) {
            return Err(SUPERSEDED.to_owned());
        }
        let path = PathBuf::from(path);
        if !path.is_file() {
            return Err(format!("{} is no longer there", path.display()));
        }
        self.play_path_as(path, ticket)
    }

    fn open_decoder(
        path: &std::path::Path,
    ) -> Result<rodio::Decoder<io::BufReader<std::fs::File>>, String> {
        let file = std::fs::File::open(path)
            .map_err(|error| format!("failed to open local track: {error}"))?;
        let length = file.metadata().map(|metadata| metadata.len()).ok();
        let mut builder = rodio::Decoder::builder()
            .with_data(io::BufReader::new(file))
            .with_seekable(true);
        // the length lets an mp4 be read from its index at the end, and the
        // extension spares the probe guessing
        if let Some(length) = length {
            builder = builder.with_byte_len(length);
        }
        if let Some(extension) = path.extension().and_then(|extension| extension.to_str()) {
            builder = builder.with_hint(extension);
        }
        builder
            .build()
            .map_err(|error| format!("failed to decode local track: {error}"))
    }

    fn play_path_as(&self, path: PathBuf, ticket: u64) -> Result<(), String> {
        // opened before the outputs are touched, so a request replaced while
        // it reads the file leaves the newer one playing undisturbed
        let decoder = Self::open_decoder(&path)?;

        let _switching = self.lock_switching();
        if !self.is_latest_request(ticket) {
            return Err(SUPERSEDED.to_owned());
        }
        self.clear_local_playback();
        // stopped, not paused: a stream still loading would otherwise be
        // kept, and start when it arrives, over this track
        if let Ok((active, standby)) = self.player_slots() {
            active.player.stop();
            if let Some(standby) = standby {
                standby.set_gain(0.0);
                standby.player.stop();
            }
        }
        // a file whose header does not say how long it is (a variable rate
        // mp3 without one) is measured, so it can still be seeked
        let duration_ms = decoder
            .total_duration()
            .map(|d| d.as_millis() as u64)
            .or_else(|| local::measure_duration_ms(&path))
            .unwrap_or(0);
        let channels = decoder.channels() as u32;
        let sample_rate = decoder.sample_rate();

        let pushed_frames = Arc::new(AtomicU64::new(0));
        let cancel = Arc::new(AtomicBool::new(false));
        let source_frames = Arc::clone(&pushed_frames);
        let spectrum = self.spectrum.clone();
        let source = file_playback::LocalTrackSource::new(
            decoder,
            spectrum,
            source_frames,
            Arc::clone(&cancel),
            self.dsp.clone(),
        );

        let sink = self.resolve_live_sink()?;
        sink.stop();
        sink.clear();

        // position is derived from the count of consumed samples. rodio consumes
        // at real time while playing, so no explicit clock is required.
        sink.append(source);
        sink.play();

        {
            let mut state = self.lock_state()?;
            state.local_playback = Some(file_playback::LocalPlayback {
                path: path.clone(),
                duration_ms,
                pushed_frames,
                base_position_ms: 0,
                channels,
                sample_rate,
                cancel: Arc::clone(&cancel),
            });
        }
        self.local_playback_active.store(true, Ordering::Release);
        self.reset_timeline();
        if let Ok(mut timeline) = self.timeline.lock() {
            timeline.duration_ms = duration_ms.min(u32::MAX as u64) as u32;
            timeline.observe_position(0, Some(true));
        }

        self.spawn_local_end_watcher(cancel);
        Ok(())
    }

    pub fn set_volume(&self, volume: f32) -> Result<(), String> {
        if !volume.is_finite() {
            return Err("volume must be a finite number".to_owned());
        }
        let volume = volume.clamp(0.0, 1.0);
        let mut state = self.lock_state()?;
        // remember the desired gain even before either audio output exists.
        state.volume = volume;
        if let Some(mixer) = &state.mixer {
            mixer.set_volume((volume * VolumeCtrl::MAX_VOLUME as f32).round() as u16);
        }
        if let Some(sink) = &state.local_sink {
            sink.set_volume(volume);
        }
        Ok(())
    }

    pub fn volume(&self) -> f32 {
        self.lock_state().map(|state| state.volume).unwrap_or(0.75)
    }

    pub async fn lyrics_json(&self, spotify_uri: String) -> Result<String, String> {
        let uri = Self::parse_playable_uri(&spotify_uri)?;
        let track_id = SpotifyId::try_from(&uri)
            .map_err(|error| format!("invalid Spotify track for lyrics: {error}"))?;
        let lyrics = Lyrics::get(&self.session()?, &track_id)
            .await
            .map_err(|error| format!("Spotify lyrics are unavailable: {error}"))?;
        let payload = LyricsPayload::from(lyrics);

        serde_json::to_string(&payload)
            .map_err(|error| format!("failed to encode Spotify lyrics: {error}"))
    }

    /// a playlist's songs read through the session, as json shaped like the
    /// web api's items page. works for a playlist someone else made, which the
    /// web api no longer gives the songs of
    pub async fn playlist_json(&self, playlist_uri: String) -> Result<String, String> {
        playlist::playlist_json(&self.session()?, &playlist_uri).await
    }

    /// an artist's top songs read through the session, as json shaped like
    /// the web api's top tracks response, which spotify took away
    pub async fn artist_top_tracks_json(&self, artist_uri: String) -> Result<String, String> {
        artist::artist_top_tracks_json(&self.session()?, &artist_uri).await
    }

    pub fn spectrum_levels(&self) -> Vec<f32> {
        self.spectrum.levels()
    }

    /// `spectrum_levels` into a caller's buffer, so a per-frame poll allocates nothing.
    pub fn copy_spectrum_levels(&self, out: &mut [f32]) -> usize {
        self.spectrum.copy_levels(out)
    }

    pub fn set_spectrum_enabled(&self, enabled: bool) {
        self.spectrum.set_enabled(enabled);
    }

    /// the switch lock guards no data, so a panic while it was held leaves
    /// nothing inconsistent behind it
    fn lock_switching(&self) -> std::sync::MutexGuard<'_, ()> {
        self.switching
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// a player a request was waiting on that has since been replaced, by a
    /// rebuilt output or a new session, would play on unheard of: stop it
    fn release_if_orphaned(&self, player: &Arc<Player>) {
        let retained = self.lock_state().ok().is_some_and(|state| {
            state.player.as_ref().is_some_and(|slot| Arc::ptr_eq(&slot.player, player))
                || state.standby_player.as_ref().is_some_and(|slot| Arc::ptr_eq(&slot.player, player))
        });
        if !retained {
            player.stop();
        }
    }

    fn lock_state(&self) -> Result<std::sync::MutexGuard<'_, SpotifyState>, String> {
        self.state
            .lock()
            .map_err(|_| "spotify player state is unavailable".to_owned())
    }

    fn player(&self) -> Result<Arc<Player>, String> {
        Ok(Arc::clone(&self.player_slots()?.0.player))
    }

    fn player_slots(&self) -> Result<(PlayerSlot, Option<PlayerSlot>), String> {
        let state = self.lock_state()?;

        if state.session.is_none() {
            return Err("not connected to spotify".to_owned());
        }

        let active = state.player.clone().ok_or_else(|| "spotify player is not initialized".to_owned())?;
        Ok((active, state.standby_player.clone()))
    }

    fn session(&self) -> Result<Session, String> {
        self.lock_state()?
            .session
            .clone()
            .ok_or_else(|| "not connected to spotify".to_owned())
    }

    fn local_active(&self) -> Result<bool, String> {
        Ok(self.lock_state()?.local_playback.is_some())
    }

    fn resolve_live_sink(&self) -> Result<Arc<output::LocalOutput>, String> {
        {
            let state = self.lock_state()?;
            if let Some(sink) = &state.local_sink {
                return Ok(Arc::clone(sink));
            }
        }
        let sink = {
            let mut cached = LOCAL_AUDIO_SINK
                .lock()
                .map_err(|_| "the audio output for files is unavailable".to_owned())?;
            match cached.as_ref() {
                Some(sink) => Arc::clone(sink),
                None => {
                    let created = Arc::new(output::LocalOutput::open().ok_or_else(|| {
                        self.output_health.mark_stalled();
                        "no audio output device is available".to_owned()
                    })?);
                    *cached = Some(Arc::clone(&created));
                    created
                }
            }
        };
        let mut state = self.lock_state()?;
        sink.set_volume(state.volume);
        state.local_sink = Some(Arc::clone(&sink));
        Ok(sink)
    }

    fn clear_local_playback(&self) {
        self.local_playback_active.store(false, Ordering::Release);
        let (local, sink) = match self.lock_state() {
            Ok(mut state) => (state.local_playback.take(), state.local_sink.clone()),
            Err(_) => return,
        };
        if let Some(local) = local {
            local.cancel.store(true, Ordering::Relaxed);
        }
        if let Some(sink) = sink {
            sink.stop();
            sink.clear();
        }
    }

    fn local_seek(&self, position_ms: u32) -> Result<(), String> {
        let playback = self
            .lock_state()?
            .local_playback
            .clone()
            .ok_or_else(|| "no local playback is active".to_owned())?;
        let duration = playback.duration_ms;
        let channels = playback.channels.max(1);
        let sample_rate = playback.sample_rate.max(1);
        let target = (position_ms as u64).min(duration);

        if let Ok(mut timeline) = self.timeline.lock() {
            timeline.end_of_track = false;
        }

        let sink = self.resolve_live_sink()?;
        let was_paused = sink.is_paused();
        playback.cancel.store(true, Ordering::Relaxed);
        sink.clear();
        playback.pushed_frames.store(0, Ordering::Relaxed);

        let path = playback.path.clone();

        // re-decode and fast-forward to the target position.
        let mut decoder = Self::open_decoder(&path)?;

        if decoder.try_seek(Duration::from_millis(target)).is_err() {
            let target_samples = target * sample_rate as u64 / 1_000 * channels as u64;
            let mut skipped: u64 = 0;
            while skipped < target_samples {
                match decoder.next() {
                    Some(_) => skipped += 1,
                    None => break,
                }
            }
        }

        let pushed_frames = Arc::new(AtomicU64::new(0));
        let cancel = Arc::new(AtomicBool::new(false));
        let spectrum = self.spectrum.clone();
        let source_frames = Arc::clone(&pushed_frames);
        let source = file_playback::LocalTrackSource::new(
            decoder,
            spectrum,
            source_frames,
            Arc::clone(&cancel),
            self.dsp.clone(),
        );

        sink.append(source);
        if was_paused {
            sink.pause();
        } else {
            sink.play();
        }

        let mut state = self.lock_state()?;
        state.local_playback = Some(file_playback::LocalPlayback {
            path,
            duration_ms: duration,
            pushed_frames,
            base_position_ms: target,
            channels,
            sample_rate,
            cancel: Arc::clone(&cancel),
        });
        drop(state);
        if let Ok(mut timeline) = self.timeline.lock() {
            timeline.end_of_track = false;
            timeline.observe_position(target as u32, Some(!was_paused));
        }
        self.spawn_local_end_watcher(cancel);
        Ok(())
    }

    fn spawn_local_end_watcher(&self, cancel: Arc<AtomicBool>) {
        let sink = match self.resolve_live_sink() {
            Ok(sink) => sink,
            Err(_) => return,
        };
        let timeline = Arc::clone(&self.timeline);
        std::thread::Builder::new()
            .name("swiftify-local-end".to_owned())
            .spawn(move || {
                while !cancel.load(Ordering::Relaxed) && !sink.empty() {
                    std::thread::sleep(Duration::from_millis(200));
                }
                if cancel.load(Ordering::Relaxed) {
                    return;
                }
                // a short grace period ensures any trailing source has been flushed.
                std::thread::sleep(Duration::from_millis(100));
                if cancel.load(Ordering::Relaxed) {
                    return;
                }
                if let Ok(mut timeline) = timeline.lock() {
                    timeline.end_of_track = true;
                    timeline.is_playing = false;
                    timeline.observed_at = Some(Instant::now());
                }
                // the device stops until something else plays
                sink.pause_if_ended();
            })
            .ok();
    }

    fn reset_timeline(&self) {
        if let Ok(mut timeline) = self.timeline.lock() {
            timeline.reset();
        }
    }

    fn parse_playable_uri(spotify_uri: &str) -> Result<SpotifyUri, String> {
        let uri = SpotifyUri::from_uri(spotify_uri)
            .map_err(|error| format!("invalid spotify uri: {error}"))?;

        if !uri.is_playable() {
            return Err(format!(
                "spotify uri is not directly playable: {spotify_uri}"
            ));
        }

        Ok(uri)
    }
}

async fn monitor_player_events(
    mut events: PlayerEventChannel,
    timeline: Arc<Mutex<PlaybackTimeline>>,
    local_playback_active: Arc<AtomicBool>,
    slot_active: Arc<AtomicBool>,
) {
    while let Some(event) = events.recv().await {
        // the connected librespot player remains alive while a file of the
        // person's own is playing. ignore its late position/end events so they cannot advance
        // the local playback queue after a seek.
        if local_playback_active.load(Ordering::Acquire) || !slot_active.load(Ordering::Acquire) {
            continue;
        }
        let Ok(mut timeline) = timeline.lock() else {
            continue;
        };
        if local_playback_active.load(Ordering::Acquire) || !slot_active.load(Ordering::Acquire) {
            continue;
        }

        match event {
            PlayerEvent::Loading { position_ms, .. } => {
                timeline.observe_position(position_ms, Some(false));
            }
            PlayerEvent::Playing { position_ms, .. }
            | PlayerEvent::PositionChanged { position_ms, .. }
            | PlayerEvent::PositionCorrection { position_ms, .. } => {
                timeline.observe_position(position_ms, Some(true));
            }
            PlayerEvent::Paused { position_ms, .. } => {
                timeline.observe_position(position_ms, Some(false));
            }
            PlayerEvent::Seeked { position_ms, .. } => {
                timeline.observe_position(position_ms, None);
            }
            PlayerEvent::TrackChanged { audio_item } => {
                timeline.duration_ms = audio_item.duration_ms;
            }
            PlayerEvent::EndOfTrack { .. } => {
                timeline.position_ms = timeline.duration_ms;
                timeline.is_playing = false;
                timeline.end_of_track = true;
                timeline.observed_at = Some(Instant::now());
            }
            PlayerEvent::Stopped { .. } => {
                timeline.is_playing = false;
                timeline.observed_at = Some(Instant::now());
            }
            _ => {}
        }
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct LyricsPayload {
    language: String,
    provider: String,
    sync_type: String,
    lines: Vec<LyricsLinePayload>,
    colors: LyricsColorsPayload,
}

impl From<Lyrics> for LyricsPayload {
    fn from(lyrics: Lyrics) -> Self {
        Self {
            language: lyrics.lyrics.language,
            provider: lyrics.lyrics.provider_display_name,
            sync_type: match lyrics.lyrics.sync_type {
                SyncType::Unsynced => "unsynced".to_owned(),
                SyncType::LineSynced => "lineSynced".to_owned(),
            },
            lines: lyrics
                .lyrics
                .lines
                .into_iter()
                .map(|line| LyricsLinePayload {
                    start_time_ms: line.start_time_ms.parse().unwrap_or(0),
                    end_time_ms: line.end_time_ms.parse().unwrap_or(0),
                    words: line.words,
                })
                .collect(),
            colors: LyricsColorsPayload {
                background: lyrics.colors.background,
                text: lyrics.colors.text,
                highlight_text: lyrics.colors.highlight_text,
            },
        }
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct LyricsLinePayload {
    start_time_ms: u32,
    end_time_ms: u32,
    words: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct LyricsColorsPayload {
    background: i32,
    text: i32,
    highlight_text: i32,
}

#[cfg(test)]
mod tests {
    use std::time::{Duration, Instant};

    use super::{PlaybackTimeline, SUPERSEDED, SpotifyCore};

    #[test]
    fn a_quiet_player_stops_inventing_progress_and_reports_the_stall() {
        let mut timeline = PlaybackTimeline {
            duration_ms: 300_000,
            ..PlaybackTimeline::default()
        };
        timeline.observe_position(10_000, Some(true));
        assert!(!timeline.is_stalled());

        // a position update every second keeps it honest.
        timeline.observed_at = Instant::now().checked_sub(Duration::from_secs(1));
        assert!(!timeline.is_stalled());
        assert!(timeline.current_position_ms() >= 11_000);

        // nothing for far longer than librespot's update interval: the player
        // thread has stopped, and the clock must not run on without it.
        timeline.observed_at = Instant::now().checked_sub(Duration::from_secs(45));
        assert!(timeline.is_stalled());
        assert_eq!(
            timeline.current_position_ms(),
            10_000 + PlaybackTimeline::STALL_TIMEOUT.as_millis() as u32
        );
    }

    #[test]
    fn a_paused_player_is_never_reported_as_stalled() {
        let mut timeline = PlaybackTimeline::default();
        timeline.observe_position(10_000, Some(false));
        timeline.observed_at = Instant::now().checked_sub(Duration::from_secs(600));

        assert!(!timeline.is_stalled());
        assert_eq!(timeline.current_position_ms(), 10_000);
    }

    #[test]
    fn a_fresh_core_has_a_healthy_output() {
        let core = SpotifyCore::new();
        assert!(!core.audio_output_stalled());
    }

    #[test]
    fn volume_is_remembered_before_connecting_and_after_disconnect() {
        let core = SpotifyCore::new();
        core.set_volume(0.08).unwrap();
        assert_eq!(core.volume(), 0.08);
        core.disconnect();
        assert_eq!(core.volume(), 0.08);
        core.set_volume(0.0).unwrap();
        assert_eq!(core.volume(), 0.0);
        core.set_volume(2.0).unwrap();
        assert_eq!(core.volume(), 1.0);
        assert!(core.set_volume(f32::NAN).is_err());
        assert!(core.set_volume(f32::INFINITY).is_err());
        assert_eq!(core.volume(), 1.0);
    }

    #[test]
    fn player_commands_require_a_connection() {
        let core = SpotifyCore::new();

        assert_eq!(core.play().unwrap_err(), "not connected to spotify");
        // pause reaches for both outputs, and with neither there is nothing
        assert_eq!(
            core.pause().unwrap_err(),
            "there is no Spotify player to pause"
        );
        assert_eq!(core.seek(1_000).unwrap_err(), "not connected to spotify");
    }

    #[test]
    fn rejects_invalid_spotify_uris() {
        let core = SpotifyCore::new();

        assert!(core.load_track("not-a-spotify-uri".to_owned()).is_err());
    }

    #[tokio::test]
    async fn awaited_playback_requires_a_connection() {
        let core = SpotifyCore::new();

        assert_eq!(
            core.play_track("spotify:track:2WUy2Uywcj5cP0IXQagO3z".to_owned())
                .await
                .unwrap_err(),
            "not connected to spotify"
        );
    }

    #[tokio::test]
    async fn a_playlist_is_read_through_the_session() {
        let core = SpotifyCore::new();

        assert_eq!(
            core.playlist_json("spotify:playlist:37i9dQZF1DXcBWIGoYBM5M".to_owned())
                .await
                .unwrap_err(),
            "not connected to spotify"
        );
    }

    #[tokio::test]
    async fn an_artists_top_songs_are_read_through_the_session() {
        let core = SpotifyCore::new();

        assert_eq!(
            core.artist_top_tracks_json("spotify:artist:4tZwfgrHOc3mvqYlEYSvVi".to_owned())
                .await
                .unwrap_err(),
            "not connected to spotify"
        );
    }

    #[tokio::test]
    async fn a_replaced_request_gives_up_before_touching_the_outputs() {
        // two songs chosen in quick succession: the first one's request is
        // run after the second's was made, as two workers may run them
        let core = SpotifyCore::new();
        let first = core.begin_playback_request();
        let second = core.begin_playback_request();
        assert!(second > first);

        assert_eq!(
            core.play_track_as("spotify:track:2WUy2Uywcj5cP0IXQagO3z".to_owned(), first)
                .await
                .unwrap_err(),
            SUPERSEDED
        );
        assert_eq!(
            core.play_file_as("/nowhere/song.flac".to_owned(), first)
                .await
                .unwrap_err(),
            SUPERSEDED
        );
        // the newer one is still the one that counts
        assert_eq!(
            core.play_track_as("spotify:track:2WUy2Uywcj5cP0IXQagO3z".to_owned(), second)
                .await
                .unwrap_err(),
            "not connected to spotify"
        );
    }

    #[test]
    fn rebuilding_the_output_or_the_session_replaces_any_request() {
        let core = SpotifyCore::new();
        let request = core.begin_playback_request();
        core.disconnect();
        assert!(!core.is_latest_request(request));
    }
}
