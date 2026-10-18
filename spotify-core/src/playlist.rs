//! a playlist's songs read through the session, the way spotify's own apps
//! read them. since february 2026 the web api only gives the name and cover of
//! a playlist someone else made, so its songs come from here instead.
//!
//! the result is json shaped like the web api's `/playlists/{id}/items` page,
//! so the front end reads both the same way.

use std::collections::HashMap;

use http::Method;
use librespot::{
    core::{Session, SpotifyId, SpotifyUri},
    protocol::{
        extended_metadata::{BatchedEntityRequest, EntityRequest, ExtensionQuery},
        extension_kind::ExtensionKind,
        metadata::{self, image::Size as ImageSize},
        playlist4_external::SelectedListContent,
    },
};
use protobuf::{EnumOrUnknown, Message};
use serde::Serialize;
use tokio::task::JoinSet;

/// asked for per request. spotify may send the whole list at once anyway
const PAGE_LENGTH: usize = 500;
/// tracks looked up per extended metadata request
const METADATA_BATCH: usize = 100;

pub async fn playlist_json(session: &Session, playlist_uri: &str) -> Result<String, String> {
    let id = match SpotifyUri::from_uri(playlist_uri) {
        Ok(SpotifyUri::Playlist { id, .. }) => id,
        _ => return Err(format!("not a spotify playlist: {playlist_uri}")),
    };
    let uris = item_uris(session, &id).await?;
    let tracks = track_metadata(session, &uris).await?;
    let items = uris
        .iter()
        .filter_map(|uri| tracks.get(uri))
        .map(|track| ItemPayload { item: track.clone() })
        .collect();
    serde_json::to_string(&PagePayload { items })
        .map_err(|error| format!("failed to encode the playlist: {error}"))
}

/// every item's uri, in the playlist's order
async fn item_uris(session: &Session, id: &SpotifyId) -> Result<Vec<String>, String> {
    let base62 = id
        .to_base62()
        .map_err(|error| format!("invalid playlist id: {error}"))?;
    let mut uris = Vec::new();
    loop {
        let endpoint = format!(
            "/playlist/v2/playlist/{base62}?from={}&length={PAGE_LENGTH}",
            uris.len()
        );
        let bytes = session
            .spclient()
            .request(&Method::GET, &endpoint, None, None)
            .await
            .map_err(|error| format!("Spotify would not send the playlist: {error}"))?;
        let list = SelectedListContent::parse_from_bytes(&bytes)
            .map_err(|error| format!("the playlist could not be read: {error}"))?;
        if !append_page(&mut uris, &list) {
            return Ok(uris);
        }
    }
}

/// adds a page's items, and says whether there are more to ask for. a page
/// that does not start where it was asked to is the whole list again
fn append_page(uris: &mut Vec<String>, list: &SelectedListContent) -> bool {
    let contents = list.contents.get_or_default();
    if usize::try_from(contents.pos()).ok() != Some(uris.len()) {
        return false;
    }
    let before = uris.len();
    uris.extend(contents.items.iter().map(|item| item.uri().to_owned()));
    let length = usize::try_from(list.length()).unwrap_or(0);
    uris.len() > before && uris.len() < length
}

/// each track's details by its uri. episodes and local files are left out,
/// as the web api path leaves them out
pub(crate) async fn track_metadata(
    session: &Session,
    uris: &[String],
) -> Result<HashMap<String, TrackPayload>, String> {
    let mut wanted: Vec<&String> = uris
        .iter()
        .filter(|uri| uri.starts_with("spotify:track:"))
        .collect();
    wanted.sort_unstable();
    wanted.dedup();

    let mut batches = JoinSet::new();
    for batch in wanted.chunks(METADATA_BATCH) {
        let request = BatchedEntityRequest {
            entity_request: batch
                .iter()
                .map(|uri| EntityRequest {
                    entity_uri: (*uri).clone(),
                    query: vec![ExtensionQuery {
                        extension_kind: EnumOrUnknown::new(ExtensionKind::TRACK_V4),
                        ..Default::default()
                    }],
                    ..Default::default()
                })
                .collect(),
            ..Default::default()
        };
        let session = session.clone();
        batches.spawn(async move { session.spclient().get_extended_metadata(request).await });
    }

    let mut tracks = HashMap::new();
    while let Some(joined) = batches.join_next().await {
        let response = joined
            .map_err(|error| format!("looking up the playlist's songs failed: {error}"))?
            .map_err(|error| format!("Spotify would not describe the playlist's songs: {error}"))?;
        for array in &response.extended_metadata {
            for data in &array.extension_data {
                let Some(any) = data.extension_data.as_ref() else {
                    continue;
                };
                let Ok(track) = metadata::Track::parse_from_bytes(&any.value) else {
                    continue;
                };
                if let Some(payload) = TrackPayload::new(&data.entity_uri, &track) {
                    tracks.insert(data.entity_uri.clone(), payload);
                }
            }
        }
    }
    Ok(tracks)
}

#[derive(Serialize)]
struct PagePayload {
    items: Vec<ItemPayload>,
}

#[derive(Serialize)]
struct ItemPayload {
    item: TrackPayload,
}

#[derive(Clone, Serialize)]
pub(crate) struct TrackPayload {
    #[serde(rename = "type")]
    kind: &'static str,
    uri: String,
    name: String,
    duration_ms: i32,
    explicit: bool,
    artists: Vec<ArtistPayload>,
    album: Option<AlbumPayload>,
}

#[derive(Clone, Serialize)]
struct ArtistPayload {
    id: String,
    uri: String,
    name: String,
}

#[derive(Clone, Serialize)]
struct AlbumPayload {
    id: String,
    uri: String,
    name: String,
    artists: Vec<ArtistPayload>,
    images: Vec<ImagePayload>,
}

#[derive(Clone, Serialize)]
struct ImagePayload {
    url: String,
    width: i32,
    height: i32,
}

impl TrackPayload {
    /// none for a track spotify sent back without a name, which it does for
    /// one it no longer has
    fn new(uri: &str, track: &metadata::Track) -> Option<Self> {
        if track.name().is_empty() {
            return None;
        }
        Some(Self {
            kind: "track",
            uri: uri.to_owned(),
            name: track.name().to_owned(),
            duration_ms: track.duration(),
            explicit: track.explicit(),
            artists: track.artist.iter().filter_map(ArtistPayload::new).collect(),
            album: track.album.as_ref().and_then(AlbumPayload::new),
        })
    }
}

impl ArtistPayload {
    fn new(artist: &metadata::Artist) -> Option<Self> {
        let uri = SpotifyUri::try_from(artist).ok()?;
        Some(Self {
            id: uri.to_id().ok()?,
            uri: uri.to_uri().ok()?,
            name: artist.name().to_owned(),
        })
    }
}

impl AlbumPayload {
    fn new(album: &metadata::Album) -> Option<Self> {
        let uri = SpotifyUri::try_from(album).ok()?;
        let covers = match album.cover_group.as_ref() {
            Some(group) if !group.image.is_empty() => &group.image,
            _ => &album.cover,
        };
        Some(Self {
            id: uri.to_id().ok()?,
            uri: uri.to_uri().ok()?,
            name: album.name().to_owned(),
            artists: album.artist.iter().filter_map(ArtistPayload::new).collect(),
            images: covers.iter().filter_map(ImagePayload::new).collect(),
        })
    }
}

impl ImagePayload {
    fn new(image: &metadata::Image) -> Option<Self> {
        let file_id = image.file_id();
        if file_id.is_empty() {
            return None;
        }
        let hex: String = file_id.iter().map(|byte| format!("{byte:02x}")).collect();
        // the sizes spotify's image server sends when the width is left out
        let side = match image.size() {
            ImageSize::SMALL => 64,
            ImageSize::DEFAULT => 300,
            ImageSize::LARGE => 640,
            ImageSize::XLARGE => 1280,
        };
        Some(Self {
            url: format!("https://i.scdn.co/image/{hex}"),
            width: image.width.unwrap_or(side),
            height: image.height.unwrap_or(side),
        })
    }
}

#[cfg(test)]
mod tests {
    use librespot::protocol::{
        metadata::{Album, Artist, Image, ImageGroup, Track},
        playlist4_external::{Item, ListItems, SelectedListContent},
    };
    use protobuf::{EnumOrUnknown, MessageField};

    use super::{ImageSize, TrackPayload, append_page};

    fn page(pos: i32, length: i32, uris: &[&str]) -> SelectedListContent {
        SelectedListContent {
            length: Some(length),
            contents: MessageField::some(ListItems {
                pos: Some(pos),
                items: uris
                    .iter()
                    .map(|uri| Item {
                        uri: Some((*uri).to_owned()),
                        ..Default::default()
                    })
                    .collect(),
                ..Default::default()
            }),
            ..Default::default()
        }
    }

    #[test]
    fn pages_are_followed_until_the_list_is_whole() {
        let mut uris = Vec::new();
        assert!(append_page(&mut uris, &page(0, 3, &["a", "b"])));
        assert!(!append_page(&mut uris, &page(2, 3, &["c"])));
        assert_eq!(uris, ["a", "b", "c"]);
    }

    #[test]
    fn a_page_from_the_start_again_ends_the_list() {
        let mut uris = Vec::new();
        assert!(append_page(&mut uris, &page(0, 3, &["a", "b"])));
        assert!(!append_page(&mut uris, &page(0, 3, &["a", "b"])));
        assert_eq!(uris, ["a", "b"]);
    }

    #[test]
    fn an_empty_page_ends_the_list() {
        let mut uris = vec!["a".to_owned()];
        assert!(!append_page(&mut uris, &page(1, 5, &[])));
        assert_eq!(uris, ["a"]);
    }

    #[test]
    fn a_track_reads_like_the_web_api_one() {
        let artist = Artist {
            gid: Some(vec![1; 16]),
            name: Some("Sade".to_owned()),
            ..Default::default()
        };
        let track = Track {
            gid: Some(vec![3; 16]),
            name: Some("Smooth Operator".to_owned()),
            duration: Some(298_000),
            explicit: Some(false),
            artist: vec![artist.clone()],
            album: MessageField::some(Album {
                gid: Some(vec![2; 16]),
                name: Some("Diamond Life".to_owned()),
                artist: vec![artist],
                cover_group: MessageField::some(ImageGroup {
                    image: vec![Image {
                        file_id: Some(vec![0xab; 20]),
                        size: Some(EnumOrUnknown::new(ImageSize::LARGE)),
                        ..Default::default()
                    }],
                    ..Default::default()
                }),
                ..Default::default()
            }),
            ..Default::default()
        };

        let payload = TrackPayload::new("spotify:track:x", &track).unwrap();
        let json = serde_json::to_value(&payload).unwrap();
        assert_eq!(json["type"], "track");
        assert_eq!(json["uri"], "spotify:track:x");
        assert_eq!(json["name"], "Smooth Operator");
        assert_eq!(json["duration_ms"], 298_000);
        assert_eq!(json["artists"][0]["name"], "Sade");
        assert!(
            json["artists"][0]["uri"]
                .as_str()
                .unwrap()
                .starts_with("spotify:artist:")
        );
        assert_eq!(json["album"]["name"], "Diamond Life");
        assert!(
            json["album"]["uri"]
                .as_str()
                .unwrap()
                .starts_with("spotify:album:")
        );
        assert_eq!(
            json["album"]["images"][0]["url"],
            format!("https://i.scdn.co/image/{}", "ab".repeat(20))
        );
        assert_eq!(json["album"]["images"][0]["width"], 640);
    }

    #[test]
    fn a_track_spotify_no_longer_has_is_left_out() {
        assert!(TrackPayload::new("spotify:track:x", &Track::default()).is_none());
    }
}
