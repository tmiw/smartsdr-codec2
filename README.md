smartsdr-codec2 - A SmartSDR Waveform for the FreeDV Protocols
============

This code implements a SmartSDR "Waveform" supporting the FreeDV family of protocols.  It uses the freedv_api code to
implement the demodulation and decoding of the voice signals.  Currently supported are the following modes:

* 1600
* 700C
* 700D
* 800XA

The other modes could be relatively easily enabled, but haven't been tested well.

Please note that the Flex6000 series radios do not have a large amount of processing power on the ARM core inside of
them.  This plays poorly with the fact that 700C tends to be a rather heavyweight mode processing-wise.  It may not
be possible to run this mode on the radio, although further optimization may be attempted.

Notes on building the waveform
------------------------------

The source uses `cmake` (http://cmake.org) to power the build process.  The included cmake configuration will
automatically download and build the two required components, `codec2` and `libsoxr`.  We do this for you for a couple
of different reasons:

1)  Both of these libraries must be built statically and linked into the resulting binary for them to work on the
radio.  You cannot install additional libraries on the system in a supported manner, and the executable must be
self contained.  Note that the resulting binary is **not** statically linked.  It still dynamically links to the C library
and friends.
2)  Getting the companion libraries compiled up using the cross-compilation tools can be tricky and problematic.
Particularly, `codec2` has a support program that must run on the host to create the codetables necessary to build
into the library.  This means you have to set up cross-compilation correctly or the codetables generator will be compiled
for the target and will refuse to run during the build process.
3)  Getting the companion libraries configured to have the correct features is easier to code for than to extensively
document.

Creating the toolchain
----------------------

Included in this distribution is a toolchain configuration file for `crosstools-ng` (https://crosstool-ng.github.io).
This should allow you to get up and running with a crosscompiler more quickly and easily.  Follow the [installation
instructions](https://crosstool-ng.github.io/docs/install/) to get `ct-ng` installed and functional.

Once `ct-ng` is installed, you can build a toolchain using the following commands:

```asm
cd smartsdr-codec2/extras
cp ct-ng.config .config
ct-ng oldocnfig
ct-ng build
```

This will create a toolchain at `~/x-tools/flex6k/`.  To use it for our purposes, you must add `~/x-tools/flex6k/bin`
to your path so CMake can find it as appropriate.

Compiling for ARM
-----------------

To compile for arm, you'll need to make sure you use the proper CMake build type.  There are two of them, one for
Debug and one for Release.  They are named ARMDebug and ARMRelease respectively.  To compile for Debug on ARM use
the following commands.  Once again **make sure you have the cross-compilation tools built in the previous section
in your `$PATH` or this will not work**.

```asm
cd smartsdr-codec2/DSP_API
mkdir cmake-build-armdebug
cd cmake-build-armdebug
cmake -DCMAKE_BUILD_TYPE=ARMDebug ..
make
```

Sit back for a minute or two and you'll have a fresh `freedv` executable.

Compiling Natively
------------------

If you'd like a native executable for testing and exploration, this is entirely possible.  We will assume that you
already have the compiler and associated tools installed in your distribution of choice (`build-essentials` or its 
equivalent).  The following commands should get you a debug executable.

```asm
cd smartsdr-code2/DSP_API
mkdir cmake-build-debug
cd cmake-build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
```