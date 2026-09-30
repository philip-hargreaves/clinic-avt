-- ClinicAVT consultation store, schema version 7.
--
-- Audio, turns and document text are sealed with AES-256-GCM under a key per consultation.
-- Each seal authenticates the domain, the consultation id and the sequence. Deleting the key
-- row leaves the consultation's ciphertext unreadable. All other columns are plaintext.
-- Times are ISO 8601 UTC.

CREATE TABLE consultations (
    id              TEXT    PRIMARY KEY,              -- 32 lowercase hex digits
    started_at      TEXT    NOT NULL,
    ended_at        TEXT,
    state           TEXT    NOT NULL CHECK (state IN ('recording', 'finalised')),
    sample_rate     INTEGER NOT NULL,
    device_id       TEXT,                             -- the microphone used
    device_name     TEXT,
    dropped_frames  INTEGER NOT NULL DEFAULT 0,       -- by the device or a failed disk write
    saved           INTEGER NOT NULL CHECK (saved IN (0, 1)),
    sample          INTEGER NOT NULL DEFAULT 0 CHECK (sample IN (0, 1)),  -- 1 when seeded
    CHECK (state = 'recording' OR ended_at IS NOT NULL)
);

CREATE TABLE consultation_keys (
    consultation_id TEXT    PRIMARY KEY REFERENCES consultations (id) ON DELETE CASCADE,
    wrapped_key     BLOB    NOT NULL                  -- wrapped with DPAPI for the user
);

-- Kept only while recording, to resume after a crash
CREATE TABLE audio_chunks (
    consultation_id TEXT    NOT NULL REFERENCES consultations (id) ON DELETE CASCADE,
    sequence        INTEGER NOT NULL,
    first_frame     INTEGER NOT NULL,
    frame_count     INTEGER NOT NULL,
    dropped_before  INTEGER NOT NULL,
    encrypted_audio BLOB    NOT NULL,                 -- float32 samples
    PRIMARY KEY (consultation_id, sequence)
);

CREATE TABLE turns (
    consultation_id TEXT    NOT NULL REFERENCES consultations (id) ON DELETE CASCADE,
    sequence        INTEGER NOT NULL,
    first_frame     INTEGER NOT NULL,
    frame_count     INTEGER NOT NULL,
    encrypted_turn  BLOB    NOT NULL,                 -- JSON speaker and text
    PRIMARY KEY (consultation_id, sequence)
);

-- At most one document of each kind per consultation
CREATE TABLE documents (
    consultation_id TEXT    NOT NULL REFERENCES consultations (id) ON DELETE CASCADE,
    kind            TEXT    NOT NULL CHECK (kind IN ('note', 'patient', 'translation', 'label',
                                                     'summary', 'reflection', 'guidance')),
    revision        INTEGER NOT NULL CHECK (revision > 0),  -- sequence the text is sealed at
    language        TEXT    NOT NULL,                 -- a language name for a translation, else en
    encrypted_text  BLOB    NOT NULL,
    style           TEXT    CHECK (style IN ('prose', 'soap')),
    detail          TEXT    CHECK (detail IN ('concise', 'detailed')),
    generated_at    TEXT,                             -- for a reflection, when it was created
    edited_at       TEXT,
    PRIMARY KEY (consultation_id, kind),
    CHECK (kind = 'note' OR (style IS NULL AND detail IS NULL))
);
