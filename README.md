# MCL SDK

Developer-facing low-level reference SDK for the **Machine Contact Layer**.

The primary reference SDK is a portable C implementation intended to run from small microcontrollers through larger embedded and host systems without changing the protocol contract.

## Implementation contract

The pre-v0.1 reference stack targets a conservative **C99** subset.

The protocol-facing library must:

- require no operating system;
- require no dynamic allocation;
- keep all mutable protocol state caller-owned;
- accept caller-provided input, output, and scratch buffers;
- require no hidden global mutable state;
- use fixed-width integer types for protocol-facing data;
- perform explicit byte and bit encoding rather than serializing C structs;
- make byte order and quantization explicit;
- return deterministic status codes rather than relying on global error state;
- compile in freestanding mode without required libc symbols;
- keep hardware, RTOS, audio, radio, clock, entropy, storage, and synchronization integration behind narrow platform or binding interfaces;
- remain usable from C++ through an `extern "C"` API boundary.

The protocol specification remains language-neutral. The C implementation is a reference implementation, not the authority for protocol meaning.

## Layer relationship

```text
application / product policy
          |
        MCL SDK
          |
  +-------+-------+
  |               |
MCL Core        MCL Link
  |               |
  +---- MCL Wire--+
          |
 transport binding
 AP / IP / BLE / UWB / future
          |
 platform driver / hardware
```

The SDK must not turn receipt of a claim or request into automatic authority. Local product policy remains sovereign.

## Memory model

The low-level API is designed around caller-owned objects and buffers:

```c
mcl_status_t mcl_node_init(
    mcl_node_t *node,
    const mcl_node_config_t *config,
    void *workspace,
    size_t workspace_size);
```

Exact API names and structures remain pre-v0.1 research until Core, Wire, and Link reference implementations are integrated.

## Portability target

Initial portability gates are:

- hosted GCC and Clang with strict warnings;
- sanitizer-backed host tests;
- freestanding ARM Cortex-M class compilation;
- freestanding 32-bit RISC-V compilation;
- no compiler extensions required by the protocol core;
- no mandatory architecture-specific DSP dependency.

Architecture-specific acceleration may be added behind optional backends without changing canonical protocol behavior.

## Status

Private research repository. Pre-v0.1 low-level reference SDK. API and ABI are not stable.
