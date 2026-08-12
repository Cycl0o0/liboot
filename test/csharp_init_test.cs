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
                (IntPtr.Size == 8 ? 80 : 56), "EngineConfig ABI size");
        Require(Marshal.SizeOf(typeof(EngineInput)) == 24,
                "EngineInput ABI size");
        Require(Marshal.SizeOf(typeof(EngineLimits)) == 72,
                "EngineLimits ABI size");
        Require(Marshal.OffsetOf(typeof(EngineLimits), "CapabilityFlags").ToInt32() == 8 &&
                Marshal.OffsetOf(typeof(EngineLimits), "LinkTriangleCapacity").ToInt32() == 16 &&
                Marshal.OffsetOf(typeof(EngineLimits), "DefaultMaxSubsteps").ToInt32() == 60 &&
                Marshal.OffsetOf(typeof(EngineLimits), "MaxLinkTriangleCapacity").ToInt32() == 64,
                "EngineLimits ABI offsets");
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
        Require(Marshal.SizeOf(typeof(GeometryBatch)) == 96,
                "GeometryBatch ABI size");
        Require(Marshal.OffsetOf(typeof(GeometryBatch), "PrimitiveColor").ToInt32() == 44,
                "GeometryBatch.PrimitiveColor ABI offset");
        Require(Marshal.SizeOf(typeof(SceneRuntime)) == 36,
                "SceneRuntime ABI size");
        Require(Marshal.OffsetOf(typeof(SceneRuntime), "ActiveRoomIndex").ToInt32() == 12 &&
                Marshal.OffsetOf(typeof(SceneRuntime), "RoomType").ToInt32() == 26,
                "SceneRuntime ABI offsets");
        Require(Marshal.SizeOf(typeof(SceneLoadOptions)) == 16,
                "SceneLoadOptions ABI size");
        Require(Marshal.SizeOf(typeof(DynamicCollisionTransform)) == 36 &&
                Marshal.SizeOf(typeof(DynamicCollisionState)) == 48,
                "dynamic collision ABI sizes");
        Require(Marshal.SizeOf(typeof(HostActorState)) == 72 &&
                Marshal.SizeOf(typeof(EngineActorContact)) == 48 &&
                Marshal.SizeOf(typeof(SceneActorEntry)) == 36,
                "actor ABI sizes");
        Require(Marshal.SizeOf(typeof(WorldEvent)) == 48,
                "WorldEvent ABI size");
        Require(Marshal.SizeOf(typeof(SceneMaterialState)) == 32 &&
                Marshal.SizeOf(typeof(SceneMaterialReference)) == 28,
                "scene material ABI sizes");
        Require(Marshal.SizeOf(typeof(SceneBackground)) ==
                (IntPtr.Size == 8 ? 72 : 56),
                "SceneBackground ABI size");
        if (IntPtr.Size == 8)
        {
            Require(Marshal.SizeOf(typeof(EngineFrame)) == 560,
                    "EngineFrame ABI size");
            Require(Marshal.OffsetOf(typeof(EngineFrame), "LinkGeometryTruncated").ToInt32() == 210 &&
                    Marshal.OffsetOf(typeof(EngineFrame), "Reserved0").ToInt32() == 211 &&
                    Marshal.OffsetOf(typeof(EngineFrame), "Skeleton").ToInt32() == 212 &&
                    Marshal.OffsetOf(typeof(EngineFrame), "GeometryBatches").ToInt32() == 544,
                    "EngineFrame truncation ABI offsets");
            Require(Marshal.SizeOf(typeof(EngineSceneGeometry)) == 96 &&
                    Marshal.OffsetOf(typeof(EngineSceneGeometry), "Batches").ToInt32() == 80,
                    "EngineSceneGeometry batch ABI layout");
        }
        Require(Enum.GetUnderlyingType(typeof(Age)) == typeof(byte) &&
                Enum.GetUnderlyingType(typeof(SfxAction)) == typeof(byte) &&
                Enum.GetUnderlyingType(typeof(AudioPlayer)) == typeof(byte) &&
                Enum.GetUnderlyingType(typeof(SceneLayer)) == typeof(byte) &&
                Enum.GetUnderlyingType(typeof(WorldEventKind)) == typeof(uint) &&
                Enum.GetUnderlyingType(typeof(Scene)) == typeof(int) &&
                Enum.GetUnderlyingType(typeof(EngineCapabilities)) == typeof(ulong),
                "enum ABI widths");

        EngineCapabilities expectedCapabilities =
            EngineCapabilities.StaticWorld |
            EngineCapabilities.RomSceneLoading |
            EngineCapabilities.LinkGeometry |
            EngineCapabilities.SceneGeometry |
            EngineCapabilities.GeometryTruncation |
            EngineCapabilities.ActorQuery |
            EngineCapabilities.Targets |
            EngineCapabilities.Textures |
            EngineCapabilities.FixedStep |
            EngineCapabilities.Audio |
            EngineCapabilities.DynamicCollision |
            EngineCapabilities.GeometryBatches |
            EngineCapabilities.HostActors |
            EngineCapabilities.SceneActorCatalog |
            EngineCapabilities.MultiInstance |
            EngineCapabilities.SceneLayers |
            EngineCapabilities.SceneTransitions |
            EngineCapabilities.SceneBackgrounds |
            EngineCapabilities.SceneMaterialMetadata;
        EngineLimits badLimits = new EngineLimits();
        badLimits.StructSize = (uint)Marshal.SizeOf(typeof(EngineLimits));
        badLimits.Version = Native.EngineLimitsVersion + 1u;
        badLimits.CapabilityFlags = (EngineCapabilities)ulong.MaxValue;
        Require(Native.EngineGetLimits(ref badLimits) == Result.ApiVersion &&
                badLimits.CapabilityFlags == (EngineCapabilities)ulong.MaxValue,
                "mismatched limits query wrote output");

        EngineLimits limits;
        Require(Native.GetEngineLimits(out limits) == Result.Ok,
                "limits query");
        Require(limits.StructSize == (uint)Marshal.SizeOf(typeof(EngineLimits)) &&
                limits.Version == Native.EngineLimitsVersion &&
                limits.CapabilityFlags == expectedCapabilities,
                "limits tags and capabilities");
        Require(limits.LinkTriangleCapacity == 2048u &&
                limits.SceneTriangleCapacity == 16384u &&
                limits.StaticSurfaceCapacity == 2730u &&
                limits.WaterBoxCapacity == 65535u &&
                limits.MaxActorCapacity == 4096u &&
                limits.TargetCapacity == 16u &&
                limits.TextureCapacity == 1024u &&
                limits.MaxLinkTriangleCapacity == 1048576u &&
                limits.MaxSceneTriangleCapacity == 1048576u,
                "limits capacities");
        Require(limits.MaxSubsteps == 1000u &&
                Math.Abs(limits.MinFixedStepSeconds - 0.001f) < 0.000001f &&
                Math.Abs(limits.MaxFixedStepSeconds - 1.0f) < 0.000001f &&
                Math.Abs(limits.DefaultFixedStepSeconds - 0.06f) < 0.000001f &&
                limits.DefaultMaxSubsteps == 4u,
                "limits time-step values");

        uint droppedTriangles = 0xA5A5A5A5u;
        Require(Native.EngineSceneGetDroppedTriangles(IntPtr.Zero,
                    out droppedTriangles) == Result.InvalidArgument &&
                droppedTriangles == 0u,
                "scene truncation invalid-engine contract");

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
                Math.Abs(config.FixedStepSeconds - 0.06f) < 0.000001f &&
                config.LinkTriangleCapacity == 2048u &&
                config.SceneTriangleCapacity == 16384u,
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
