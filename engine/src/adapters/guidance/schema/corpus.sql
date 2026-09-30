-- ClinicAVT guidance corpus. One file per corpus, built as corpus.db.tmp by the
-- indexer and renamed into corpora/<id>/corpus.db beside manifest.json.
-- Public or licensed reference text, never patient data, so nothing is sealed.
-- Immutable once built: rollback journal, opened read-only. page_size 65536.
-- application_id 0x414D4247 ("AMBG"). user_version 1 is the format.

-- One row. manifest.json duplicates the key fields and adds the file hash
CREATE TABLE corpus_meta (
    id            INTEGER PRIMARY KEY CHECK (id = 1),
    corpus_id     TEXT    NOT NULL,                 -- "nice-2026-08-25", carries the fetch date
    name          TEXT    NOT NULL,
    licence       TEXT    NOT NULL,
    attribution   TEXT    NOT NULL,
    source        TEXT    NOT NULL CHECK (source IN ('nice', 'text')),
    embedder_id   TEXT    NOT NULL,                 -- model store id, "gte-large-int8"
    embedder_rev  TEXT    NOT NULL,                 -- sha256 of the model weights
    chunk_prefix  TEXT    NOT NULL DEFAULT '',      -- unused, always ''
    query_prefix  TEXT    NOT NULL DEFAULT '',      -- unused, always ''
    max_tokens    INTEGER NOT NULL,                 -- truncation length at index time
    dim           INTEGER NOT NULL CHECK (dim > 0),
    vector_format TEXT    NOT NULL CHECK (vector_format = 'f32le'),
    normalised    INTEGER NOT NULL CHECK (normalised = 1),  -- unit vectors: cosine is a dot product
    chunk_count   INTEGER NOT NULL CHECK (chunk_count >= 0),
    shard_count   INTEGER NOT NULL CHECK (shard_count >= 0),
    built_at      TEXT    NOT NULL,                 -- ISO 8601 UTC
    builder       TEXT    NOT NULL                  -- indexer version
);

-- One recommendation or paragraph run. ord is dense 0..chunk_count-1 and is
-- the row of the in-memory matrix
CREATE TABLE chunks (
    ord           INTEGER PRIMARY KEY,
    chunk_id      TEXT    NOT NULL UNIQUE,          -- "ng100-1_1_1", the id the panel shows
    code          TEXT    NOT NULL,                 -- "ng100"
    title         TEXT    NOT NULL,
    chapter       TEXT    NOT NULL DEFAULT '',
    number        TEXT    NOT NULL DEFAULT '',
    section       TEXT    NOT NULL DEFAULT '',
    update_tag    TEXT    NOT NULL DEFAULT '',
    last_updated  TEXT    NOT NULL DEFAULT '',
    url           TEXT    NOT NULL DEFAULT '',
    text          TEXT    NOT NULL                  -- verbatim, displayed, never generated from
);

-- Vectors in shards of consecutive ordinals, ~1 MB each. dim is repeated so the
-- length check needs only the row
CREATE TABLE guidance_vectors (
    shard         INTEGER PRIMARY KEY,
    first_ord     INTEGER NOT NULL CHECK (first_ord >= 0),
    count         INTEGER NOT NULL CHECK (count > 0),
    dim           INTEGER NOT NULL CHECK (dim > 0),
    data          BLOB    NOT NULL CHECK (length(data) = count * dim * 4)
);
