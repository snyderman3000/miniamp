MiniAmp for Miyoo Mini Plus (OnionOS)
https://github.com/snyderman3000/miniamp

A music player in the style of the classic skinnable desktop players. It plays
MP3, FLAC, Ogg Vorbis and WAV, loads classic .wsz skins, and has a 10-band
equalizer and full-screen visualizers. MiniAmp is an independent project and
is not affiliated with Winamp or its owners.

Install: copy the App folder to the root of the SD card (or install it with
Mixtape). Launch "MiniAmp" from Apps.

Music: put it anywhere on the SD card (Media/Music is a good place) and press
B to browse. A plays a folder starting at a song, Y plays a whole folder
including subfolders, X adds to the playlist. .m3u playlists work too.

Controls
  A              play the selected song (or pause it)
  START          play / pause
  L / R          previous / next song
  Left / Right   seek (hold to go faster)
  Up / Down      move in the playlist (L2 / R2 page)
  B              music library
  Y              equalizer (Up/Down level, Left/Right band, A on/off, X presets)
  X              full-screen visualizer (Left/Right change effect)
  SELECT / MENU  options: screen off, shuffle, repeat, sleep timer, volume,
                 skins, quit

Screen off: Options > Turn screen off. Music keeps playing; START pauses,
L / R skip, any other button turns the screen back on. (The power button
puts the whole handheld to sleep, which stops the music.)

Music in games: Options > Music in games. With it on, quit MiniAmp while a
song is playing and the music keeps going in the menu, in games and in other
apps. While it plays in the background:
  SELECT + L / R      previous / next song
  SELECT + Up / Down  volume
  SELECT + START      pause / resume
Opening MiniAmp again picks up the same song. Apps that make their own sound
some other way pause the music until you leave them.
How it works: MiniAmp swaps OnionOS's sound library (miyoo/lib/libpadsp.so)
for a version that blends the music into the sound of whatever is playing; the
original is kept as libpadsp_orig.so. Turning the option off puts the original
back - do that before deleting MiniAmp. If an OnionOS update replaces the file,
MiniAmp sets it up again the next time the music goes to the background.

Skins: copy classic .wsz skin files into MiniAmp/skins, then pick one in
Options > Skin. Thousands are at https://skins.webamp.org.

Settings and the last playlist live in MiniAmp/data. The log of the last run
is MiniAmp/log.txt; please attach it to bug reports.

Licenses for everything in this package are in the licenses folder.
