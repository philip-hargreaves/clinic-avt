-- ClinicAVT guidance corpus, format 2. One read-only file per corpus, at
-- corpora/<id>/corpus.db beside manifest.json. Public or licensed text with no
-- patient data, so nothing is sealed. application_id 0x414D4247 ("AMBG").

-- One row
CREATE TABLE corpus_metadata (
    id            INTEGER PRIMARY KEY CHECK (id = 1),
    corpus_id     TEXT    NOT NULL,                 -- the folder name, "nice-2026-08-25"
    name          TEXT    NOT NULL,
    licence       TEXT    NOT NULL,
    attribution   TEXT    NOT NULL,
    source        TEXT    NOT NULL CHECK (source IN ('nice', 'text')),  -- the importer
    embedder_id   TEXT    NOT NULL,                 -- "gte-large-int8"
    embedder_rev  TEXT    NOT NULL,                 -- sha256 of the model weights
    max_tokens    INTEGER NOT NULL,                 -- passages were cut to this length
    dim           INTEGER NOT NULL CHECK (dim > 0),
    built_at      TEXT    NOT NULL,                 -- ISO 8601 UTC
    builder       TEXT    NOT NULL                  -- indexer version
);

-- One recommendation or run of paragraphs. Positions run from 0 without gaps, one per row of
-- the vector matrix
CREATE TABLE passages (
    position      INTEGER PRIMARY KEY,
    passage_id    TEXT    NOT NULL UNIQUE,          -- "ng100-1_1_1"
    code          TEXT    NOT NULL,                 -- the guideline, "ng100"
    title         TEXT    NOT NULL,
    number        TEXT    NOT NULL DEFAULT '',
    section       TEXT    NOT NULL DEFAULT '',
    update_tag    TEXT    NOT NULL DEFAULT '',
    last_updated  TEXT    NOT NULL DEFAULT '',
    url           TEXT    NOT NULL DEFAULT '',
    text          TEXT    NOT NULL
);

-- Unit vectors of dim float32 values, little-endian, for consecutive positions
CREATE TABLE vector_shards (
    shard          INTEGER PRIMARY KEY,
    first_position INTEGER NOT NULL CHECK (first_position >= 0),
    passage_count  INTEGER NOT NULL CHECK (passage_count > 0),
    vectors        BLOB    NOT NULL
);
