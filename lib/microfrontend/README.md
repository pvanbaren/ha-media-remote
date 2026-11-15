# microfrontend

The audio front end of TensorFlow Lite Micro: it turns 16 kHz audio into the
40-channel spectrogram features that microWakeWord's models take, 10 ms at a
time. The model itself, and the interpreter that runs it, come from the
framework (`espressif__esp-tflite-micro`, linked into every ESP32-S3 build);
this front end is the one part of it that the framework does not include.

`src/tensorflow/lite/experimental/microfrontend/lib/` is that directory of
https://github.com/tensorflow/tflite-micro at commit
`d4fd5bf48af18eee01558b3c5a94dca827e70362`, less its tests, its file I/O and
its command-line tools. Apache License 2.0, in `LICENSE`.

One file is changed: `kiss_fft_int16.h` takes kissfft's 16-bit build from
the framework's TensorFlow Lite Micro, which already compiles it, instead of
compiling kissfft's sources a second time here. `kiss_fft_int16.cc` is left
out for the same reason. The change is marked at the top of the file.

Two files are this project's own. `library.json` force-includes
`src/microfrontend_psram.h` into every file of the library, and that header
points the library's `malloc`, `calloc` and `free` at PSRAM first: the front
end's buffers are small enough that the framework would otherwise put them
in internal RAM, about 12 KB of it.

Used by `src/services/wake_word.cpp`.
