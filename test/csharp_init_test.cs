// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Cycl0o0

using System;
using System.Runtime.InteropServices;
using LibOot;

internal static class CSharpInitTest
{
    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    public static int Main()
    {
        uint runtimeApi = Native.EngineApiVersionGet();
        Require(runtimeApi == Native.EngineApiVersion, "runtime API mismatch");
        Require(Marshal.SizeOf(typeof(EngineConfig)) ==
                (IntPtr.Size == 8 ? 72 : 48), "EngineConfig ABI size");
        Require(Marshal.SizeOf(typeof(EngineInput)) == 24,
                "EngineInput ABI size");
        Require(Marshal.SizeOf(typeof(EngineLinkState)) == 92,
                "EngineLinkState ABI size");
        Require(Marshal.OffsetOf(typeof(EngineLinkState), "AnimationId").ToInt32() == 90,
                "EngineLinkState.AnimationId ABI offset");
        Require(Marshal.SizeOf(typeof(Surface)) == 40, "Surface ABI size");
        Require(Marshal.SizeOf(typeof(WaterBox)) == 10, "WaterBox ABI size");
        Require(Marshal.SizeOf(typeof(SfxEvent)) == 28, "SfxEvent ABI size");
        Require(Marshal.SizeOf(typeof(SequenceInfo)) == 32,
                "SequenceInfo ABI size");
        Require(Marshal.SizeOf(typeof(AudioState)) == 32,
                "AudioState ABI size");
        Require(Marshal.OffsetOf(typeof(AudioState), "FramesRendered").ToInt32() == 24,
                "AudioState.FramesRendered ABI offset");
        Require(Marshal.SizeOf(typeof(SfxInfo)) == 64,
                "SfxInfo ABI size");
        Require(Marshal.SizeOf(typeof(ActorInfo)) == 24, "ActorInfo ABI size");
        Require(Marshal.SizeOf(typeof(SceneRuntime)) == 36,
                "SceneRuntime ABI size");
        Require(Marshal.OffsetOf(typeof(SceneRuntime), "ActiveRoomIndex").ToInt32() == 12 &&
                Marshal.OffsetOf(typeof(SceneRuntime), "RoomType").ToInt32() == 26,
                "SceneRuntime ABI offsets");
        Require(Enum.GetUnderlyingType(typeof(Age)) == typeof(byte) &&
                Enum.GetUnderlyingType(typeof(SfxAction)) == typeof(byte) &&
                Enum.GetUnderlyingType(typeof(AudioPlayer)) == typeof(byte) &&
                Enum.GetUnderlyingType(typeof(Scene)) == typeof(int),
                "enum ABI widths");

        SequenceInfo sequenceInfo;
        Require(!Native.AudioSequenceGetInfo(0, out sequenceInfo) &&
                sequenceInfo.StructSize == (uint)Marshal.SizeOf(typeof(SequenceInfo)) &&
                sequenceInfo.Version == Native.SequenceInfoVersion,
                "sequence info initialization before engine creation");
        Require(!Native.AudioSequencePrewarm(0),
                "sequence prewarm before engine creation");
        AudioState audioState;
        Require(Native.AudioSequenceGetState(AudioPlayer.Main, out audioState) &&
                audioState.StructSize == (uint)Marshal.SizeOf(typeof(AudioState)) &&
                audioState.Version == Native.AudioStateVersion,
                "audio state initialization before engine creation");
        SfxInfo sfxInfo;
        Require(Native.AudioSfxCatalogGet(0, out sfxInfo) &&
                sfxInfo.StructSize == (uint)Marshal.SizeOf(typeof(SfxInfo)) &&
                sfxInfo.Version == Native.SfxInfoVersion,
                "SFX info initialization before engine creation");

        EngineConfig config = new EngineConfig();
        config.StructSize = 0xA5A5A5A5u;
        Result mismatch = Native.EngineConfigInitSized(ref config,
            (uint)Marshal.SizeOf(typeof(EngineConfig)),
            Native.EngineApiVersion + 1u);
        Require(mismatch == Result.ApiVersion &&
                config.StructSize == 0xA5A5A5A5u,
                "mismatched config initializer wrote output");
        Require(Native.EngineConfigInit(ref config) == Result.Ok,
                "config initializer");
        Require(config.StructSize == (uint)Marshal.SizeOf(typeof(EngineConfig)) &&
                config.ApiVersion == Native.EngineApiVersion &&
                config.ActorCapacity == 64u && config.MaxSubsteps == 4u &&
                Math.Abs(config.FixedStepSeconds - 0.05f) < 0.000001f,
                "config defaults");

        EngineInput input = new EngineInput();
        Require(Native.EngineInputInit(ref input) == Result.Ok,
                "input initializer");
        Require(input.StructSize == (uint)Marshal.SizeOf(typeof(EngineInput)) &&
                input.CamLookX == 0.0f && input.CamLookZ == 1.0f &&
                input.StickX == 0.0f && input.StickY == 0.0f &&
                input.Buttons == Buttons.None, "input defaults");

        Console.WriteLine("C# ABI/init: PASS");
        return 0;
    }
}
