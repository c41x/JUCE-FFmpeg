# JUCE-FFmpeg
FFmpeg reader for JUCE.

## Installation
* Copy files from `JUCE` catalog into JUCE source
* Go to `modules/juce_audio_formats/juce_audio_formats.cpp/h` and include reader
files: `#include "codecs/juce_FFmpegAudioFormat.cpp"` and `
#include "codecs/juce_FFmpegAudioFormat.h"` respectively.
* Do not forget to add FFmpeg include / library paths to your project. It
depends on your configuration / operating system so I wont go into details here.

## Supported formats
This supports all formats available for libavcodec. Writing files (encoding) is
not supported

## Known issues
* Seeking is not sample-perfect. Seeking is quite hard to do properly in FFmpeg.
For simple playback this is not great issue.