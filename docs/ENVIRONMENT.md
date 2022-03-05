---
title: Known Environment Variables
category: Interfaces
layout: default
SPDX-License-Identifier: LGPL-2.1-or-later
---

# Known Environment Variables

A number of systemd components take additional runtime parameters via
environment variables. Many of these environment variables are not supported at
the same level as command line switches and other interfaces are: we don't
document them in the man pages and we make no stability guarantees for
them. While they generally are unlikely to be dropped any time soon again, we
do not want to guarantee that they stay around for good either.

Below is an (incomprehensive) list of the environment variables understood by
the various tools. Note that this list only covers environment variables not
documented in the proper man pages.

All tools:

* `$SYSTEMD_OFFLINE=[0|1]` — if set to `1`, then `systemctl` will refrain from
  talking to PID 1; this has the same effect as the historical detection of
  `chroot()`. Setting this variable to `0` instead has a similar effect as
  `$SYSTEMD_IGNORE_CHROOT=1`; i.e. tools will try to communicate with PID 1
  even if a `chroot()` environment is detected. You almost certainly want to
  set this to `1` if you maintain a package build system or similar and are
  trying to use a modern container system and not plain `chroot()`.

* `$SYSTEMD_IGNORE_CHROOT=1` — if set, don't check whether being invoked in a
  `chroot()` environment. This is particularly relevant for systemctl, as it
  will not alter its behaviour for `chroot()` environments if set. Normally it
  refrains from talking to PID 1 in such a case; turning most operations such
  as `start` into no-ops.  If that's what's explicitly desired, you might
  consider setting `$SYSTEMD_OFFLINE=1`.

* `$SD_EVENT_PROFILE_DELAYS=1` — if set, the sd-event event loop implementation
  will print latency information at runtime.

* `$SYSTEMD_PROC_CMDLINE` — if set, the contents are used as the kernel command
  line instead of the actual one in `/proc/cmdline`. This is useful for
  debugging, in order to test generators and other code against specific kernel
  command lines.

* `$SYSTEMD_FSTAB` — if set, use this path instead of `/etc/fstab`. Only useful
  for debugging.

* `$SYSTEMD_CRYPTTAB` — if set, use this path instead of `/etc/crypttab`. Only
  useful for debugging. Currently only supported by
  `systemd-cryptsetup-generator`.

* `$SYSTEMD_INTEGRITYTAB` — if set, use this path instead of
  `/etc/integritytab`. Only useful for debugging. Currently only supported by
  `systemd-integritysetup-generator`.

* `$SYSTEMD_VERITYTAB` — if set, use this path instead of
  `/etc/veritytab`. Only useful for debugging. Currently only supported by
  `systemd-veritysetup-generator`.

* `$SYSTEMD_EFI_OPTIONS` — if set, used instead of the string in the
  `SystemdOptions` EFI variable. Analogous to `$SYSTEMD_PROC_CMDLINE`.

* `$SYSTEMD_DEFAULT_HOSTNAME` — override the compiled-in fallback hostname
  (relevant in particular for the system manager and `systemd-hostnamed`).
  Must be a valid hostname (either a single label or a FQDN).

* `$SYSTEMD_IN_INITRD=[auto|lenient|0|1]` — if set, specifies initrd detection
  method. Defaults to `auto`. Behavior is defined as follows:
  `auto`: Checks if `/etc/initrd-release` exists, and a temporary fs is mounted
          on `/`. If both conditions meet, then it's in initrd.
  `lenient`: Similar to `auto`, but the rootfs check is skipped.
  `0|1`: Simply overrides initrd detection. This is useful for debugging and
         testing initrd-only programs in the main system.

* `$SYSTEMD_BUS_TIMEOUT=SECS` — specifies the maximum time to wait for method call
  completion. If no time unit is specified, assumes seconds. The usual other units
  are understood, too (us, ms, s, min, h, d, w, month, y). If it is not set or set
  to 0, then the built-in default is used.

* `$SYSTEMD_MEMPOOL=0` — if set, the internal memory caching logic employed by
  hash tables is turned off, and libc `malloc()` is used for all allocations.

* `$SYSTEMD_EMOJI=0` — if set, tools such as `systemd-analyze security` will
  not output graphical smiley emojis, but ASCII alternatives instead. Note that
  this only controls use of Unicode emoji glyphs, and has no effect on other
  Unicode glyphs.

* `$RUNTIME_DIRECTORY` — various tools use this variable to locate the
  appropriate path under `/run/`. This variable is also set by the manager when
  `RuntimeDirectory=` is used, see systemd.exec(5).

* `$SYSTEMD_CRYPT_PREFIX` — if set configures the hash method prefix to use for
  UNIX `crypt()` when generating passwords. By default the system's "preferred
  method" is used, but this can be overridden with this environment variable.
  Takes a prefix such as `$6$` or `$y$`. (Note that this is only honoured on
  systems built with libxcrypt and is ignored on systems using glibc's
  original, internal `crypt()` implementation.)

* `$SYSTEMD_RDRAND=0` — if set, the RDRAND instruction will never be used,
  even if the CPU supports it.

* `$SYSTEMD_SECCOMP=0` — if set, seccomp filters will not be enforced, even if
  support for it is compiled in and available in the kernel.

* `$SYSTEMD_LOG_SECCOMP=1` — if set, system calls blocked by seccomp filtering,
  for example in `systemd-nspawn`, will be logged to the audit log, if the
  kernel supports this.

`systemctl`:

* `$SYSTEMCTL_FORCE_BUS=1` — if set, do not connect to PID1's private D-Bus
  listener, and instead always connect through the dbus-daemon D-bus broker.

* `$SYSTEMCTL_INSTALL_CLIENT_SIDE=1` — if set, enable or disable unit files on
  the client side, instead of asking PID 1 to do this.

* `$SYSTEMCTL_SKIP_SYSV=1` — if set, do not call SysV compatibility hooks.

`systemd-nspawn`:

* `$SYSTEMD_NSPAWN_UNIFIED_HIERARCHY=1` — if set, force `systemd-nspawn` into
  unified cgroup hierarchy mode.

* `$SYSTEMD_NSPAWN_API_VFS_WRITABLE=1` — if set, make `/sys/`, `/proc/sys/`,
  and friends writable in the container. If set to "network", leave only
  `/proc/sys/net/` writable.

* `$SYSTEMD_NSPAWN_CONTAINER_SERVICE=…` — override the "service" name nspawn
  uses to register with machined. If unset defaults to "nspawn", but with this
  variable may be set to any other value.

* `$SYSTEMD_NSPAWN_USE_CGNS=0` — if set, do not use cgroup namespacing, even if
  it is available.

* `$SYSTEMD_NSPAWN_LOCK=0` — if set, do not lock container images when running.

* `$SYSTEMD_NSPAWN_TMPFS_TMP=0` — if set, do not overmount `/tmp/` in the
  container with a tmpfs, but leave the directory from the image in place.

* `$SYSTEMD_SUPPRESS_SYNC=1` — if set, all disk synchronization syscalls are
  blocked to the container payload (e.g. `sync()`, `fsync()`, `syncfs()`, …)
  and the `O_SYNC`/`O_DSYNC` flags are made unavailable to `open()` and
  friends. This is equivalent to passing `--suppress-sync=yes` on the
  `systemd-nspawn` command line.

`systemd-logind`:

* `$SYSTEMD_BYPASS_HIBERNATION_MEMORY_CHECK=1` — if set, report that
  hibernation is available even if the swap devices do not provide enough room
  for it.

* `$SYSTEMD_REBOOT_TO_FIRMWARE_SETUP` — if set, overrides `systemd-logind`'s
  built-in EFI logic of requesting a reboot into the firmware. Takes a boolean.
  If set to false, the functionality is turned off entirely. If set to true,
  instead of requesting a reboot into the firmware setup UI through EFI a file,
  `/run/systemd/reboot-to-firmware-setup` is created whenever this is
  requested. This file may be checked for by services run during system
  shutdown in order to request the appropriate operation from the firmware in
  an alternative fashion.

* `$SYSTEMD_REBOOT_TO_BOOT_LOADER_MENU` — similar to the above, allows
  overriding of `systemd-logind`'s built-in EFI logic of requesting a reboot
  into the boot loader menu. Takes a boolean. If set to false, the
  functionality is turned off entirely. If set to true, instead of requesting a
  reboot into the boot loader menu through EFI, the file
  `/run/systemd/reboot-to-boot-loader-menu` is created whenever this is
  requested. The file contains the requested boot loader menu timeout in µs,
  formatted in ASCII decimals, or zero in case no timeout is requested. This
  file may be checked for by services run during system shutdown in order to
  request the appropriate operation from the boot loader in an alternative
  fashion.

* `$SYSTEMD_REBOOT_TO_BOOT_LOADER_ENTRY` — similar to the above, allows
  overriding of `systemd-logind`'s built-in EFI logic of requesting a reboot
  into a specific boot loader entry. Takes a boolean. If set to false, the
  functionality is turned off entirely. If set to true, instead of requesting a
  reboot into a specific boot loader entry through EFI, the file
  `/run/systemd/reboot-to-boot-loader-entry` is created whenever this is
  requested. The file contains the requested boot loader entry identifier. This
  file may be checked for by services run during system shutdown in order to
  request the appropriate operation from the boot loader in an alternative
  fashion. Note that by default only boot loader entries which follow the [Boot
  Loader Specification](https://systemd.io/BOOT_LOADER_SPECIFICATION) and are
  placed in the ESP or the Extended Boot Loader partition may be selected this
  way. However, if a directory `/run/boot-loader-entries/` exists, the entries
  are loaded from there instead. The directory should contain the usual
  directory hierarchy mandated by the Boot Loader Specification, i.e. the entry
  drop-ins should be placed in
  `/run/boot-loader-entries/loader/entries/*.conf`, and the files referenced by
  the drop-ins (including the kernels and initrds) somewhere else below
  `/run/boot-loader-entries/`. Note that all these files may be (and are
  supposed to be) symlinks. `systemd-logind` will load these files on-demand,
  these files can hence be updated (ideally atomically) whenever the boot
  loader configuration changes. A foreign boot loader installer script should
  hence synthesize drop-in snippets and symlinks for all boot entries at boot
  or whenever they change if it wants to integrate with `systemd-logind`'s
  APIs.

`systemd-udevd`:

* `$NET_NAMING_SCHEME=` — if set, takes a network naming scheme (i.e. one of
  "v238", "v239", "v240"…, or the special value "latest") as parameter. If
  specified udev's `net_id` builtin will follow the specified naming scheme
  when determining stable network interface names. This may be used to revert
  to naming schemes of older udev versions, in order to provide more stable
  naming across updates. This environment variable takes precedence over the
  kernel command line option `net.naming-scheme=`, except if the value is
  prefixed with `:` in which case the kernel command line option takes
  precedence, if it is specified as well.

`nss-systemd`:

* `$SYSTEMD_NSS_BYPASS_SYNTHETIC=1` — if set, `nss-systemd` won't synthesize
  user/group records for the `root` and `nobody` users if they are missing from
  `/etc/passwd`.

* `$SYSTEMD_NSS_DYNAMIC_BYPASS=1` — if set, `nss-systemd` won't return
  user/group records for dynamically registered service users (i.e. users
  registered through `DynamicUser=1`).

`systemd-timedated`:

* `$SYSTEMD_TIMEDATED_NTP_SERVICES=…` — colon-separated list of unit names of
  NTP client services. If set, `timedatectl set-ntp on` enables and starts the
  first existing unit listed in the environment variable, and
  `timedatectl set-ntp off` disables and stops all listed units.

`systemd-sulogin-shell`:

* `$SYSTEMD_SULOGIN_FORCE=1` — This skips asking for the root password if the
  root password is not available (such as when the root account is locked).
  See `sulogin(8)` for more details.

`bootctl` and other tools that access the EFI System Partition (ESP):

* `$SYSTEMD_RELAX_ESP_CHECKS=1` — if set, the ESP validation checks are
  relaxed. Specifically, validation checks that ensure the specified ESP path
  is a FAT file system are turned off, as are checks that the path is located
  on a GPT partition with the correct type UUID.

* `$SYSTEMD_ESP_PATH=…` — override the path to the EFI System Partition. This
  may be used to override ESP path auto detection, and redirect any accesses to
  the ESP to the specified directory. Note that unlike with `bootctl`'s
  `--path=` switch only very superficial validation of the specified path is
  done when this environment variable is used.

`systemd` itself:

* `$SYSTEMD_ACTIVATION_UNIT` — set for all NSS and PAM module invocations that
  are done by the service manager on behalf of a specific unit, in child
  processes that are later (after execve()) going to become unit
  processes. Contains the full unit name (e.g. "foobar.service"). NSS and PAM
  modules can use this information to determine in which context and on whose
  behalf they are being called, which may be useful to avoid deadlocks, for
  example to bypass IPC calls to the very service that is about to be
  started. Note that NSS and PAM modules should be careful to only rely on this
  data when invoked privileged, or possibly only when getppid() returns 1, as
  setting environment variables is of course possible in any even unprivileged
  contexts.

* `$SYSTEMD_ACTIVATION_SCOPE` — closely related to `$SYSTEMD_ACTIVATION_UNIT`,
  it is either set to `system` or `user` depending on whether the NSS/PAM
  module is called by systemd in `--system` or `--user` mode.

`systemd-remount-fs`:

* `$SYSTEMD_REMOUNT_ROOT_RW=1` — if set and no entry for the root directory
  exists in `/etc/fstab` (this file always takes precedence), then the root
  directory is remounted writable. This is primarily used by
  `systemd-gpt-auto-generator` to ensure the root partition is mounted writable
  in accordance to the GPT partition flags.

`systemd-firstboot` and `localectl`:

* `$SYSTEMD_LIST_NON_UTF8_LOCALES=1` — if set, non-UTF-8 locales are listed among
  the installed ones. By default non-UTF-8 locales are suppressed from the
  selection, since we are living in the 21st century.

`systemd-sysext`:

* `$SYSTEMD_SYSEXT_HIERARCHIES` — this variable may be used to override which
  hierarchies are managed by `systemd-sysext`. By default only `/usr/` and
  `/opt/` are managed, and directories may be added or removed to that list by
  setting this environment variable to a colon-separated list of absolute
  paths. Only "real" file systems and directories that only contain "real" file
  systems as submounts should be used. Do not specify API file systems such as
  `/proc/` or `/sys/` here, or hierarchies that have them as submounts. In
  particular, do not specify the root directory `/` here.

`systemd-tmpfiles`:

* `$SYSTEMD_TMPFILES_FORCE_SUBVOL` — if unset, `v`/`q`/`Q` lines will create
  subvolumes only if the OS itself is installed into a subvolume. If set to `1`
  (or another value interpreted as true), these lines will always create
  subvolumes if the backing filesystem supports them. If set to `0`, these
  lines will always create directories.

`systemd-sysv-generator`:

* `$SYSTEMD_SYSVINIT_PATH` — Controls where `systemd-sysv-generator` looks for
  SysV init scripts.

* `$SYSTEMD_SYSVRCND_PATH` — Controls where `systemd-sysv-generator` looks for
  SysV init script runlevel link farms.

systemd tests:

* `$SYSTEMD_TEST_DATA` — override the location of test data. This is useful if
  a test executable is moved to an arbitrary location.

* `$SYSTEMD_TEST_NSS_BUFSIZE` — size of scratch buffers for "reentrant"
  functions exported by the nss modules.

fuzzers:

* `$SYSTEMD_FUZZ_OUTPUT` — A boolean that specifies whether to write output to
  stdout. Setting to true is useful in manual invocations, since all output is
  suppressed by default.

* `$SYSTEMD_FUZZ_RUNS` — The number of times execution should be repeated in
  manual invocations.

Note that is may be also useful to set `$SYSTEMD_LOG_LEVEL`, since all logging
is suppressed by default.

`systemd-importd`:

* `$SYSTEMD_IMPORT_BTRFS_SUBVOL` — takes a boolean, which controls whether to
  prefer creating btrfs subvolumes over plain directories for machine
  images. Has no effect on non-btrfs file systems where subvolumes are not
  available anyway. If not set, defaults to true.

* `$SYSTEMD_IMPORT_BTRFS_QUOTA` — takes a boolean, which controls whether to set
  up quota automatically for created btrfs subvolumes for machine images. If
  not set, defaults to true. Has no effect if machines are placed in regular
  directories, because btrfs subvolumes are not supported or disabled. If
  enabled, the quota group of the subvolume is automatically added to a
  combined quota group for all such machine subvolumes.

* `$SYSTEMD_IMPORT_SYNC` — takes a boolean, which controls whether to
  synchronize images to disk after installing them, before completing the
  operation. If not set, defaults to true. If disabled installation of images
  will be quicker, but not as safe.

`systemd-dissect`, `systemd-nspawn` and all other tools that may operate on
disk images with `--image=` or similar:

* `$SYSTEMD_DISSECT_VERITY_SIDECAR` — takes a boolean, which controls whether to
  load "sidecar" Verity metadata files. If enabled (which is the default),
  whenever a disk image is used, a set of files with the `.roothash`,
  `.usrhash`, `.roothash.p7s`, `.usrhash.p7s`, `.verity` suffixes are searched
  adjacent to disk image file, containing the Verity root hashes, their
  signatures or the Verity data itself. If disabled this automatic discovery of
  Verity metadata files is turned off.

* `$SYSTEMD_DISSECT_VERITY_EMBEDDED` — takes a boolean, which controls whether
  to load the embedded Verity signature data. If enabled (which is the
  default), Verity root hash information and a suitable signature is
  automatically acquired from a signature partition, following the
  [Discoverable Partitions
  Specification](https://systemd.io/DISCOVERABLE_PARTITIONS). If disabled any
  such partition is ignored. Note that this only disables discovery of the root
  hash and its signature, the Verity data partition itself is still searched in
  the GPT image.

* `$SYSTEMD_DISSECT_VERITY_SIGNATURE` — takes a boolean, which controls whether
  to validate the signature of the Verity root hash if available. If enabled
  (which is the default), the signature of suitable disk images is validated
  against any of the certificates in `/etc/verity.d/*.crt` (and similar
  directories in `/usr/lib/`, `/run`, …) or passed to the kernel for validation
  against its built-in certificates.

* `$SYSTEMD_LOOP_DIRECT_IO` – takes a boolean, which controls whether to enable
  LO_FLAGS_DIRECT_IO (i.e. direct IO + asynchronous IO) on loopback block
  devices when opening them. Defaults to on, set this to "0" to disable this
  feature.

`systemd-cryptsetup`:

* `$SYSTEMD_CRYPTSETUP_USE_TOKEN_MODULE` – takes a boolean, which controls
  whether to use the libcryptsetup "token" plugin module logic even when
  activating via FIDO2, PKCS#11, TPM2, i.e. mechanisms natively supported by
  `systemd-cryptsetup`. Defaults to enabled.

Various tools that read passwords from the TTY, such as `systemd-cryptenroll`
and `homectl`:

* `$PASSWORD` — takes a string: the literal password to use. If this
  environment variable is set it is used as password instead of prompting the
  user interactively. This exists primarily for debugging and testing
  purposes. Do not use this for production code paths, since environment
  variables are typically inherited down the process tree without restrictions
  and should thus not be used for secrets.

* `$NEWPASSWORD` — similar to `$PASSWORD` above, but is used when both a
  current and a future password are required, for example if the password is to
  be changed. In that case `$PASSWORD` shall carry the current (i.e. old)
  password and `$NEWPASSWORD` the new.

`systemd-homed`:

* `$SYSTEMD_HOME_ROOT` – defines an absolute path where to look for home
  directories/images. When unspecified defaults to `/home/`. This is useful for
  debugging purposes in order to run a secondary `systemd-homed` instance that
  operates on a different directory where home directories/images are placed.

* `$SYSTEMD_HOME_RECORD_DIR` – defines an absolute path where to look for
  fixated home records kept on the host. When unspecified defaults to
  `/var/lib/systemd/home/`. Similar to `$SYSTEMD_HOME_ROOT` this is useful for
  debugging purposes, in order to run a secondary `systemd-homed` instance that
  operates on a record database entirely separate from the host's.

* `$SYSTEMD_HOME_DEBUG_SUFFIX` – takes a short string that is suffixed to
  `systemd-homed`'s D-Bus and Varlink service names/sockets. This is also
  understood by `homectl`. This too is useful for running an additional copy of
  `systemd-homed` that doesn't interfere with the host's main one.

* `$SYSTEMD_HOMEWORK_PATH` – configures the path to the `systemd-homework`
  binary to invoke. If not specified defaults to
  `/usr/lib/systemd/systemd-homework`.

  Combining these four environment variables is pretty useful when
  debugging/developing `systemd-homed`:
```sh
SYSTEMD_HOME_DEBUG_SUFFIX=foo \
      SYSTEMD_HOMEWORK_PATH=/home/lennart/projects/systemd/build/systemd-homework \
      SYSTEMD_HOME_ROOT=/home.foo/ \
      SYSTEMD_HOME_RECORD_DIR=/var/lib/systemd/home.foo/ \
      /home/lennart/projects/systemd/build/systemd-homed
```

* `$SYSTEMD_HOME_MOUNT_OPTIONS_BTRFS`, `$SYSTEMD_HOME_MOUNT_OPTIONS_EXT4`,
  `$SYSTEMD_HOME_MOUNT_OPTIONS_XFS` – configure the default mount options to
  use for LUKS home directories, overriding the built-in default mount
  options. There's one variable for each of the supported file systems for the
  LUKS home directory backend.
