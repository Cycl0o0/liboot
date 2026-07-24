// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Cycl0o0

using System;
using System.Runtime.InteropServices;

namespace LibOot
{
    public enum Result : int
    {
        Ok = 0,
        InvalidArgument = -1,
        ApiVersion = -2,
        OutOfMemory = -3,
        SingletonInUse = -4,
        RomUnsupported = -5,
        NotInitialized = -6,
        Busy = -7,
        LinkAlreadyExists = -8,
        LinkNotFound = -9,
        AgeRestricted = -10,
        TargetCapacity = -11,
        TargetNotFound = -12,
        SceneLoadFailed = -13,
        SceneGeometryUnavailable = -14,
        NoFrame = -15,
        NotAvailable = -16,
    }

    public enum Age : byte
    {
        Adult = 0,
        Child = 1,
    }

    public enum Sword : byte
    {
        None = 0,
        Kokiri = 1,
        Master = 2,
        Biggoron = 3,
    }

    public enum Shield : byte
    {
        None = 0,
        Deku = 1,
        Hylian = 2,
        Mirror = 3,
    }

    public enum Tunic : byte
    {
        Kokiri = 0,
        Goron = 1,
        Zora = 2,
    }

    public enum Boots : byte
    {
        Kokiri = 0,
        Iron = 1,
        Hover = 2,
    }

    public enum Item : byte
    {
        None = 0,
        Ocarina = 1,
        Bottle = 2,
        Hammer = 3,
        DekuStick = 4,
        Boomerang = 5,
        Bow = 6,
        Hookshot = 7,
        Bomb = 8,
    }

    public enum Scene : int
    {
        DekuTree = 0x00,
        LinksHouse = 0x34,
        TempleOfTime = 0x43,
        HyruleField = 0x51,
        KakarikoVillage = 0x52,
        KokiriForest = 0x55,
        ZorasDomain = 0x58,
        OutsideGanonsCastle = 0x64,
    }

    public enum SfxAction : byte
    {
        Play = 0,
        StopId = 1,
        StopPosition = 2,
    }

    // Mirrors the four retail Zelda AudioSeq players.
    public enum AudioPlayer : byte
    {
        Main = 0,
        Fanfare = 1,
        Sfx = 2,
        Sub = 3,
        Count = 4,
    }

    [Flags]
    public enum RenderFlags : uint
    {
        None = 0,
        Navi = 1u << 0,
        Actors = 1u << 1,
    }

    [Flags]
    public enum Buttons : uint
    {
        None = 0,
        A = 1u << 0,
        B = 1u << 1,
        Z = 1u << 2,
        R = 1u << 3,
        Item = 1u << 4,
        CUp = 1u << 5,
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void DebugCallback(IntPtr userData, IntPtr message);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SfxCallback(IntPtr userData, IntPtr sfxEvent);

    [StructLayout(LayoutKind.Sequential)]
    public struct EngineConfig
    {
        public uint StructSize;
        public uint ApiVersion;
        public IntPtr RomData;
        public UIntPtr RomSize;
        public uint ActorCapacity;
        public uint MaxSubsteps;
        public float FixedStepSeconds;
        public RenderFlags RenderFlags;
        public IntPtr DebugCallback;
        public IntPtr DebugUserData;
        public IntPtr SfxCallback;
        public IntPtr SfxUserData;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct EngineInput
    {
        public uint StructSize;
        public float CamLookX;
        public float CamLookZ;
        public float StickX;
        public float StickY;
        public Buttons Buttons;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct Surface
    {
        public ushort Type;
        public fixed int Vertices[9];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WaterBox
    {
        public short XMin;
        public short ZMin;
        public short XLength;
        public short ZLength;
        public short YSurface;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct SfxEvent
    {
        public ushort SfxId;
        public byte Token;
        public sbyte Reverb;
        public SfxAction Action;
        public byte IsRefresh;
        public fixed byte Reserved[2];
        public fixed float Position[3];
        public float FrequencyScale;
        public float Volume;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct SequenceInfo
    {
        public uint StructSize;
        public uint Version;
        public ushort SequenceId;
        public ushort ResolvedId;
        public uint DataSize;
        public byte FontCount;
        public byte IsAlias;
        public byte Medium;
        public byte CachePolicy;
        public fixed byte FontIds[8];
        public uint Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct AudioState
    {
        public uint StructSize;
        public uint Version;
        public ushort SequenceId;
        public ushort ResolvedId;
        public AudioPlayer Player;
        public byte Playing;
        public byte Paused;
        public byte Finished;
        public byte ActiveChannels;
        public byte ActiveVoices;
        public fixed byte Reserved[2];
        public float Volume;
        public ulong FramesRendered;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct SfxInfo
    {
        public uint StructSize;
        public uint Version;
        public ushort SfxId;
        public byte Bank;
        public byte Reserved8;
        public ushort BankIndex;
        public ushort Reserved16;
        public fixed byte Name[48];
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ActorInfo
    {
        public short Id;
        public short Category;
        public short Params;
        public short Yaw;
        public byte Active;
        public fixed float Position[3];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct EngineGeometry
    {
        public uint StructSize;
        public IntPtr Position;
        public IntPtr Normal;
        public IntPtr Color;
        public IntPtr Uv;
        public IntPtr TriangleTexture;
        public uint TriangleCount;
        public uint TriangleCapacity;
        public IntPtr Alpha;   // liboot vNEXT: 1 float/vertex shade alpha (parallel to Color)
        public IntPtr TriFlags; // liboot vNEXT: 1 byte/triangle render flags (parallel to TriangleTexture)
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct EngineLinkState
    {
        public uint StructSize;
        public fixed float Position[3];
        public fixed float Velocity[3];
        public short FaceAngle;
        public short Health;
        public short HealthCapacity;
        public short Magic;
        public float LinearVelocity;
        public float AnimationFrame;
        public uint StateFlags1;
        public uint StateFlags2;
        public byte MagicLevel;
        public Age Age;
        public byte IsDead;
        public sbyte HeldItemAction;
        public byte MeleeWeaponState;
        public byte LockOnActive;
        public byte InWater;
        public byte Reserved0;
        public fixed float LockOnPosition[3];
        public float WaterSurfaceY;
        // liboot vNEXT: appended state (all fields above are unchanged).
        public uint Action;          // enum OoTAction
        public short LookPitch;      // head/aim pitch, binary angle
        public short LookYaw;        // head/aim yaw, binary angle
        public ushort FloorSfxOffset;
        public byte AttackAnim;      // PLAYER_MWA_* swing id; valid while MeleeWeaponState != 0
        public byte StateFlags3;
        public ushort UnderwaterTimer; // 0..300 while submerged; host drives its own air meter
        public short AnimationId;    // stable 1-based link_animetion entry; 0 unknown
    }

    // liboot vNEXT: mirrors struct OoTSurfaceInfo (oot_engine_scene_query_surface).
    public struct SurfaceInfo
    {
        public float GroundY;
        public uint FloorType;
        public uint Material;
        public byte Hookshot;
    }

    // liboot vNEXT: mirrors struct OoTDoor (transition actor / door).
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct Door
    {
        public short FrontRoom;
        public short BackRoom;
        public short ActorId;
        public short Yaw;
        public fixed float Position[3];
    }

    // liboot vNEXT: mirrors struct OoTSceneEnvironment (scene light/fog settings).
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct SceneEnvironment
    {
        public fixed float AmbientColor[3];
        public fixed float Light1Dir[3];
        public fixed float Light1Color[3];
        public fixed float Light2Dir[3];
        public fixed float Light2Color[3];
        public fixed float FogColor[3];
        public float FogNear;
        public float FogFar;
        public byte Valid;
    }

    // Live OoT scene/room fields that drive Player behavior.
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct SceneRuntime
    {
        public uint StructSize;
        public uint Version;
        public int SceneIndex;
        public int ActiveRoomIndex;
        public int GeometryRoomIndex;
        public int RoomCount;
        public short WorldMapArea;
        public byte RoomType;
        public byte EnvironmentType;
        public sbyte Echo;
        public byte LensMode;
        public byte WarpSongsDisabled;
        public byte SceneCamType;
        public byte AllRoomsLoaded;
        public byte RoomMetadataValid;
        public fixed byte Reserved[2];
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct EngineNaviState
    {
        public uint StructSize;
        public byte Available;
        public fixed byte Reserved[3];
        public fixed float Position[3];
        public fixed float InnerColor[4];
        public fixed float OuterColor[4];
        public float Scale;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct SkeletonPose
    {
        public byte JointCount;
        public fixed byte Parent[21];
        public fixed float JointPosition[21 * 3];
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct EngineFrame
    {
        public uint StructSize;
        public ulong SimulationTick;
        public float FixedStepSeconds;
        public float InterpolationAlpha;
        public EngineLinkState Link;
        public EngineGeometry Geometry;
        public IntPtr Actors;
        public uint ActorCount;
        public uint ActorCapacity;
        public byte ActorListTruncated;
        public byte SkeletonAvailable;
        public fixed byte Reserved[2];
        public SkeletonPose Skeleton;
        public EngineNaviState Navi;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct EngineSceneGeometry
    {
        public uint StructSize;
        public IntPtr Position;
        public IntPtr Normal;
        public IntPtr Color;
        public IntPtr Uv;
        public IntPtr TriangleTexture;
        public uint TriangleCount;
        public uint TranslucentStartTriangle;
        public uint TriangleCapacity;
        public IntPtr Alpha;   // liboot vNEXT: 1 float/vertex shade alpha (parallel to Color)
        public IntPtr TriFlags; // liboot vNEXT: 1 byte/triangle render flags (parallel to TriangleTexture)
    }

    // liboot vNEXT: mirrors enum OoTTriangleFlags (per-triangle render flags).
    [System.Flags]
    public enum TriangleFlags : byte
    {
        None      = 0,
        CullFront = 1 << 0,
        CullBack  = 1 << 1,
        AlphaTest = 1 << 2,
        Decal     = 1 << 3,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct EngineTexture
    {
        public uint StructSize;
        public ushort Width;
        public ushort Height;
        public byte WrapS;
        public byte WrapT;
        public ushort Reserved;
        public uint Revision;
        public IntPtr RgbaPixels;
        public UIntPtr RgbaSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct EnginePcm
    {
        public uint StructSize;
        public IntPtr Samples;
        public uint SampleCount;
        public uint SampleRate;
        public uint LoopStart;
    }

    public static unsafe class Native
    {
        public const string LibraryName = "liboot";
        public const uint EngineApiVersion = 1;
        public const float DefaultFixedStep = 1.0f / 20.0f;
        public const uint InvalidTarget = 0;
        public const ushort AudioSequenceCatalogCount = 110;
        public const ushort AudioNoMusic = 0x7F;
        public const ushort AudioNatureRain = 0x80;
        public const byte AudioNatureCount = 19;
        public const byte AudioNatureNone = 0x13;
        public const uint SequenceInfoVersion = 1;
        public const uint AudioStateVersion = 1;
        public const uint SfxInfoVersion = 1;

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_api_version")]
        public static extern uint EngineApiVersionGet();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_result_string")]
        private static extern IntPtr EngineResultStringNative(Result result);

        public static string EngineResultString(Result result)
        {
            return Marshal.PtrToStringAnsi(EngineResultStringNative(result))
                ?? "unknown result";
        }

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_count")]
        public static extern int AudioSequenceCount();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_name")]
        private static extern IntPtr AudioSequenceNameNative(ushort sequenceId);

        public static string AudioSequenceName(ushort sequenceId)
        {
            return Marshal.PtrToStringAnsi(AudioSequenceNameNative(sequenceId))
                ?? "UNKNOWN";
        }

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_get_info")]
        [return: MarshalAs(UnmanagedType.I1)]
        private static extern bool AudioSequenceGetInfoNative(ushort sequenceId,
            ref SequenceInfo info);

        public static bool AudioSequenceGetInfo(ushort sequenceId,
            out SequenceInfo info)
        {
            info = new SequenceInfo();
            info.StructSize = (uint)Marshal.SizeOf(typeof(SequenceInfo));
            info.Version = SequenceInfoVersion;
            return AudioSequenceGetInfoNative(sequenceId, ref info);
        }

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_prewarm")]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool AudioSequencePrewarm(ushort sequenceId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_play")]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool AudioSequencePlay(AudioPlayer player,
            ushort sequenceId, ushort fadeInMs);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_nature_play")]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool AudioNaturePlay(AudioPlayer player,
            byte ambienceId, ushort fadeInMs);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_stop")]
        public static extern void AudioSequenceStop(AudioPlayer player,
            ushort fadeOutMs);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_pause")]
        public static extern void AudioSequencePause(AudioPlayer player,
            [MarshalAs(UnmanagedType.I1)] bool paused);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_set_volume")]
        public static extern void AudioSequenceSetVolume(AudioPlayer player,
            float volume);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_set_io")]
        public static extern void AudioSequenceSetIo(AudioPlayer player,
            byte port, sbyte value);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_channel_set_io")]
        public static extern void AudioChannelSetIo(AudioPlayer player,
            byte channel, byte port, sbyte value);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sequence_get_state")]
        [return: MarshalAs(UnmanagedType.I1)]
        private static extern bool AudioSequenceGetStateNative(AudioPlayer player,
            ref AudioState state);

        public static bool AudioSequenceGetState(AudioPlayer player,
            out AudioState state)
        {
            state = new AudioState();
            state.StructSize = (uint)Marshal.SizeOf(typeof(AudioState));
            state.Version = AudioStateVersion;
            return AudioSequenceGetStateNative(player, ref state);
        }

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_set_master_volume")]
        public static extern void AudioSetMasterVolume(float volume);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_stop_all")]
        public static extern void AudioStopAll(ushort fadeOutMs);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_render_f32")]
        public static extern uint AudioRenderF32(float* interleavedStereo,
            uint frames, uint sampleRate);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sfx_catalog_count")]
        public static extern int AudioSfxCatalogCount();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sfx_catalog_get")]
        [return: MarshalAs(UnmanagedType.I1)]
        private static extern bool AudioSfxCatalogGetNative(int catalogIndex,
            ref SfxInfo info);

        public static bool AudioSfxCatalogGet(int catalogIndex, out SfxInfo info)
        {
            info = new SfxInfo();
            info.StructSize = (uint)Marshal.SizeOf(typeof(SfxInfo));
            info.Version = SfxInfoVersion;
            return AudioSfxCatalogGetNative(catalogIndex, ref info);
        }

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sfx_play")]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool AudioSfxPlay(ushort sfxId, float pan,
            float volume);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sfx_stop")]
        public static extern void AudioSfxStop(ushort sfxId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_audio_sfx_stop_all")]
        public static extern void AudioSfxStopAll();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_config_init_sized")]
        public static extern Result EngineConfigInitSized(ref EngineConfig config,
            uint structSize, uint apiVersion);

        public static Result EngineConfigInit(ref EngineConfig config)
        {
            if (EngineApiVersionGet() != EngineApiVersion)
                return Result.ApiVersion;

            return EngineConfigInitSized(ref config,
                (uint)Marshal.SizeOf(typeof(EngineConfig)), EngineApiVersion);
        }

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_input_init_sized")]
        public static extern Result EngineInputInitSized(ref EngineInput input,
            uint structSize, uint apiVersion);

        public static Result EngineInputInit(ref EngineInput input)
        {
            if (EngineApiVersionGet() != EngineApiVersion)
                return Result.ApiVersion;

            return EngineInputInitSized(ref input,
                (uint)Marshal.SizeOf(typeof(EngineInput)), EngineApiVersion);
        }

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_create")]
        public static extern Result EngineCreate(ref EngineConfig config,
            out IntPtr engine);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_destroy")]
        public static extern Result EngineDestroy(IntPtr engine);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_set_callbacks")]
        public static extern Result EngineSetCallbacks(IntPtr engine,
            IntPtr debugCallback, IntPtr debugUserData,
            IntPtr sfxCallback, IntPtr sfxUserData);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_create")]
        public static extern Result EngineLinkCreate(IntPtr engine,
            float x, float y, float z);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_delete")]
        public static extern Result EngineLinkDelete(IntPtr engine);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_set_age")]
        public static extern Result EngineLinkSetAge(IntPtr engine, Age age);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_set_equipment")]
        public static extern Result EngineLinkSetEquipment(IntPtr engine,
            Sword sword, Shield shield, Tunic tunic, Boots boots);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_use_item")]
        public static extern Result EngineLinkUseItem(IntPtr engine, Item item);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_set_health")]
        public static extern Result EngineLinkSetHealth(IntPtr engine,
            short health, short capacity);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_damage")]
        public static extern Result EngineLinkDamage(IntPtr engine, short amount);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_set_magic")]
        public static extern Result EngineLinkSetMagic(IntPtr engine,
            byte level, short amount);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_set_pose")]
        public static extern Result EngineLinkSetPose(IntPtr engine,
            float x, float y, float z, short yaw);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_freeze")]
        public static extern Result EngineLinkFreeze(IntPtr engine, byte frozen);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_link_set_invincible")]
        public static extern Result EngineLinkSetInvincible(IntPtr engine, sbyte frames);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_query_surface")]
        public static extern Result EngineSceneQuerySurface(IntPtr engine,
            float x, float y, float z, out SurfaceInfo info);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_step")]
        public static extern Result EngineStep(IntPtr engine,
            EngineInput* input, out IntPtr frame);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_advance")]
        public static extern Result EngineAdvance(IntPtr engine,
            float elapsedSeconds, EngineInput* input, out uint steps,
            out IntPtr frame);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_get_frame")]
        public static extern Result EngineGetFrame(IntPtr engine,
            out IntPtr frame);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_reset_clock")]
        public static extern Result EngineResetClock(IntPtr engine);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_static_world_load")]
        public static extern Result EngineStaticWorldLoad(IntPtr engine,
            Surface* surfaces, uint surfaceCount, WaterBox* waterBoxes,
            uint waterBoxCount);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_load")]
        public static extern Result EngineSceneLoad(IntPtr engine,
            int sceneIndex, int roomIndex, out int nativeResult);

        public static Result EngineSceneLoad(IntPtr engine, Scene scene,
            int roomIndex, out int nativeResult)
        {
            return EngineSceneLoad(engine, (int)scene, roomIndex,
                out nativeResult);
        }

        // liboot vNEXT: door-driven room transitions.
        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_set_room")]
        public static extern Result EngineSceneSetRoom(IntPtr engine,
            int roomIndex, out int nativeResult);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_get_door_count")]
        public static extern Result EngineSceneGetDoorCount(IntPtr engine, out uint count);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_get_door")]
        public static extern Result EngineSceneGetDoor(IntPtr engine, uint index, out Door door);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_get_sequence_id")]
        public static extern Result EngineSceneGetSequenceId(IntPtr engine, out int seqId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_get_ambience_id")]
        public static extern Result EngineSceneGetAmbienceId(IntPtr engine, out int ambienceId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_get_environment")]
        public static extern Result EngineSceneGetEnvironment(IntPtr engine, out SceneEnvironment env);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_get_runtime")]
        public static extern Result EngineSceneGetRuntime(IntPtr engine, out SceneRuntime runtime);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_get_geometry")]
        public static extern Result EngineSceneGetGeometry(IntPtr engine,
            out IntPtr geometry);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_scene_get_spawn")]
        public static extern Result EngineSceneGetSpawn(IntPtr engine,
            int spawnIndex, float* position, short* yaw);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_target_create")]
        public static extern Result EngineTargetCreate(IntPtr engine,
            float x, float y, float z, float focusHeight, out uint target);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_target_move")]
        public static extern Result EngineTargetMove(IntPtr engine,
            uint target, float x, float y, float z);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_target_remove")]
        public static extern Result EngineTargetRemove(IntPtr engine,
            uint target);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_targets_clear")]
        public static extern Result EngineTargetsClear(IntPtr engine);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_set_render_flags")]
        public static extern Result EngineSetRenderFlags(IntPtr engine,
            RenderFlags flags);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_get_render_flags")]
        public static extern Result EngineGetRenderFlags(IntPtr engine,
            out RenderFlags flags);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_texture_count")]
        public static extern Result EngineTextureCount(IntPtr engine,
            out uint count);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_texture_get")]
        public static extern Result EngineTextureGet(IntPtr engine,
            uint index, ref EngineTexture texture);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_voice_get")]
        public static extern Result EngineVoiceGet(IntPtr engine,
            ushort sfxId, ref EnginePcm pcm);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_engine_ocarina_note_get")]
        public static extern Result EngineOcarinaNoteGet(IntPtr engine,
            byte noteIndex, ref EnginePcm pcm);

        // Stateless ocarina-song helpers (raw API — no engine handle needed).
        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_ocarina_song_notes")]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool OcarinaSongNotes(int song, byte* outNotes, out int outCount);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl,
            EntryPoint = "oot_ocarina_match")]
        public static extern int OcarinaMatch(byte* notes, int count);
    }

    // Mirrors enum OoTOcarinaSong (indices are the ABI contract, do not reorder).
    public enum OcarinaSong
    {
        Minuet = 0,   // warp: Sacred Forest Meadow
        Bolero,       // warp: Death Mountain Crater
        Serenade,     // warp: Lake Hylia
        Requiem,      // warp: Desert Colossus
        Nocturne,     // warp: Graveyard
        Prelude,      // warp: Temple of Time
        Sarias,
        Eponas,
        Lullaby,
        Suns,
        Time,
        Storms,
        Count
    }
}
