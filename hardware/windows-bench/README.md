# Windows physical bench (experimental)

This host adapter drives the canonical machine facade, continuous acoustic
listener, modulator and BLE fragmentation code. Python supplies Windows audio,
Bluetooth, a monotonic clock and OS randomness. It accepts no peer address,
endpoint token or session reference. `--admit` explicitly authorizes reachable
strangers for a laboratory run; this is not authentication.

Build with CMake and Visual Studio 2022, then run the bridge checks:

```powershell
cmake -S mcl-sdk/hardware/windows-bench -B build-windows-bench -A x64
cmake --build build-windows-bench --config Release
python mcl-sdk/hardware/windows-bench/test_bridge.py build-windows-bench/Release/mcl_windows_bench.dll
python mcl-sdk/hardware/windows-bench/bench.py --dll build-windows-bench/Release/mcl_windows_bench.dll --duration 150 --admit
```

The host environment needs Python, NumPy, sounddevice/PortAudio, Bleak, and
Python/WinRT projections for Windows Bluetooth and storage streams. These are
host-tool dependencies, not dependencies of the freestanding C runtime. Select
the machine's microphone and speakers with `--input` and `--output`; enumerate
them with `python -m sounddevice`. The physical campaign uses WASAPI with a
measured input latency rather than assuming the default Windows audio backend.

The ABI checks require exact canonical PCM recovery, exact 40-byte recovery
through three minimum-MTU fragments, and rejection of a missing middle
fragment. Those checks do not establish physical interoperability.

## Known physical limitation

On the tested laptop, Windows GATT service advertising reports
`StartedWithoutAllAdvertisementData`: it omits the required complete
BLE-ACTIVATE-1 service-data beacon. The adapter refuses that candidate. A
separate raw-advertisement diagnostic emitted the exact token but did not
complete a connection; it is not part of this adapter and is not a workaround
approved by the protocol.

This port is therefore **not qualified for BLE-ACTIVATE-1 in both roles** and
must not be described as a fully conformant third peer based on its startup,
codec tests, or a successful scan. Three-machine exploratory runs and their
failures must be retained independently of the two-device lifecycle evidence.

Every callback and protocol operation runs on the owning event-loop thread.
Input captured during local transmission is excluded from decoding; queues are
bounded and drops are reported. Asynchronous BLE sends report queue admission,
and only the canonical handshake can establish contact.
