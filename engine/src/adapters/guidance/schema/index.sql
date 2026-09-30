-- ClinicAVT added-documents index, format 4. A cache of the clinician's guidelines folder,
-- deleted and rebuilt on a format or embedder change. Plaintext, since every text here is
-- already a file in that folder. application_id 0x414D4249 ("AMBI").

-- One row
CREATE TABLE index_metadata (
    id                 INTEGER PRIMARY KEY CHECK (id = 1),
    embedder_id        TEXT    NOT NULL,            -- "gte-large-int8"
    embedder_rev       TEXT    NOT NULL,            -- sha256 of the model weights
    dim                INTEGER NOT NULL CHECK (dim > 0),
    max_tokens         INTEGER NOT NULL
);

-- One row per content. id is the first 63 bits of the sha256, so the same bytes at any path
-- share a row and a changed file gets a new one
CREATE TABLE guideline_documents (
    id                 INTEGER PRIMARY KEY,
    sha256             TEXT    NOT NULL UNIQUE CHECK (length(sha256) = 64),
    mime               TEXT    NOT NULL,
    state              TEXT    NOT NULL CHECK (state IN ('indexing', 'ready', 'failed')),
    error              TEXT,                        -- a reason code, never content
    added_at           TEXT    NOT NULL,
    indexed_at         TEXT,
    pages              INTEGER,
    pages_without_text INTEGER
);

-- The paths holding each document. A document left with no path is deleted
CREATE TABLE guideline_files (
    path               TEXT    NOT NULL PRIMARY KEY,  -- relative to the folder
    document_id        INTEGER NOT NULL REFERENCES guideline_documents (id) ON DELETE CASCADE,
    size               INTEGER NOT NULL,
    modified           INTEGER NOT NULL             -- last write time in file clock ticks
);
CREATE INDEX guideline_files_by_document ON guideline_files (document_id);

CREATE TABLE passages (
    document_id        INTEGER NOT NULL REFERENCES guideline_documents (id) ON DELETE CASCADE,
    position           INTEGER NOT NULL,
    page               INTEGER NOT NULL,
    number             TEXT    NOT NULL DEFAULT '',  -- the document's own numbering
    section            TEXT    NOT NULL DEFAULT '',  -- the heading above
    text               TEXT    NOT NULL,
    vector             BLOB    NOT NULL,            -- float32 unit vector of dim values
    line_boxes         TEXT    NOT NULL,            -- JSON, as fractions of the page
    PRIMARY KEY (document_id, position)
);
