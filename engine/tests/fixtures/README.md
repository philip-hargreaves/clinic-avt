# Test fixtures

| Folder | Holds | Source |
|---|---|---|
| `archive/` | A version 1 backup file | Written by the engine |
| `storage/` | The store's schema, as the snapshot test expects it | Written by the engine |
| `guidance/` | Four invented guidelines, six notes with the ids each should retrieve, and three reference embeddings | Written for the tests, with no publisher's text |
| `recordings/` | A one-second 440 Hz tone in each format the import accepts | Made with ffmpeg |
| `transcription/` | The reference transcript of one PriMock57 consultation | PriMock57 |
| `diarisation/` | Audio features and expected outputs for six PriMock57 clips | PriMock57 |

The recordings are stereo with the tone on the left channel only. All are 44.1 kHz except
`tone.wav`, which is 8 kHz. `tone.m4a` carries a creation time and the others do not.

PriMock57 is by Babylon Health (Papadopoulos Korfiatis et al., 2022), used under CC BY 4.0:
https://github.com/babylonhealth/primock57
