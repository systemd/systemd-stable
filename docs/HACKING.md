---
title: Hacking on systemd
category: Contributing
layout: default
SPDX-License-Identifier: LGPL-2.1-or-later
---

# Hacking on systemd

We welcome all contributions to systemd. If you notice a bug or a missing
feature, please feel invited to fix it, and submit your work as a
[GitHub Pull Request (PR)](https://github.com/systemd/systemd/pull/new).

Please make sure to follow our [Coding Style](/CODING_STYLE) when submitting
patches. Also have a look at our [Contribution Guidelines](/CONTRIBUTING).

When adding new functionality, tests should be added. For shared functionality
(in `src/basic/` and `src/shared/`) unit tests should be sufficient. The general
policy is to keep tests in matching files underneath `src/test/`,
e.g. `src/test/test-path-util.c` contains tests for any functions in
`src/basic/path-util.c`. If adding a new source file, consider adding a matching
test executable. For features at a higher level, tests in `src/test/` are very
strongly recommended. If that is not possible, integration tests in `test/` are
encouraged.

Please also have a look at our list of [code quality tools](/CODE_QUALITY) we
have setup for systemd, to ensure our codebase stays in good shape.

Please always test your work before submitting a PR. For many of the components
of systemd testing is straightforward as you can simply compile systemd and
run the relevant tool from the build directory.

For some components (most importantly, systemd/PID 1 itself) this is not
possible, however. In order to simplify testing for cases like this we provide
a set of `mkosi` build files directly in the source tree.
[mkosi](https://github.com/systemd/mkosi) is a tool for building clean OS images
from an upstream distribution in combination with a fresh build of the project
in the local working directory. To make use of this, please install `mkosi` v19
or newer using your distribution's package manager or from the
[GitHub repository](https://github.com/systemd/mkosi). `mkosi` will build an
image for the host distro by default. First, run `mkosi genkey` to generate a key
and certificate to be used for secure boot and verity signing. After that is done,
it is sufficient to type `mkosi` in the systemd project directory to generate a disk
image you can boot either in `systemd-nspawn` or in a UEFI-capable VM:

```sh
$ sudo mkosi boot # nspawn still needs sudo for now
```

or:

```sh
$ mkosi qemu
```

Every time you rerun the `mkosi` command a fresh image is built, incorporating
all current changes you made to the project tree.

Putting this all together, here's a series of commands for preparing a patch
for systemd:

```sh
$ git clone https://github.com/systemd/mkosi.git  # If mkosi v19 or newer is not packaged by your distribution
$ ln -s $PWD/mkosi/bin/mkosi /usr/local/bin/mkosi # If mkosi v19 or newer is not packaged by your distribution
$ git clone https://github.com/systemd/systemd.git
$ cd systemd
$ git checkout -b <BRANCH>        # where BRANCH is the name of the branch
$ vim src/core/main.c             # or wherever you'd like to make your changes
$ mkosi -f qemu                   # (re-)build and boot up the test image in qemu
$ git add -p                      # interactively put together your patch
$ git commit                      # commit it
$ git push -u <REMOTE>            # where REMOTE is your "fork" on GitHub
```

And after that, head over to your repo on GitHub and click "Compare & pull request"

If you want to do a local build without mkosi, most distributions also provide
very simple and convenient ways to install most development packages necessary
to build systemd:

```sh
# Fedora
$ sudo dnf builddep systemd
# Debian/Ubuntu
$ sudo apt-get build-dep systemd
# Arch
$ sudo pacman -S devtools
$ pkgctl repo clone --protocol=https systemd
$ cd systemd
$ makepkg -seoc
```

After installing the development packages, systemd can be built from source as follows:

```sh
$ meson setup build <options>
$ ninja -C build
$ meson test -C build
```

Happy hacking!

## Templating engines in .in files

Some source files are generated during build. We use two templating engines:
* meson's `configure_file()` directive uses syntax with `@VARIABLE@`.

  See the
  [Meson docs for `configure_file()`](https://mesonbuild.com/Reference-manual.html#configure_file)
  for details.

{% raw %}
* most files are rendered using jinja2, with `{{VARIABLE}}` and `{% if … %}`,
  `{% elif … %}`, `{% else … %}`, `{% endif … %}` blocks. `{# … #}` is a
  jinja2 comment, i.e. that block will not be visible in the rendered
  output. `{% raw %} … `{% endraw %}`{{ '{' }}{{ '% endraw %' }}}` creates a block
  where jinja2 syntax is not interpreted.

  See the
  [Jinja Template Designer Documentation](https://jinja.palletsprojects.com/en/3.1.x/templates/#synopsis)
  for details.

Please note that files for both template engines use the `.in` extension.

## Developer and release modes

In the default meson configuration (`-Dmode=developer`), certain checks are
enabled that are suitable when hacking on systemd (such as internal
documentation consistency checks). Those are not useful when compiling for
distribution and can be disabled by setting `-Dmode=release`.

## Sanitizers in mkosi

See [Testing systemd using sanitizers](/TESTING_WITH_SANITIZERS) for more information
on how to build with sanitizers enabled in mkosi.

## Fuzzers

systemd includes fuzzers in `src/fuzz/` that use libFuzzer and are automatically
run by [OSS-Fuzz](https://github.com/google/oss-fuzz) with sanitizers.
To add a fuzz target, create a new `src/fuzz/fuzz-foo.c` file with a `LLVMFuzzerTestOneInput`
function and add it to the list in `src/fuzz/meson.build`.

Whenever possible, a seed corpus and a dictionary should also be added with new
fuzz targets. The dictionary should be named `src/fuzz/fuzz-foo.dict` and the seed
corpus should be built and exported as `$OUT/fuzz-foo_seed_corpus.zip` in
`tools/oss-fuzz.sh`.

The fuzzers can be built locally if you have libFuzzer installed by running
`tools/oss-fuzz.sh`, or by running:

```
CC=clang CXX=clang++ \
meson setup build-libfuzz -Dllvm-fuzz=true -Db_sanitize=address,undefined -Db_lundef=false \
                          -Dc_args='-fno-omit-frame-pointer -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION'
ninja -C build-libfuzz fuzzers
```

Each fuzzer then can be then run manually together with a directory containing
the initial corpus:

```
export UBSAN_OPTIONS=print_stacktrace=1:print_summary=1:halt_on_error=1
build-libfuzz/fuzz-varlink-idl test/fuzz/fuzz-varlink-idl/
```

Note: the `halt_on_error=1` UBSan option is especially important, otherwise
the fuzzer won't crash when undefined behavior is triggered.

You should also confirm that the fuzzers can be built and run using
[the OSS-Fuzz toolchain](https://google.github.io/oss-fuzz/advanced-topics/reproducing/#building-using-docker):

```
path_to_systemd=...

git clone --depth=1 https://github.com/google/oss-fuzz
cd oss-fuzz

for sanitizer in address undefined memory; do
  for engine in libfuzzer afl honggfuzz; do
    ./infra/helper.py build_fuzzers --sanitizer "$sanitizer" --engine "$engine" \
       --clean systemd "$path_to_systemd"

    ./infra/helper.py check_build --sanitizer "$sanitizer" --engine "$engine" \
      -e ALLOWED_BROKEN_TARGETS_PERCENTAGE=0 systemd
  done
done

./infra/helper.py build_fuzzers --clean --architecture i386 systemd "$path_to_systemd"
./infra/helper.py check_build --architecture i386 -e ALLOWED_BROKEN_TARGETS_PERCENTAGE=0 systemd

./infra/helper.py build_fuzzers --clean --sanitizer coverage systemd "$path_to_systemd"
./infra/helper.py coverage --no-corpus-download systemd
```

If you find a bug that impacts the security of systemd, please follow the
guidance in [CONTRIBUTING.md](/CONTRIBUTING) on how to report a security vulnerability.

For more details on building fuzzers and integrating with OSS-Fuzz, visit:

- [Setting up a new project - OSS-Fuzz](https://google.github.io/oss-fuzz/getting-started/new-project-guide/)
- [Tutorials - OSS-Fuzz](https://google.github.io/oss-fuzz/reference/useful-links/#tutorials)

## Debugging binaries that need to run as root in vscode

When trying to debug binaries that need to run as root, we need to do some custom configuration in vscode to
have it try to run the applications as root and to ask the user for the root password when trying to start
the binary. To achieve this, we'll use a custom debugger path which points to a script that starts `gdb` as
root using `pkexec`. pkexec will prompt the user for their root password via a graphical interface. This
guide assumes the C/C++ extension is used for debugging.

First, create a file `sgdb` in the root of the systemd repository with the following contents and make it
executable:

```
#!/bin/sh
exec pkexec gdb "$@"
```

Then, open launch.json in vscode, and set `miDebuggerPath` to `${workspaceFolder}/sgdb` for the corresponding
debug configuration. Now, whenever you try to debug the application, vscode will try to start gdb as root via
pkexec which will prompt you for your password via a graphical interface. After entering your password,
vscode should be able to start debugging the application.

For more information on how to set up a debug configuration for C binaries, please refer to the official
vscode documentation [here](https://code.visualstudio.com/docs/cpp/launch-json-reference)

## Debugging systemd with mkosi + vscode

To simplify debugging systemd when testing changes using mkosi, we're going to show how to attach
[VSCode](https://code.visualstudio.com/)'s debugger to an instance of systemd running in a mkosi image using
QEMU.

To allow VSCode's debugger to attach to systemd running in a mkosi image, we have to make sure it can access
the virtual machine spawned by mkosi where systemd is running. mkosi makes this possible via a handy SSH
option that makes the generated image accessible via SSH when booted. Thus you must build the image with
`mkosi --ssh`. The easiest way to set the option is to create a file `mkosi.local.conf` in the root of the
repository and add the following contents:

```
[Host]
Ssh=yes
RuntimeTrees=.
```

Also make sure that the SSH agent is running on your system and that you've added your SSH key to it with
`ssh-add`. Also make sure that `virtiofsd` is installed.

After rebuilding the image and booting it with `mkosi qemu`, you should now be able to connect to it by
running `mkosi ssh` from the same directory in another terminal window.

Now we need to configure VSCode. First, make sure the C/C++ extension is installed. If you're already using
a different extension for code completion and other IDE features for C in VSCode, make sure to disable the
corresponding parts of the C/C++ extension in your VSCode user settings by adding the following entries:

```json
"C_Cpp.formatting": "Disabled",
"C_Cpp.intelliSenseEngine": "Disabled",
"C_Cpp.enhancedColorization": "Disabled",
"C_Cpp.suggestSnippets": false,
```

With the extension set up, we can create the launch.json file in the .vscode/ directory to tell the VSCode
debugger how to attach to the systemd instance running in our mkosi container/VM. Create the file, and possibly
the directory, and add the following contents:

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "type": "cppdbg",
            "program": "/usr/lib/systemd/systemd",
            "processId": "${command:pickRemoteProcess}",
            "request": "attach",
            "name": "systemd",
            "pipeTransport": {
                "pipeProgram": "mkosi",
                "pipeArgs": [
                    "-C",
                    "/path/to/systemd/repo/directory/on/host/system/",
                    "ssh"
                ],
                "debuggerPath": "/usr/bin/gdb"
            },
            "MIMode": "gdb",
            "sourceFileMap": {
                "/root/src/systemd": {
                    "editorPath": "${workspaceFolder}",
                    "useForBreakpoints": false
                },
            }
        }
    ]
}
```

Now that the debugger knows how to connect to our process in the container/VM and we've set up the necessary
source mappings, go to the "Run and Debug" window and run the "systemd" debug configuration. If everything
goes well, the debugger should now be attached to the systemd instance running in the container/VM. You can
attach breakpoints from the editor and enjoy all the other features of VSCode's debugger.

To debug systemd components other than PID 1, set "program" to the full path of the component you want to
debug and set "processId" to "${command:pickProcess}". Now, when starting the debugger, VSCode will ask you
the PID of the process you want to debug. Run `systemctl show --property MainPID --value <component>` in the
container to figure out the PID and enter it when asked and VSCode will attach to that process instead.

## Debugging systemd-boot

During boot, systemd-boot and the stub loader will output messages like
`systemd-boot@0x0A` and `systemd-stub@0x0B`, providing the base of the loaded
code. This location can then be used to attach to a QEMU session (provided it
was run with `-s`). See `debug-sd-boot.sh` script in the tools folder which
automates this processes.

If the debugger is too slow to attach to examine an early boot code passage,
the call to `DEFINE_EFI_MAIN_FUNCTION()` can be modified to enable waiting. As
soon as the debugger has control, we can then run `set variable wait = 0` or
`return` to continue. Once the debugger has attached, setting breakpoints will
work like usual.

To debug systemd-boot in an IDE such as VSCode we can use a launch configuration like this:
```json
{
    "name": "systemd-boot",
    "type": "cppdbg",
    "request": "launch",
    "program": "${workspaceFolder}/build/src/boot/efi/systemd-bootx64.efi",
    "cwd": "${workspaceFolder}",
    "MIMode": "gdb",
    "miDebuggerServerAddress": ":1234",
    "setupCommands": [
        { "text": "shell mkfifo /tmp/sdboot.{in,out}" },
        { "text": "shell qemu-system-x86_64 [...] -s -serial pipe:/tmp/sdboot" },
        { "text": "shell ${workspaceFolder}/tools/debug-sd-boot.sh ${workspaceFolder}/build/src/boot/efi/systemd-bootx64.efi /tmp/sdboot.out systemd-boot.gdb" },
        { "text": "source /tmp/systemd-boot.gdb" },
    ]
}
```
