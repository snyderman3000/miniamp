# MiniAmp 0.2.3 test build (Mini v4 sound)

`MiniAmp-miyoo-v0.2.3-test.zip` is a test build for the Miyoo Mini v4, where
MiniAmp was silent inside the app (music in games worked).

MiniAmp now plays through OnionOS's own sound library (`miyoo/lib/libpadsp.so`),
the one the menu and every game use, instead of the firmware's copy in
`/customer/lib`, which doesn't seem to work with OnionOS's sound service on the v4.

To try it: copy `App/MiniAmp` from the zip over the one on the SD card, open
MiniAmp and play a song. If it's still silent, please send
`App/MiniAmp/log.txt` (the `audio: LD_PRELOAD=...` line shows which library was used).

This build isn't released yet; it isn't offered as an update in Mixtape.
