# Run Fuchsia Emulator (FEMU)

You can run Fuchsia under emulation using the Fuchsia Emulator (FEMU).

FEMU is downloaded by `jiri` as part of `jiri update` or `jiri run-hooks`, and
is fetched into `//prebuilt/third_party/aemu`. You can run FEMU using `fx emu`
(see section ["Run Fuchsia under FEMU"](#run_fuchsia_under_femu)).

## Prerequisites

In order to run FEMU, you must do the following:

 * [Installed Fuchsia source and created environment variables](/docs/get-started/get_fuchsia_source.md)
 * [Configured and built Fuchsia](/docs/get-started/build_fuchsia.md)
 * [Set up and configured FEMU](/docs/get-started/set_up_femu.md)

## Run Fuchsia under FEMU

Ensure that you have set up and built the Fuchsia product (this example uses
`workstation` product). Currently only `qemu-x64` boards are supported.

```posix-terminal
cd $FUCHSIA_DIR

fx set workstation.qemu-x64 --release [--with=...]

fx build
```

Then run Fuchsia Emulator with ssh access:

```posix-terminal
cd $FUCHSIA_DIR

fx emu -N
```

To exit FEMU, run `dm poweroff` in the FEMU terminal.

### Run FEMU without GUI

If you don't need graphics or working under the remote workflow,
you can run FEMU in headless mode by adding `--headless` argument to the
`fx emu` command, for example:

```posix-terminal
cd $FUCHSIA_DIR

fx emu -N --headless
```

### Specify GPU used by FEMU

By default, FEMU tries using the host GPU automatically if it is available,
and will fall back to software rendering using
[SwiftShader](https://swiftshader.googlesource.com/SwiftShader/) if host GPU is
unavailable.

You can also add argument `--host-gpu` or `--software-gpu` to `fx emu` command
to enforce FEMU to use a specific graphics device.

### Develop remotely

There are multiple ways to develop Fuchsia remotely using FEMU, and all the
workflow supports GPU acceleration without requiring GPU drivers and runtime
on the local machine.

#### Remote Desktop

These instructions require remote access to the remote machine using remote
desktop tools like [Chrome Remote Desktop](https://remotedesktop.google.com/).

Follow general remote desktop instructions for logging in to your remote machine.
Once logged in, follow standard Fuchsia development instructions for building
Fuchsia, and use the following command to run Fuchsia in the emulator inside
your remote desktop session:

```posix-terminal
cd $FUCHSIA_DIR

fx emu
```

The emulator will use Swiftshader instead of real GPU hardware for GPU
acceleration when running inside Chrome Remote Desktop. Performance will be
worse than using real GPU hardware but the benefit is that it runs on any
workstation.

#### `fx emu-remote` Command

These instructions work for local machines with macOS or Linux, and require SSH
access to a Linux workstation capable of building Fuchsia.

On the terminal of your local machine, type the following to build, fetch the
artifacts and start the emulator with networking:

```posix-terminal
cd $FUCHSIA_DIR

fx emu-remote {{ '<var>REMOTE-WORKSTATION-NAME</var>' }}  -- -N
```

Alternatively, start the emulator on remote workstation, and open an WebRTC
connection to it using local browser.

On the terminal of your local machine, type the following to start the emulator
with networking on {{ '<var>REMOTE-WORKSTATION-NAME</var>' }} and have the
output forwarded using WebRTC to a Chrome tab on local machine:

```posix-terminal
cd $FUCHSIA_DIR

fx emu-remote --stream {{ '<var>REMOTE-WORKSTATION-NAME</var>' }} -- -N
```

This by default uses software rendering and GPU acceleration is supported by
using an existing X server on the remote machine with access to GPU hardware:

```posix-terminal
cd $FUCHSIA_DIR

fx emu-remote --stream --display :0 {{ '<var>REMOTE-WORKSTATION-NAME</var>' }} -- -N
```

Any arguments after “--” will be passed to the fx emu invocation on the remote
machine.

