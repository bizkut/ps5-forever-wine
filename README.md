## INTRODUCTION

Wine is a program which allows running Microsoft Windows programs
(including DOS, Windows 3.x, Win32, and Win64 executables) on Unix.
It consists of a program loader which loads and executes a Microsoft
Windows binary, and a library (called Winelib) that implements Windows
API calls using their Unix, X11 or Mac equivalents.  The library may also
be used for porting Windows code into native Unix executables.

Wine is free software, released under the GNU LGPL; see the file
LICENSE for the details.


## QUICK START

From the top-level directory of the Wine source (which contains this file),
run:

```
./configure
make
```

Then either install Wine:

```
make install
```

Or run Wine directly from the build directory:

```
./wine notepad
```

Run programs as `wine program`. For more information and problem
resolution, read the rest of this file, the Wine man page, and
especially the wealth of information found at https://www.winehq.org.


## REQUIREMENTS

To compile and run Wine, you must have one of the following:

- Linux version 2.6.22 or later
- FreeBSD 12.4 or later
- Solaris x86 9 or later
- NetBSD-current
- macOS 10.12 or later

As Wine requires kernel-level thread support to run, only the operating
systems mentioned above are supported.  Other operating systems which
support kernel threads may be supported in the future.

**FreeBSD info**:
  See https://wiki.freebsd.org/Wine for more information.

**Solaris info**:
  You will most likely need to build Wine with the GNU toolchain
  (gcc, gas, etc.). Warning : installing gas does *not* ensure that it
  will be used by gcc. Recompiling gcc after installing gas or
  symlinking cc, as and ld to the gnu tools is said to be necessary.

**NetBSD info**:
  Make sure you have the USER_LDT, SYSVSHM, SYSVSEM, and SYSVMSG options
  turned on in your kernel.

**macOS info**:
  You need Xcode/Xcode Command Line Tools or Apple cctools.  The
  minimum requirements for compiling Wine are clang 3.8 with the
  MacOSX10.13.sdk and mingw-w64 v12 for 32-bit wine.  The
  MacOSX10.14.sdk and later can build 64-bit wine.

**Supported file systems**:
  Wine should run on most file systems. A few compatibility problems
  have also been reported using files accessed through Samba. Also,
  NTFS does not provide all the file system features needed by some
  applications.  Using a native Unix file system is recommended.

**Basic requirements**:
  You need to have the X11 development include files installed
  (called xorg-dev in Debian and libX11-devel in Red Hat).
  Of course you also need make (most likely GNU make).
  You also need flex version 2.5.33 or later and bison.

**Optional support libraries**:
  Configure will display notices when optional libraries are not found
  on your system. See https://gitlab.winehq.org/wine/wine/-/wikis/Building-Wine
  for hints about the packages you should install. On 64-bit
  platforms, you have to make sure to install the 32-bit versions of
  these libraries.


## COMPILATION

To build Wine, do:

```
./configure
make
```

This will build the program "wine" and numerous support libraries/binaries.
The program "wine" will load and run Windows executables.
The library "libwine" ("Winelib") can be used to compile and link
Windows source code under Unix.

To see compile configuration options, do `./configure --help`.

For more information, see https://gitlab.winehq.org/wine/wine/-/wikis/Building-Wine

The PS5 title's WoW voice input uses `PS5_DESKTOP_VOICE_EDIT` from
`include/wine/ps5_desktop.h`. Each bounded edit posts control key taps and
committed UTF-16 characters to one focused-window FIFO, instead of mixing
hardware keys with posted text. This preserves the order of voice edits and
later hardware input in the normal unfiltered message pump. It deliberately
uses WoW's direct `WM_CHAR` path, not `VK_PACKET` translation; it is not a
general-purpose IME or an input-state replacement. The standalone host
regression tests the wire bounds, focus snapshot, control scan codes and
posted-message ordering without logging speech:

```
cc -std=c11 -Wall -Wextra -Werror -Iinclude loader/tests/ps5_input.c -o /tmp/ps5_input_test
/tmp/ps5_input_test
```

The title's explicit prefix initialization (`wineboot -i` with
`WINEPS5_PREFIX_INIT=1`) forces the normal native and WOW64 install passes even
when `.update-timestamp` is current. It publishes `.wineboot-init-done` only
after each install child exits successfully and both registry views contain
the normal Generic Verify V2 trust callbacks and PE SIP callbacks, with their
expected DLL/function values and resolvable provider exports. Optional DLL
registration HRESULTs keep SetupAPI's existing handling; an interrupted or
failed installation process cannot certify completion.
Before emitting proof, Wineboot checks `RegFlushKey` on real opened
`HKLM\\Software` handles in both views. The in-process title server saves its
registered hives itself and returns save failures through the request status,
without sending absolute title filenames to the client's relative-path writer.
Stream errors, data flush/sync errors, close errors and failed renames retain
dirty state; ordinary hive replacement occurs only after a successful save.
Native checkpoint publication synchronizes the prefix directory after Wine
quiesces, completing rename-metadata durability before readiness is committed.

The transient file contains exactly `WINE_PS5_PREFIX_INIT_PROOF` from
`include/wine/ps5_prefix_init.h`. The native title must wait for Wine to quiesce
and save its hives before persisting that proof under
`WINE_PS5_PREFIX_TRUST_READY_NAME` and admitting applications. The proof is a
prefix-initialization checkpoint, not a successful verification of any file.
Normal signature, digest, signer and certificate-policy checks still run.
An unregistered trust action with no verification steps now fails with
`TRUST_E_PROVIDER_UNKNOWN`, rather than reporting success without verification.
There is no file/hash allowlist or signature-failure override.

Products sharing the PS5 runtime may define `WB_USER_ROOT` (including its
trailing slash) and `WB_TITLE_ID` consistently for both Wine and the native
write broker. `include/wine/ps5_writebroker.h` retains `/data/forever/` and
`PPSA99030` when no overrides are supplied. The root scopes broker admission,
the token and icon paths; the title ID scopes home-screen icon replacement.
Never mix differently configured Wine and broker binaries.


The PS5 title write buffer admits regular-file writes to CASC archive layouts
on C:/W: at any depth: the leaf is `data.NNN` with exactly three decimal
digits, its parent is `data`, and its grandparent component ends with `data`
(for example `Data/data/data.000` or `SC2Data/data/data.000`). Components are
ASCII case-insensitive; no game-name or channel-name list is required, so
WoW's `_retail_` layout and StarCraft II both match.

The separate Battle.net client rule admits files below C:'s
`Program Files (x86)/Battle.net/` or `ProgramData/Battle.net/`. Its components
and excluded suffixes are ASCII case-insensitive; the configured Unix drive
roots must match exactly. `Logs` components, `.db`, `.sqlite`, `.sqlite3`,
`.log`, `-journal`, `-wal` and `-shm` leaves, and dot/traversal components
remain excluded from the Battle.net rule. Other paths are not made eligible,
and trace class names are unchanged.

Both scopes use the same process-local, inode-checked buffer: eight 1 MiB
extents, at most four per handle, with a 40 ms residence limit checked by a
10 ms worker. Explicit append/append-only and write-through writes bypass
buffering after flushing. Explicit nonnegative offsets, including overlapped
and `NO_BUFFERING` writes, and synchronous direct-mode file-pointer writes
may buffer. Overlapped writes retain Wine's regular-file completion path:
the copied byte count reaches the IOSB, event, APC queue or completion port,
and the call returns `STATUS_PENDING` without moving the file pointer.
There is no executable-name or read-access
filter. Reads and size queries through any same-process handle flush the
matching inode, as do explicit flush, size/allocation changes and handle
duplication. Close drains its owned extents; process handoff/exit and
namespace barriers drain globally. Overlap, capacity and identity conflicts
drain before falling back. Non-image file mappings drain and permanently
exclude only that device/inode, leaving unrelated files eligible. Resolved
data mappings outside the path rule are remembered too, so an ancestor
directory rename cannot admit a previously mapped descendant. File renames
into scope likewise exclude their source identity. The bounded 1,024-entry
identity table falls back to process-wide disable on exhaustion or unresolved
identity; hard-link creation also retains the global fail-closed boundary.
Short writes are retried; a failed flush retains unwritten bytes on a pinned
fd, disables new admission and reports the pending error to the owning
handle. This is not a cross-process cache-coherency protocol;
pre-existing/cross-process aliases remain outside its guarantees, and
database/log files deliberately stay on the direct path.

Host policy tests live in `loader/tests/ps5_title.c`; the companion
`ps5-wow/ps5/probes/psmkit-wine/build-write-buffer-test.py` extracts the
production buffer, regular-file write dispatch and completion tail from
`dlls/ntdll/unix/file.c` for its host regression suite. They cover CASC layout
and Battle.net path boundaries, same-/other-handle read/query, close/reopen,
flush and duplicate visibility using 7 KiB writes. Overlapped regressions also cover
IOSB byte counts, event/APC/completion-port calls, unchanged file position,
copied caller bytes, append handling and the write-through exclusion.

Title exit keeps asynchronous exit signals blocked through native pthread
teardown; a pending `SIGQUIT` must not re-enter `pthread_exit` when the TLS
destructor unregisters the Wine thread. The companion
`ps5-wow/ps5/probes/psmkit-wine/build-title-exit-test.py` extracts the actual
exit request and unregister destructor into `loader/tests/ps5_title_exit.c`.
Its host regression uses real pthread exits, pending signals and native TLS
destructors, covering callback return, a signal queued during teardown,
the already-closed callback gate and thread-registry retirement.



## SETUP

Once Wine has been built correctly, you can do `make install`; this
will install the wine executable and libraries, the Wine man page, and
other needed files.

Don't forget to uninstall any conflicting previous Wine installation
first.  Try either `dpkg -r wine` or `rpm -e wine` or `make uninstall`
before installing.

Once installed, you can run the `winecfg` configuration tool. See the
Support area at https://www.winehq.org/ for configuration hints.


## RUNNING PROGRAMS

When invoking Wine, you may specify the entire path to the executable,
or a filename only.

For example, to run Notepad:

```
wine notepad            (using the search Path as specified in
wine notepad.exe         the registry to locate the file)

wine c:\\windows\\notepad.exe      (using DOS filename syntax)

wine ~/.wine/drive_c/windows/notepad.exe  (using Unix filename syntax)

wine notepad.exe readme.txt          (calling program with parameters)
```

Wine is not perfect, so some programs may crash. If that happens you
will get a crash log that you should attach to your report when filing
a bug.


## GETTING MORE INFORMATION

- **WWW**: A great deal of information about Wine is available from WineHQ at
	https://www.winehq.org/ : various Wine Guides, application database,
	bug tracking. This is probably the best starting point.

- **FAQ**: The Wine FAQ is located at https://gitlab.winehq.org/wine/wine/-/wikis/FAQ

- **Wiki**: The Wine Wiki is located at https://gitlab.winehq.org/wine/wine/-/wikis/

- **Gitlab**: Wine development is hosted at https://gitlab.winehq.org

- **Mailing lists**:
	There are several mailing lists for Wine users and developers; see
	https://gitlab.winehq.org/wine/wine/-/wikis/Forums for more
	information.

- **Bugs**: Report bugs to Wine Bugzilla at https://bugs.winehq.org
	Please search the bugzilla database to check whether your
	problem is already known or fixed before posting a bug report.

- **IRC**: Online help is available at channel `#WineHQ` on irc.libera.chat.
