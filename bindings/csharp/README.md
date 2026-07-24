# liboot C# binding

`LibOot.cs` is a dependency-free P/Invoke binding for the stable liboot engine
API. It targets runtimes that support classic `System.Runtime.InteropServices`,
including Mono and modern .NET.

## Build

Compile the binding as a library (unsafe code is required by the native buffer
views):

```sh
mcs -unsafe -warnaserror -target:library -out:LibOot.dll LibOot.cs
```

Place the native library where the application loader can find it. On Linux
this is normally `liboot.so` beside the executable or in a directory from the
dynamic loader search path; use the corresponding `liboot.dll` or
`liboot.dylib` name on Windows or macOS.

## Safe initialization

Always compare the runtime API version before initializing an ABI structure.
The convenience methods below repeat this check and then call the sized native
initializer with `Marshal.SizeOf` and API version `1`. A mismatch returns
`Result.ApiVersion` without asking an incompatible library to write the
structure.

```csharp
using System;
using System.Runtime.InteropServices;
using LibOot;

static void Check(Result result, string operation)
{
    if (result != Result.Ok)
        throw new InvalidOperationException(
            operation + ": " + Native.EngineResultString(result));
}

byte[] rom = System.IO.File.ReadAllBytes("oot.z64");
IntPtr engine = IntPtr.Zero;

try
{
    uint runtimeApi = Native.EngineApiVersionGet();
    if (runtimeApi != Native.EngineApiVersion)
        throw new NotSupportedException(
            "liboot API " + runtimeApi + " is incompatible with binding API " +
            Native.EngineApiVersion);

    EngineConfig config = new EngineConfig();
    Check(Native.EngineConfigInit(ref config), "initialize config");

    GCHandle romPin = GCHandle.Alloc(rom, GCHandleType.Pinned);
    try
    {
        config.RomData = romPin.AddrOfPinnedObject();
        config.RomSize = new UIntPtr((ulong)rom.LongLength);
        Check(Native.EngineCreate(ref config, out engine), "create engine");
    }
    finally
    {
        romPin.Free();
    }

    int nativeSceneResult;
    Check(Native.EngineSceneLoad(engine, Scene.DekuTree, 0,
        out nativeSceneResult), "load collision scene");
    Check(Native.EngineLinkCreate(engine, 0.0f, 0.0f, 0.0f), "create Link");
    Check(Native.EngineLinkSetAge(engine, Age.Adult), "set age");
    Check(Native.EngineLinkSetEquipment(engine, Sword.Master, Shield.Hylian,
        Tunic.Kokiri, Boots.Kokiri), "set equipment");
}
finally
{
    if (engine != IntPtr.Zero)
        Check(Native.EngineDestroy(engine), "destroy engine");
}
```

## AudioSeq playback

While an engine is alive, the native audio state is ready for ROM-backed
music, nature ambience, and the complete SFX selector. Call
`Native.AudioSequencePlay` or `Native.AudioNaturePlay` under the same lock as
your audio callback (and lock state getters too), then fill a pinned
interleaved stereo `float` buffer with
`Native.AudioRenderF32`. `Native.AudioSfxCatalogGet` initializes and returns a
fixed-size `SfxInfo` record for selector UIs. Stop rendering before destroying
the engine because audio state is process-global. Before starting the device,
call `Native.AudioSequencePrewarm` for the tracks it may play; this keeps ROM
decode/allocation work out of first-play device locks.

Keep the ROM pinned until `EngineCreate` returns; liboot copies the bytes during
creation. Keep managed callback delegates strongly referenced for as long as
native code can invoke their function pointers.

Load a static world or successful ROM scene before creating Link. Destroy the
engine outside callbacks; `Result.Busy` means it is still live and ownership
must be retained for a later retry.

`Native.EngineSceneGetRuntime` copies the active ROM scene and room behavior
into a managed `SceneRuntime` value. It returns `Result.NotAvailable` before a
ROM scene is loaded or after switching back to a custom static world.

Initialize input the same way before filling it for a frame:

```csharp
EngineInput input = new EngineInput();
Check(Native.EngineInputInit(ref input), "initialize input");
input.StickY = 1.0f;
input.Buttons = Buttons.Z;
```

`EngineConfigInitSized` and `EngineInputInitSized` expose the underlying ABI
entry points for hosts that manage structure sizes themselves. Callers using
them directly must perform the same `EngineApiVersionGet()` check first and
pass `Native.EngineApiVersion` as `apiVersion`.

## Gameplay values and native views

The public `Age`, `Sword`, `Shield`, `Tunic`, `Boots`, `Item`, `Scene`, and
`SfxAction` enums mirror `src/liboot.h`. Their underlying C# types match the
native ABI: byte-sized gameplay and sound-action values, and a signed 32-bit
scene index. The named `Scene` values are conveniences, not an exhaustive scene
list; the `int` overload of `EngineSceneLoad` accepts any valid game scene
index.

Frame, geometry, texture, actor, PCM, and callback pointers are native-owned
borrowed views. Copy data that must survive the next documented invalidating
call, and never free those pointers from managed code. An SFX callback receives
an `IntPtr` valid only for the duration of the callback; marshal it as
`SfxEvent` and enqueue a copy rather than calling back into the engine from the
callback.
