-- Schema 6 to 7. Runs with foreign keys off, after the version 6 turns and documents tables
-- are renamed turns_v6 and documents_v6 and the version 7 tables are created. Every
-- document has a revision above 0 by then.

INSERT INTO consultations (id, started_at, ended_at, state, sample_rate, device_id,
                           device_name, dropped_frames, saved, sample)
SELECT id, started_at, ended_at, state, sample_rate, device_id,
       device_name, lost_frames, retain, demo
FROM sessions;

INSERT INTO consultation_keys (consultation_id, wrapped_key)
SELECT session_id, wrapped FROM session_keys;

INSERT INTO audio_chunks (consultation_id, sequence, first_frame, frame_count,
                          dropped_before, encrypted_audio)
SELECT session_id, seq, first_frame, frame_count, lost_before, payload FROM chunks;

INSERT INTO turns (consultation_id, sequence, first_frame, frame_count, encrypted_turn)
SELECT session_id, seq, first_frame, frame_count, payload FROM turns_v6;

-- The note's options move onto its row. Detail 'standard' was renamed 'concise'
INSERT INTO documents (consultation_id, kind, revision, language, encrypted_text,
                       style, detail, generated_at, edited_at)
SELECT d.session_id, d.kind, d.seq, d.language, d.payload,
       CASE WHEN o.style IN ('prose', 'soap') THEN o.style END,
       CASE WHEN o.detail IN ('concise', 'standard') THEN 'concise'
            WHEN o.detail = 'detailed' THEN 'detailed' END,
       d.generated_at, d.edited_at
FROM documents_v6 d
LEFT JOIN note_options o ON o.session_id = d.session_id AND d.kind = 'note';

DROP TABLE note_options;
DROP TABLE documents_v6;
DROP TABLE turns_v6;
DROP TABLE chunks;
DROP TABLE session_keys;
DROP TABLE sessions;
