//! an artist's top songs read through the session. spotify took the web api's
//! top tracks call away in february 2026.
//!
//! the result is json shaped like the web api's `/artists/{id}/top-tracks`
//! response, so the front end reads it the same way.

use librespot::{
    core::{Session, SpotifyUri},
    protocol::metadata::{self, TopTracks},
};
use protobuf::Message;
use serde::Serialize;

use crate::playlist::{TrackPayload, track_metadata};

/// as many as the web api gave
const TOP_TRACK_COUNT: usize = 10;

pub async fn artist_top_tracks_json(session: &Session, artist_uri: &str) -> Result<String, String> {
    let uri = match SpotifyUri::from_uri(artist_uri) {
        Ok(uri @ SpotifyUri::Artist { .. }) => uri,
        _ => return Err(format!("not a spotify artist: {artist_uri}")),
    };
    let bytes = session
        .spclient()
        .get_artist_metadata(&uri)
        .await
        .map_err(|error| format!("Spotify would not send the artist: {error}"))?;
    let artist = metadata::Artist::parse_from_bytes(&bytes)
        .map_err(|error| format!("the artist could not be read: {error}"))?;
    let uris = top_track_uris(&artist.top_track, &session.country());
    let found = track_metadata(session, &uris).await?;
    let tracks = uris
        .iter()
        .filter_map(|uri| found.get(uri).cloned())
        .collect();
    serde_json::to_string(&TopTracksPayload { tracks })
        .map_err(|error| format!("failed to encode the top songs: {error}"))
}

/// the list for the account's country, else the one for everywhere, else the
/// first there is
fn top_track_uris(lists: &[TopTracks], country: &str) -> Vec<String> {
    let list = lists
        .iter()
        .find(|list| list.country() == country)
        .or_else(|| lists.iter().find(|list| list.country().is_empty()))
        .or_else(|| lists.first());
    let Some(list) = list else {
        return Vec::new();
    };
    list.track
        .iter()
        .filter_map(|track| SpotifyUri::try_from(track).ok()?.to_uri().ok())
        .take(TOP_TRACK_COUNT)
        .collect()
}

#[derive(Serialize)]
struct TopTracksPayload {
    tracks: Vec<TrackPayload>,
}

#[cfg(test)]
mod tests {
    use librespot::protocol::metadata::{TopTracks, Track};

    use super::top_track_uris;

    fn list(country: &str, ids: &[u8]) -> TopTracks {
        TopTracks {
            country: Some(country.to_owned()),
            track: ids
                .iter()
                .map(|id| Track {
                    gid: Some(vec![*id; 16]),
                    ..Default::default()
                })
                .collect(),
            ..Default::default()
        }
    }

    #[test]
    fn the_accounts_country_comes_first() {
        let lists = [list("", &[1]), list("SE", &[2]), list("DE", &[3])];
        let uris = top_track_uris(&lists, "DE");
        assert_eq!(uris.len(), 1);
        assert_eq!(uris, top_track_uris(&[list("", &[3])], "US"));
    }

    #[test]
    fn another_country_falls_back_to_everywhere_then_the_first() {
        let lists = [list("SE", &[2]), list("", &[1])];
        assert_eq!(
            top_track_uris(&lists, "DE"),
            top_track_uris(&[list("", &[1])], "DE")
        );
        assert_eq!(
            top_track_uris(&[list("SE", &[2])], "DE"),
            top_track_uris(&lists, "SE")
        );
        assert!(top_track_uris(&[], "DE").is_empty());
    }

    #[test]
    fn only_ten_are_kept_as_track_uris() {
        let ids: Vec<u8> = (1..=12).collect();
        let uris = top_track_uris(&[list("", &ids)], "DE");
        assert_eq!(uris.len(), 10);
        assert!(uris.iter().all(|uri| uri.starts_with("spotify:track:")));
    }
}
