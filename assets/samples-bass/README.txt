Thirteen bass samples (supplied by the project owner, Ben Mathers), built into the firmware as SAMPLE sets 21..33 (factory
presets, the BASS category): one sample pitched across the whole keyboard, root note from its file name.

  01_808    808 G           one-shot   root G1 (31)
  02_808D   808 dist G      one-shot   root G1
  03_BASS1  Bass 1 E        loop       root E1 (28)
  04_BASS2  Bass 2 C        one-shot   root C2 (36)
  05_REESD  Reese dist G    loop       root G1
  06_SUBA   akai sub A      loop       root A1 (33)
  07_BASS3  bass 3 A        one-shot   root A1
  08_DONK   donk F          one-shot   root F1 (29)
  09_DRED   dred bass C     one-shot   root C2
  10_DREDR  dred bass raw C one-shot   root C2
  11_REESE  reese C         loop       root C2
  12_SDROP  sub drop G      one-shot   root G1
  13_SFALL  sub fall        one-shot   no note in the name: root B1 (35), the pitch it starts on

tools/gen_samples.py (BASS_META) turns them into 4-bit ADPCM at 11,025 Hz to fit the flash: a one-shot is cut to a length of
0.33 .. 1.2 s with a fade-out; a loop keeps 0.12 s of attack and about 0.4 s of a crossfaded loop (the best-matching end).
Stereo float files are fine as sources (any WAV tools/sampleio.py reads).
