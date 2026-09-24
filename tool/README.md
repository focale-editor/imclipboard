# Linux clipboard component benchmark

This probe uses a private **X11 selection**. It does not overwrite the desktop's
CLIPBOARD or PRIMARY selection. GTK 3 development libraries and an X11/Xwayland
display are required. Run from the package root:

```sh
c++ -std=c++14 -O2 -Wall -Werror -Ilinux \
  tool/benchmark_linux_clipboard.cc linux/clipboard_service.cc \
  $(pkg-config --cflags --libs gtk+-3.0) -pthread \
  -o /tmp/imclipboard-benchmark
GDK_BACKEND=x11 /tmp/imclipboard-benchmark /path/to/image.jpg
```

The input is converted to RGBA PNG at compression level 1 before measurements.
Three rounds compare ordinary fully validated writes with generated-PNG writes,
including the owned input copy, asynchronous validation and publication. GTK
supply is measured separately. An optional second argument selects the input
PNG compression level; GTK may normalize encoder settings. The supplied bytes must equal the input exactly. This measures
native component cost, not a complete Flutter Copy/Cut operation or cross-process
clipboard transfer. A private selection has no desktop persistence manager:
manager acknowledgement and application-exit persistence are not measured.

## Recorded run

September 24, 2026, local Linux session, `Auroras.jpg`, 4903 × 3262 pixels,
28,655,167 input PNG bytes. Times are milliseconds:

| Round | Former decode + encode | New write | GTK supply | New total | Owned metadata |
|-------|-----------------------:|----------:|-----------:|----------:|---------------:|
| 1     | 5806.513               | 246.610   | 33.959     | 280.569   | 0.006          |
| 2     | 5812.077               | 242.718   | 26.463     | 269.181   | 0.005          |
| 3     | 5817.082               | 216.929   | 33.217     | 250.146   | 0.004          |

Median: **5812.077 → 269.181 ms**, approximately **95.4% less time** (21.6×).
These are machine- and input-specific measurements. GTK supply still copies
encoded data; PNG validation still decodes pixels, but does so on a worker.
A 1 ms GLib heartbeat ran 199–215 times during each new write, with maximum
observed gaps of 2.8–16.2 ms. The heartbeat excludes the synchronous supply step.

## Regression validation

Linux native tests exercise byte equality, token preservation, foreign PNG and
JPEG reads, cheap PNG metadata, invalid-write preservation, ordered writes,
mid-transfer ownership replacement, URI reads, and an empty clipboard. These
use private X11 selections too, and skip when an X11 display is unavailable.
Build the example with `include_imclipboard_tests` enabled and run its CTest
suite with `GDK_BACKEND=x11`.

Dart tests cover default defensive copies and ownership views, including slice
offsets and protection against mutation through the exposed buffer. Android
unit tests exercise bounded provider reads and PNG detection. Windows native
tests exercise asynchronous failure callbacks, write ordering and destruction
with work pending; they require a Windows build and message loop.

## Generated-PNG follow-up

September 24, 2026, same photo, 28,655,167 input PNG bytes. Three rounds:

| Round | Validated write | Generated PNG write | GTK supply |
|-------|----------------:|--------------------:|-----------:|
| 1 | 269.207 ms | 17.422 ms | 22.339 ms |
| 2 | 243.635 ms | 19.872 ms | 26.112 ms |
| 3 | 228.248 ms | 17.946 ms | 26.508 ms |

Median native write: 243.635 → 17.946 ms (92.6% lower). This comparison omits
Flutter channel serialization and cross-process transfer; it does not imply
that complete Copy/Cut takes 18 ms. Supplied PNG bytes are checked for equality.
The generated path validates structure only; ordinary writes keep full decoding.
