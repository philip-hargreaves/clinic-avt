-- Added-documents index, a cache of the guidelines folder, rebuilt on a format or embedder
-- mismatch. Plaintext, since every text here is already a file in the user's folder

CREATE TABLE index_meta (
    id            INTEGER PRIMARY KEY CHECK (id = 1),
    embedder_id   TEXT    NOT NULL,                 -- model store id, "gte-large-int8"
    embedder_rev  TEXT    NOT NULL,                 -- sha256 of the model weights
    dim           INTEGER NOT NULL CHECK (dim > 0),
    query_prefix  TEXT    NOT NULL DEFAULT '',      -- unused, always ''
    max_tokens    INTEGER NOT NULL,
    created_at    TEXT    NOT NULL                  -- ISO 8601 UTC
);

-- One row per content. id is the first 63 bits of the sha256, so identical
-- bytes at any path share a row and a changed file gets a new one
CREATE TABLE documents (
    id            INTEGER PRIMARY KEY,
    sha256        TEXT    NOT NULL UNIQUE CHECK (length(sha256) = 64),
    mime          TEXT    NOT NULL,
    state         TEXT    NOT NULL CHECK (state IN ('indexing', 'ready', 'failed')),
    error         TEXT,                             -- a reason code, never content
    added_at      TEXT    NOT NULL,
    indexed_at    TEXT,                             -- NULL until ready
    pages         INTEGER,
    pages_without_text INTEGER
);

-- Folder paths holding a document. Several may share one, and a document with no path is
-- deleted
CREATE TABLE files (
    path          TEXT    NOT NULL PRIMARY KEY,     -- relative to the folder, UTF-8
    document_id   INTEGER NOT NULL REFERENCES documents (id) ON DELETE CASCADE,
    size          INTEGER NOT NULL,
    modified      INTEGER NOT NULL  -- last write time in file clock ticks, compared for equality
);
CREATE INDEX files_by_document ON files (document_id);

CREATE TABLE chunks (
    document_id   INTEGER NOT NULL REFERENCES documents (id) ON DELETE CASCADE,
    ord           INTEGER NOT NULL,
    page          INTEGER NOT NULL,
    number        TEXT    NOT NULL DEFAULT '',      -- the document's own numbering
    section       TEXT    NOT NULL DEFAULT '',      -- the heading above
    text          TEXT    NOT NULL,                 -- verbatim, displayed, never generated from
    vector        BLOB    NOT NULL,                 -- f32 unit vector, dim floats
    boxes         TEXT    NOT NULL,                 -- line boxes as page fractions, JSON
    PRIMARY KEY (document_id, ord)
) WITHOUT ROWID;
