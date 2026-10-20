"""Data catalog and persistence management for ztOS Music."""
import os
from typing import Dict, List, Optional
import ztos

MUSIC_DIR = os.path.expanduser("~/.local/share/ztos/music")

CURATED_TRACKS: Dict[str, List[dict]] = {
    "top": [
        {"id": "UtF6Jej8yb4", "title": "The Nights", "artist": "Avicii", "duration": 176, "category": "top"},
        {"id": "IcrbM1l_BoI", "title": "Wake Me Up", "artist": "Avicii", "duration": 252, "category": "top"},
        {"id": "4NRXx6U8ABQ", "title": "Blinding Lights", "artist": "The Weeknd", "duration": 200, "category": "top"},
        {"id": "34Na4j8AVgA", "title": "Starboy", "artist": "The Weeknd ft. Daft Punk", "duration": 230, "category": "top"},
        {"id": "JGwWNGJdvx8", "title": "Shape of You", "artist": "Ed Sheeran", "duration": 233, "category": "top"},
        {"id": "H5v3k2izfcI", "title": "As It Was", "artist": "Harry Styles", "duration": 167, "category": "top"},
        {"id": "TUVcZfQe-Kw", "title": "Levitating", "artist": "Dua Lipa", "duration": 203, "category": "top"},
        {"id": "KTJ3Azx6GQQ", "title": "STAY", "artist": "The Kid LAROI, Justin Bieber", "duration": 141, "category": "top"},
    ],
    "pop": [
        {"id": "WRz2MxhAdJo", "title": "Without You", "artist": "Avicii ft. Sandro Cavazza", "duration": 182, "category": "pop"},
        {"id": "_ovdm2yX4MA", "title": "Levels", "artist": "Avicii", "duration": 200, "category": "pop"},
        {"id": "dY2M0-sQWf0", "title": "Espresso", "artist": "Sabrina Carpenter", "duration": 175, "category": "pop"},
        {"id": "ic8j13piAhQ", "title": "Cruel Summer", "artist": "Taylor Swift", "duration": 178, "category": "pop"},
        {"id": "G7KNmW9a75Y", "title": "Flowers", "artist": "Miley Cyrus", "duration": 200, "category": "pop"},
        {"id": "d5gxZgmCeAU", "title": "BIRDS OF A FEATHER", "artist": "Billie Eilish", "duration": 196, "category": "pop"},
        {"id": "ZsJ-BHpH0lo", "title": "vampire", "artist": "Olivia Rodrigo", "duration": 219, "category": "pop"},
        {"id": "OiC1rgCPmUQ", "title": "Dance The Night", "artist": "Dua Lipa", "duration": 176, "category": "pop"},
    ],
    "hiphop": [
        {"id": "H58vbez_m4E", "title": "Not Like Us", "artist": "Kendrick Lamar", "duration": 274, "category": "hiphop"},
        {"id": "xpVfcZ0ZcFM", "title": "God's Plan", "artist": "Drake", "duration": 198, "category": "hiphop"},
        {"id": "tvTRZJ-4EyI", "title": "HUMBLE.", "artist": "Kendrick Lamar", "duration": 177, "category": "hiphop"},
        {"id": "6ONRf7h3Mdk", "title": "SICKO MODE", "artist": "Travis Scott", "duration": 312, "category": "hiphop"},
        {"id": "mzB1V9R-FeM", "title": "Lucid Dreams", "artist": "Juice WRLD", "duration": 239, "category": "hiphop"},
        {"id": "UTHLK9MaDYM", "title": "INDUSTRY BABY", "artist": "Lil Nas X, Jack Harlow", "duration": 212, "category": "hiphop"},
        {"id": "22tVWwmTie8", "title": "Houdini", "artist": "Eminem", "duration": 227, "category": "hiphop"},
    ],
    "rnb": [
        {"id": "LDY_XZroNi8", "title": "Snooze", "artist": "SZA", "duration": 201, "category": "rnb"},
        {"id": "m7OWT0Ni54Y", "title": "Kill Bill", "artist": "SZA", "duration": 153, "category": "rnb"},
        {"id": "u9n7Cw-4_PE", "title": "Die For You", "artist": "The Weeknd", "duration": 260, "category": "rnb"},
        {"id": "tQ0yjYUFKAE", "title": "Peaches", "artist": "Justin Bieber ft. Daniel Caesar", "duration": 198, "category": "rnb"},
        {"id": "Kp7eSUU9485", "title": "Redbone", "artist": "Childish Gambino", "duration": 326, "category": "rnb"},
        {"id": "uzS3WG6__G4", "title": "Pink + White", "artist": "Frank Ocean", "duration": 184, "category": "rnb"},
    ],
}


def ensure_music_dir() -> str:
    os.makedirs(MUSIC_DIR, exist_ok=True)
    return MUSIC_DIR


def get_local_file(track_id: str) -> Optional[str]:
    """Finds downloaded audio file for a given track id."""
    ensure_music_dir()
    for ext in ("m4a", "webm", "opus", "mp3", "ogg"):
        p = os.path.join(MUSIC_DIR, f"{track_id}.{ext}")
        if os.path.isfile(p) and os.path.getsize(p) > 1024:
            return p
    return None


def is_saved(track_id: str) -> bool:
    return get_local_file(track_id) is not None


def get_saved_tracks() -> List[dict]:
    store = ztos.store("library")
    saved_map = store.get("saved_tracks", {})
    ensure_music_dir()
    result = []
    for tid, info in list(saved_map.items()):
        fpath = get_local_file(tid)
        if fpath:
            track = dict(info)
            track["local_path"] = fpath
            track["is_saved"] = True
            result.append(track)
        else:
            saved_map.pop(tid, None)
    if len(saved_map) != len(store.get("saved_tracks", {})):
        store["saved_tracks"] = saved_map
        store.save()
    return result


def save_track_meta(track: dict, file_path: str):
    store = ztos.store("library")
    saved_map = store.get("saved_tracks", {})
    clean = dict(track)
    clean["local_path"] = file_path
    clean["is_saved"] = True
    saved_map[track["id"]] = clean
    store["saved_tracks"] = saved_map
    store.save()


def remove_saved_track(track_id: str):
    store = ztos.store("library")
    saved_map = store.get("saved_tracks", {})
    saved_map.pop(track_id, None)
    store["saved_tracks"] = saved_map
    store.save()

    fpath = get_local_file(track_id)
    if fpath and os.path.exists(fpath):
        try:
            os.remove(fpath)
        except OSError:
            pass


def get_history() -> List[dict]:
    store = ztos.store("library")
    return store.get("history", [])


def add_to_history(track: dict):
    store = ztos.store("library")
    hist = store.get("history", [])
    hist = [t for t in hist if t.get("id") != track.get("id")]
    clean = dict(track)
    clean["is_saved"] = is_saved(track.get("id", ""))
    hist.insert(0, clean)
    store["history"] = hist[:40]
    store.save()
