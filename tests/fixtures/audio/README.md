# Audio test fixtures

`startup.ogg` is a 0.25-second Vorbis encoding derived from the
project-owned `assets/audio/startup.wav`. It exists solely to exercise
LamaPon's built-in OGG decoder in automated tests.

`nonfinite.wav` contains a short, deliberately invalid IEEE float PCM signal
with NaN and infinity values. It is an original negative fixture for the native
audio decoder; it must never become a playing voice.
