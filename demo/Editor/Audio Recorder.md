---
tags: [demo, editor, audio-recorder]
---

# Audio recorder

What to look for: "Audio recorder: Start recording audio" in the command
palette records the default microphone into a file in the vault, and
"Audio recorder: Stop recording audio" ends it and puts a player for it in the
note, at the caret. Settings > Core plugins > "Audio recorder" (on by default)
removes the commands; nothing touches the microphone until you start a
recording. It needs a microphone and a recording format Qt Multimedia can
write here (webm, as Obsidian does, else ogg, else m4a). Without them, or in a
build without Qt Multimedia, starting says "Cannot record:" with the reason in
the status bar, for example "No microphone, or no format this machine can
record", and creates nothing.

## Try it

1. Put the caret on the empty line below, in this note, in live preview.
2. Run "Audio recorder: Start recording audio". The status bar shows
   "Recording".
3. Speak for a few seconds, then run "Audio recorder: Stop recording audio".
4. A line `![[Recording 20261019120000.webm]]` (the name is the date and time
   it started, in the file's real container) appears at the caret and
   becomes the same player card the audio embed shows in
   [[Media Embeds]]: play and pause, a seek bar and a clock.

The recording is a normal attachment. It is saved where Settings puts
attachments: in this vault that is `Embeds/assets` (from `.obsidian/app.json`),
beside `bell.oga`. Delete the file and the line to tidy up.



## Edge cases

- Starting twice does nothing the second time; one recording at a time.
- Stop before you start does nothing.
- If you switch to another note while recording, the player is still put into
  the note the recording started from, where its caret was; if that note has
  been closed meanwhile, the status bar says "Recording saved as" and the
  file's path, and nothing is inserted.
- A recording that fails (the device went away) says "The recording failed:"
  and the reason, and inserts nothing.
