# Recording fixtures

One-second clips in each format the import accepts, for `media_foundation_reader_test.cpp` and the client's `ImportContractTest.cs`. Each is stereo with a 440 Hz tone at 0.125 on the left channel and silence on the right, so the decoded mono carries the tone at half its level (the downmix averages the channels). All are 44.1 kHz except `tone.wav`, which is 8 kHz so one clip is upsampled rather than downsampled. `tone.m4a` carries a creation time of 2026-09-26T13:05:00Z; the others carry no date.

Made with ffmpeg 8.1.2, from this folder, in bash:

```sh
IN="-f lavfi -i sine=frequency=440:sample_rate=44100:duration=1 -f lavfi -i anullsrc=r=44100:cl=mono -filter_complex [0:a][1:a]amerge=inputs=2[a] -map [a] -t 1"
ffmpeg -y $IN -c:a aac -b:a 32k -metadata creation_time=2026-09-26T13:05:00Z tone.m4a
ffmpeg -y $IN -c:a libmp3lame -q:a 6 tone.mp3
ffmpeg -y $IN -c:a wmav2 -b:a 32k tone.wma
ffmpeg -y $IN -c:a flac tone.flac
ffmpeg -y $IN -c:a aac -b:a 32k -f adts tone.aac
ffmpeg -y $IN -ar 8000 -c:a pcm_s16le tone.wav
```
