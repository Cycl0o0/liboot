# Language bindings

The repository includes two starter bindings for the engine API in
[`src/liboot_engine.h`](../src/liboot_engine.h). They cover the common lifecycle
and integration path but do not yet mirror every exported function:

- [`cpp/liboot.hpp`](cpp/liboot.hpp) — C++11 RAII ownership, exceptions for
  `OoTResult`, and borrowed frame views.
- [`csharp/LibOot.cs`](csharp/LibOot.cs) — unsafe blittable layouts and P/Invoke
  declarations suitable for Unity or another .NET host. See the
  [C# binding guide](csharp/README.md) for a complete lifecycle example.

They preserve the native ownership rules: one `OoTEngine` and one Link per
process, serialized calls on one gameplay thread, borrowed frame pointers, and
no callback re-entry.

## C++

Add `bindings/cpp` and the installed liboot include directory to the include
path, then link the application with `liboot`:

```cpp
#include <liboot.hpp>

liboot::Engine engine(rom.data(), rom.size());
const OoTSurface ground[] = {
    {0, {{-1000, 0, -1000}, {-1000, 0, 1000}, {1000, 0, 1000}}},
    {0, {{-1000, 0, -1000}, {1000, 0, 1000}, {1000, 0, -1000}}},
};
engine.load_world(ground, 2);
engine.create_link(0.0f, 0.0f, 0.0f);

OoTEngineInput input = liboot::default_input();
input.stickY = 1.0f;
const OoTEngineFrame &frame = engine.step(&input);
```

Frames and the pointers inside them are borrowed. Do not retain a reference
after another mutating call. Call `engine.close()` when teardown errors must be
reported or retried. Destruction must happen outside a liboot callback; the
noexcept RAII destructor terminates if native teardown fails rather than
silently abandoning the process-wide singleton.

## C# and Unity

Compile with unsafe code enabled and place `liboot.so` (Linux) or
`liboot.dylib` (macOS) where the runtime can resolve `liboot`. Windows is not
currently supported. Check every initializer result before using its structure:

```csharp
LibOot.EngineConfig config = default;
if (LibOot.Native.EngineConfigInit(ref config) != LibOot.Result.Ok)
    throw new InvalidOperationException("incompatible liboot config ABI");

LibOot.EngineInput input = default;
if (LibOot.Native.EngineInputInit(ref input) != LibOot.Result.Ok)
    throw new InvalidOperationException("incompatible liboot input ABI");
```

Pin the ROM only for `EngineCreate`; native creation copies it synchronously.
The returned frame, texture, event, and PCM pointers are borrowed native
memory. Copy them before a later mutating call or before sending work to
another thread.

If callbacks are installed, keep every managed delegate strongly referenced
until after `EngineDestroy`. Convert each delegate to a function pointer with
`Marshal.GetFunctionPointerForDelegate`, copy event data immediately, and do
not allow exceptions to cross the native boundary.

The integration examples in
[`docs/ENGINE_INTEGRATION.md`](../docs/ENGINE_INTEGRATION.md) cover coordinate
conversion, rendering, audio, collision, and engine-specific plugin layouts.
