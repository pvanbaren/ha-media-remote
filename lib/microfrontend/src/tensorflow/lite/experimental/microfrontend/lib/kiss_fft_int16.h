/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// MODIFIED for this project (see lib/microfrontend/README.md): kissfft's
// 16-bit build is taken from the framework's TensorFlow Lite Micro, whose
// signal library already compiles it as kiss_fft_fixed16, rather than from a
// copy of kissfft's sources compiled again here as kissfft_fixed16.

#ifndef TENSORFLOW_LITE_EXPERIMENTAL_MICROFRONTEND_LIB_KISS_FFT_INT16_H_
#define TENSORFLOW_LITE_EXPERIMENTAL_MICROFRONTEND_LIB_KISS_FFT_INT16_H_

#include "signal/src/kiss_fft_wrappers/kiss_fft_int16.h"

namespace kissfft_fixed16 = kiss_fft_fixed16;

#undef kiss_fft_scalar
#undef KISS_FFT_H

#endif  // TENSORFLOW_LITE_EXPERIMENTAL_MICROFRONTEND_LIB_KISS_FFT_INT16_H_
