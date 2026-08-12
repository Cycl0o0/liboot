# Security policy

## Supported version

Security fixes are applied to the latest `main` branch while liboot is in
pre-1.0 development.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting for the `Cycl0o0/liboot`
repository. Do not attach a ROM, extracted Nintendo assets, credentials, or a
crash dump containing process memory to a public issue.

Include a minimal reproduction, platform/compiler information, and a stack
trace with sensitive paths and environment values removed. A synthetic input
or a hash identifying the ROM revision is preferable to sharing copyrighted
data.

## Host responsibilities

Treat every ROM and host-provided collision buffer as untrusted input. Run
ROM processing away from secrets where possible, validate file size before
loading, serialize liboot calls on one thread, and keep crash dumps private.
The project is still improving parser hardening and does not claim that a
malformed ROM can be processed safely.

## Parser fuzzing

Standalone Clang libFuzzer targets for ROM byte-order normalization, dmadata,
bounded file extraction, and Yaz0 decoding live in [`fuzz/`](fuzz/README.md).
They use generated or arbitrary bytes and do not require a ROM.

Scene and audio parsing are not yet isolated from process-global runtime state.
The precise seams needed for useful coverage are recorded in the fuzzing README;
until those parsers are split at bounded byte-span interfaces, malformed scene
and audio structures remain outside the current fuzz claims.
