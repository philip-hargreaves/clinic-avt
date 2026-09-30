-- ClinicAVT session store. One database, one writer thread.
--
-- Content (audio, transcript, documents) is AES-256-GCM under a per-session key.
-- The cipher authenticates domain, session id and sequence, so a blob moved
-- between rows or sessions is detected. Metadata (timing, state, options) is
-- plaintext so it can be queried without a key. Deleting the key row makes the
-- session's content unreadable.

CREATE TABLE sessions (
    id           TEXT    PRIMARY KEY,             -- random 128-bit hex
    started_at   TEXT    NOT NULL,                -- ISO 8601 UTC
    ended_at     TEXT,                            -- NULL while recording or after a crash
    state        TEXT    NOT NULL
                 CHECK (state IN ('recording', 'finalised')),
    sample_rate  INTEGER NOT NULL,
    device_id    TEXT,                            -- capture device at the time, a snapshot
    device_name  TEXT,
    lost_frames  INTEGER NOT NULL DEFAULT 0,      -- frames the device dropped
    retain       INTEGER NOT NULL DEFAULT 1,      -- 0: erased once the consultation is left
    demo         INTEGER NOT NULL DEFAULT 0       -- 1: a seeded sample, never a real record
);

CREATE TABLE session_keys (
    session_id   TEXT    PRIMARY KEY REFERENCES sessions (id) ON DELETE CASCADE,
    wrapped      BLOB    NOT NULL                 -- AES key, DPAPI-wrapped for the user
);

-- Audio write-ahead log, committed every second while recording, used to resume after a
-- crash and erased when the transcript is sealed
CREATE TABLE chunks (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    seq          INTEGER NOT NULL,                -- also the nonce sequence
    first_frame  INTEGER NOT NULL,                -- position on the session timeline
    frame_count  INTEGER NOT NULL,
    lost_before  INTEGER NOT NULL,                -- frames dropped before this chunk
    payload      BLOB    NOT NULL,                -- sealed float32 frames, domain 0
    PRIMARY KEY (session_id, seq)
);

-- The transcript: live turns during capture, replaced in one transaction by
-- the speaker-attributed turns at finalise
CREATE TABLE turns (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    seq          INTEGER NOT NULL,
    first_frame  INTEGER NOT NULL,
    frame_count  INTEGER NOT NULL,
    payload      BLOB    NOT NULL,                -- sealed {speaker, text}, domain 1
    PRIMARY KEY (session_id, seq)
);

-- One text of each kind per session; a rewrite replaces it
CREATE TABLE documents (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    kind         TEXT    NOT NULL
                 CHECK (kind IN ('note', 'patient', 'translation', 'label',
                                 'summary', 'reflection', 'guidance')),
    seq          INTEGER NOT NULL DEFAULT 0,      -- nonce sequence of this payload; a rewrite adds one
    language     TEXT    NOT NULL,                -- BCP 47
    payload      BLOB    NOT NULL,                -- sealed text, domain 2..8 by kind
    generated_at TEXT,                            -- when the model wrote it
    edited_at    TEXT,                            -- NULL until a person changed it
    PRIMARY KEY (session_id, kind)
);

-- Options the note was written with, one row per note document
CREATE TABLE note_options (
    session_id   TEXT    PRIMARY KEY,
    kind         TEXT    NOT NULL DEFAULT 'note' CHECK (kind = 'note'),
    style        TEXT    NOT NULL,                -- prose | soap
    detail       TEXT    NOT NULL,                -- concise | standard | detailed
    FOREIGN KEY (session_id, kind) REFERENCES documents (session_id, kind) ON DELETE CASCADE
);
