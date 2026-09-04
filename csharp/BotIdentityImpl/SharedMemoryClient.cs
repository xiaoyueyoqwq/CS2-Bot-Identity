using System.Runtime.InteropServices;
using System.Text;
using BotIdentityApi;

namespace BotIdentityImpl;

// Read-only client for the /dev/shm/CS2BotHider_Slots region that the
// BotIdentity native plugin writes. Field offsets must stay in sync
// with the native side (see src/shm_pub.h).
internal sealed class SharedMemoryClient : IDisposable
{
    // POSIX shm location. Same as BotHiderImpl's client.
    private const string PosixMappingPath = "/dev/shm/CS2BotHider_Slots";
    private const string WindowsMappingName = "Local\\CS2BotHider_Slots";
    private const uint Magic = 0x44494842; // 'BHID'
    private const uint Version = 1;
    private const int MaxSlots = 64;
    private const int NameLen = 32;

    // Offsets, must match native slot_shm.h and src/shm_pub.h.
    private const int OffMagic = 0;
    private const int OffVersion = 4;
    private const int OffMaxSlots = 8;
    private const int OffSlotState = 16;
    private const int OffSyntheticSid = 80;
    private const int OffPersonaName = 592;
    private const int OffIncarnation = 13216;
    private const int TotalSize = 1_064_960;

    private System.IO.MemoryMappedFiles.MemoryMappedFile? _mmf;
    private System.IO.MemoryMappedFiles.MemoryMappedViewAccessor? _view;

    public bool TryConnect()
    {
        if (_view != null) return true;
        try
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                _mmf = System.IO.MemoryMappedFiles.MemoryMappedFile.OpenExisting(
                    WindowsMappingName,
                    System.IO.MemoryMappedFiles.MemoryMappedFileRights.ReadWrite);
            }
            else
            {
                _mmf = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateFromFile(
                    PosixMappingPath, System.IO.FileMode.Open, null,
                    TotalSize, System.IO.MemoryMappedFiles.MemoryMappedFileAccess.ReadWrite);
            }
            _view = _mmf.CreateViewAccessor(0, TotalSize,
                System.IO.MemoryMappedFiles.MemoryMappedFileAccess.ReadWrite);

            if (_view.ReadUInt32(OffMagic) != Magic ||
                _view.ReadUInt32(OffVersion) != Version ||
                _view.ReadUInt32(OffMaxSlots) != MaxSlots)
            {
                Dispose();
                return false;
            }
            return true;
        }
        catch (System.IO.FileNotFoundException) { return false; }
        catch (System.IO.IOException) { return false; }
        catch (Exception) { Dispose(); return false; }
    }

    public void Dispose()
    {
        _view?.Dispose();
        _view = null;
        _mmf?.Dispose();
        _mmf = null;
    }

    private bool Valid(int slot)
    {
        if (_view == null) TryConnect();
        return _view != null && slot >= 0 && slot < MaxSlots;
    }

    public bool IsConnected => _view != null || TryConnect();

    public bool IsManagedBot(int slot) =>
        Valid(slot) && _view!.ReadByte(OffSlotState + slot) != 0;

    public ulong GetBotSteamId(int slot) =>
        Valid(slot) ? _view!.ReadUInt64(OffSyntheticSid + slot * 8) : 0UL;

    public ulong GetSlotIncarnation(int slot) =>
        Valid(slot) ? _view!.ReadUInt64(OffIncarnation + slot * 8) : 0UL;

    public string GetPersonaName(int slot)
    {
        if (!Valid(slot)) return string.Empty;
        var buf = new byte[NameLen];
        _view!.ReadArray(OffPersonaName + slot * NameLen, buf, 0, NameLen);
        int len = System.Array.IndexOf(buf, (byte)0);
        if (len < 0) len = NameLen;
        return Encoding.UTF8.GetString(buf, 0, len);
    }

    public int[] GetManagedSlots()
    {
        if (_view == null) TryConnect();
        if (_view == null) return System.Array.Empty<int>();
        var list = new System.Collections.Generic.List<int>();
        for (int s = 0; s < MaxSlots; s++)
        {
            if (_view.ReadByte(OffSlotState + s) == 0) continue;
            list.Add(s);
        }
        return list.ToArray();
    }

    public bool IsManagedBotIncarnation(int slot, ulong incarnation) =>
        IsManagedBot(slot) && GetSlotIncarnation(slot) == incarnation;

    public ManagedBotSnapshot[] GetManagedBotSnapshots()
    {
        var slots = GetManagedSlots();
        var result = new ManagedBotSnapshot[slots.Length];
        for (int i = 0; i < slots.Length; i++)
        {
            int s = slots[i];
            result[i] = new ManagedBotSnapshot
            {
                Slot = s,
                Incarnation = GetSlotIncarnation(s),
                Connected = true, // We only snapshot slots in the managed set
                IdentityMode = BotIdentityMode.Player, // BotIdentity is always "player"
                SteamId = GetBotSteamId(s),
                PersonaName = GetPersonaName(s),
            };
        }
        return result;
    }

    public BotIdentityMode GetIdentityMode() => BotIdentityMode.Player;
}
