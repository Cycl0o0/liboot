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
