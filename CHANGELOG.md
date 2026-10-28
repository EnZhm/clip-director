# Changelog

Clip Director for OBS Studio 32.2.x, Windows x64. Part of the
[Director](https://github.com/EnZhm/director) suite.

This repository's first release is 1.1.0. The 1.0.0 build was made before it and is
listed here for completeness; its files are not republished.

## 1.1.0

- Synesthesia scene control over OSC: link chip, host and ports, Groove / Breakdown /
  Drop scene pools, Next scene / Hold scene / Fire bang 1, Import from Synesthesia, and
  a log of every message with its beat position and reason. OSC needs a Synesthesia Pro
  licence.
- Scene launches land on the same beat as the clip transition they pair with: the start
  of the breakdown crossfade and the drop downbeat.
- New dock **MIDI Director**: learn mode maps MIDI notes and CCs to OBS hotkeys across
  the suite (Palette Director, Music, Clip Director, Synesthesia).
- Windows-only packaging: releases ship the Windows x64 zip. The Ubuntu and macOS
  packaging jobs are gone; the core unit tests still run on Ubuntu.

## 1.0.0

- First release: the Clip Director source (clip folders, beat-aligned cuts, deep
  shuffle, level evening, brightness inversion), the Clip Director dock, and the ingest
  tool `tools/cd-ingest.py`.
