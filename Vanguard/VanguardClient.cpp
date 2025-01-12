// A basic test implementation of Netcore for IPC in Dolphin

#pragma warning(disable : 4564)


#include <string>

#include "VanguardClient.h"
#include "VanguardClientInitializer.h"
#include "Helpers.hpp"
//#include "../core/emulator.h"
#include <msclr/marshal_cppstd.h>
//#include <../core/hw/sh4/sh4_mem.h>
#include "UnmanagedWrapper.h"
#include "VanguardSettingsWrapper.h"
#include "VanguardClient.h"
#include "ManagedWrapper.h"

//#include "core/core.h"
#using < system.dll>
#using < system.windows.forms.dll>
#using < system.collections.dll>

//If we provide just the dll name and then compile with /AI it works, but intellisense doesn't pick up on it, so we use a full relative path
#using <../../RTCV/Build/NetCore.dll>
#using <../../RTCV/Build/Vanguard.dll>
#using <../../RTCV/Build/CorruptCore.dll>
#using <../../RTCV/Build/RTCV.Common.dll>


using namespace cli;
using namespace System;
using namespace Text;
using namespace RTCV;
using namespace RTCV::CorruptCore::Extensions;
using namespace NetCore;
using namespace CorruptCore;
using namespace Vanguard;
using namespace Runtime::InteropServices;
using namespace Threading;
using namespace Collections::Generic;
using namespace Reflection;
using namespace Diagnostics;

#define SRAM_SIZE 25165824
#define ARAM_SIZE 16777216
#define EXRAM_SIZE 67108864

static void EmuThreadExecute(Action^ callback);
static void EmuThreadExecute(IntPtr ptr);

// Define this in here as it's managed and weird stuff happens if it's in a header
public
    ref class VanguardClient {
public:
    static NetCoreReceiver^ receiver;
    static VanguardConnector^ connector;

    static void OnMessageReceived(Object^ sender, NetCoreEventArgs^ e);
    static void SpecUpdated(Object^ sender, SpecUpdateEventArgs^ e);
    static void RegisterVanguardSpec();

    static void StartClient();
    static void RestartClient();
    static void StopClient();

    static bool VanguardInitializationComplete = false;
    static void LoadRom(String^ filename);
    static bool LoadState(std::string filename);
    static bool SaveState(String^ filename, bool wait);

    static String ^ GetConfigAsJson(VanguardSettingsWrapper ^ settings);
    static VanguardSettingsWrapper ^ GetConfigFromJson(String ^ json);

    static void LoadWindowPosition();
    static void SaveWindowPosition();
    static String^ GetSyncSettings();
    static void SetSyncSettings(String^ ss);

    static String^ emuDir = IO::Path::GetDirectoryName(Assembly::GetExecutingAssembly()->Location);
    static String^ logPath = IO::Path::Combine(emuDir, "EMU_LOG.txt");


    static cli::array<String^>^ configPaths;

    static volatile bool loading = false;
    static volatile bool stateLoading = false;
    static bool attached = false;
    static Object^ GenericLockObject = gcnew Object();
    static bool enableRTC = true;
    static System::String ^ lastStateName = "";
    static System::String ^ fileToCopy = "";
    //static Core::TimingEventType* event;
};

static void EmuThreadExecute(Action^ callback) {
    EmuThreadExecute(Marshal::GetFunctionPointerForDelegate(callback));
}

static void EmuThreadExecute(IntPtr callbackPtr) {
    //  main_window.SetEmuThread(false);
    static_cast<void(__stdcall*)(void)>(callbackPtr.ToPointer())();
    // main_window.SetEmuThread(true);
}

static PartialSpec^
getDefaultPartial() {
    PartialSpec^ partial = gcnew PartialSpec("VanguardSpec");
    partial->Set(VSPEC::NAME, "PPSSPP");
    partial->Set(VSPEC::SUPPORTS_RENDERING, false);
    partial->Set(VSPEC::SUPPORTS_CONFIG_MANAGEMENT, false);
    partial->Set(VSPEC::SUPPORTS_CONFIG_HANDOFF, true);
    partial->Set(VSPEC::SUPPORTS_KILLSWITCH, true);
    partial->Set(VSPEC::SUPPORTS_REALTIME, true);
    partial->Set(VSPEC::SUPPORTS_SAVESTATES, true);
    partial->Set(VSPEC::SUPPORTS_REFERENCES, true);
    partial->Set(VSPEC::SUPPORTS_MIXED_STOCKPILE, true);
    partial->Set(VSPEC::CONFIG_PATHS, VanguardClient::configPaths);
    partial->Set(VSPEC::SYSTEM, "PSP");
    partial->Set(VSPEC::GAMENAME, String::Empty);
    partial->Set(VSPEC::SYSTEMPREFIX, String::Empty);
    partial->Set(VSPEC::OPENROMFILENAME, "IGNORE");
    partial->Set(VSPEC::OVERRIDE_DEFAULTMAXINTENSITY, 100000);
    partial->Set(VSPEC::SYNCSETTINGS, String::Empty);
    partial->Set(VSPEC::MEMORYDOMAINS_BLACKLISTEDDOMAINS, gcnew cli::array<String ^>{"Raw System Bus", "VRAM"});
    partial->Set(VSPEC::SYSTEM, "PSP");
    partial->Set(VSPEC::LOADSTATE_USES_CALLBACKS, true);
    partial->Set(VSPEC::EMUDIR, VanguardClient::emuDir);
    return partial;
}

void VanguardClient::SpecUpdated(Object^ sender, SpecUpdateEventArgs^ e) {
    PartialSpec^ partial = e->partialSpec;

    LocalNetCoreRouter::Route(Endpoints::CorruptCore,
                              Commands::Remote::PushVanguardSpecUpdate, partial, true);
    LocalNetCoreRouter::Route(Endpoints::UI, Commands::Remote::PushVanguardSpecUpdate,
                              partial, true);
}

void VanguardClient::RegisterVanguardSpec() {
    PartialSpec^ emuSpecTemplate = gcnew PartialSpec("VanguardSpec");

    emuSpecTemplate->Insert(getDefaultPartial());

    AllSpec::VanguardSpec = gcnew FullSpec(emuSpecTemplate, true);
    // You have to feed a partial spec as a template

    if (VanguardClient::attached)
        RTCV::Vanguard::VanguardConnector::PushVanguardSpecRef(AllSpec::VanguardSpec);

    LocalNetCoreRouter::Route(Endpoints::CorruptCore,
                              Commands::Remote::PushVanguardSpec, emuSpecTemplate, true);
    LocalNetCoreRouter::Route(Endpoints::UI, Commands::Remote::PushVanguardSpec,
                              emuSpecTemplate, true);
    AllSpec::VanguardSpec->SpecUpdated += gcnew EventHandler<SpecUpdateEventArgs^>(
        &VanguardClient::SpecUpdated);
}

// Lifted from Bizhawk
static Assembly^ CurrentDomain_AssemblyResolve(Object^ sender, ResolveEventArgs^ args) {
    try {
        Trace::WriteLine("Entering AssemblyResolve\n" + args->Name + "\n" +
                         args->RequestingAssembly);
        String^ requested = args->Name;
        Monitor::Enter(AppDomain::CurrentDomain);
        {
            cli::array<Assembly^>^ asms = AppDomain::CurrentDomain->GetAssemblies();
            for (int i = 0; i < asms->Length; i++) {
                Assembly^ a = asms[i];
                if (a->FullName == requested) {
                    return a;
                }
            }

            AssemblyName^ n = gcnew AssemblyName(requested);
            // load missing assemblies by trying to find them in the dll directory
            String^ dllname = n->Name + ".dll";
            String^ directory = IO::Path::Combine(
                IO::Path::GetDirectoryName(Assembly::GetExecutingAssembly()->Location), "..",
                "RTCV");
            String^ fname = IO::Path::Combine(directory, dllname);
            if (!IO::File::Exists(fname)) {
                Trace::WriteLine(fname + " doesn't exist");
                return nullptr;
            }

            // it is important that we use LoadFile here and not load from a byte array; otherwise
            // mixed (managed/unamanged) assemblies can't load
            Trace::WriteLine("Loading " + fname);
            return Assembly::UnsafeLoadFrom(fname);
        }
    } catch (Exception^ e) {
        Trace::WriteLine("Something went really wrong in AssemblyResolve. Send this to the devs\n" +
                         e);
        return nullptr;
    }
    finally {
        Monitor::Exit(AppDomain::CurrentDomain);
    }
}

// Create our VanguardClient
void VanguardClientInitializer::Initialize() {
    // This has to be in its own method where no other dlls are used so the JIT can compile it
    AppDomain::CurrentDomain->AssemblyResolve +=
        gcnew ResolveEventHandler(CurrentDomain_AssemblyResolve);

    ConfigureVisualStyles();
    StartVanguardClient();
}

//This ensures things render as we want them.
//There are no issues running this within QT/WXWidgets applications
//This HAS to be its own method for the JIT. If it's merged with StartVanguardClient(), fun exceptions occur
void VanguardClientInitializer::ConfigureVisualStyles()
{
    // this needs to be done before the warnings/errors show up
    System::Windows::Forms::Application::EnableVisualStyles();
    System::Windows::Forms::Application::SetCompatibleTextRenderingDefault(false);
}

// Create our VanguardClient
void VanguardClientInitializer::StartVanguardClient()
{
    System::Windows::Forms::Form^ dummy = gcnew System::Windows::Forms::Form();
    IntPtr Handle = dummy->Handle;
    SyncObjectSingleton::SyncObject = dummy;
    //SyncObjectSingleton::EmuInvokeDelegate = gcnew SyncObjectSingleton::ActionDelegate(&EmuThreadExecute);
    SyncObjectSingleton::UseQueue = true;

    // Start everything
    VanguardClient::configPaths = gcnew cli::array<String^>{""
    };

    VanguardClient::StartClient();
    VanguardClient::RegisterVanguardSpec();
    RtcCore::StartEmuSide();

    // Lie if we're in attached
    if (VanguardClient::attached)
        VanguardConnector::ImplyClientConnected();

    //VanguardClient::LoadWindowPosition();
}


void VanguardClient::StartClient() {
    RTCV::Common::Logging::StartLogging(logPath);
    // Can't use contains
    auto args = Environment::GetCommandLineArgs();
    for (int i = 0; i < args->Length; i++) {
        if (args[i] == "-ATTACHED") {
            attached = true;
        }
        if (args[i] == "-DISABLERTC") {
            enableRTC = false;
        }
    }

    receiver = gcnew NetCoreReceiver();
    receiver->Attached = attached;
    receiver->MessageReceived += gcnew EventHandler<NetCoreEventArgs^>(
        &VanguardClient::OnMessageReceived);
    connector = gcnew VanguardConnector(receiver);
}


void VanguardClient::RestartClient() {
    VanguardClient::StopClient();
    VanguardClient::StartClient();
}

void VanguardClient::StopClient() {
    connector->Kill();
    connector = nullptr;
}

void VanguardClient::LoadWindowPosition() {
    if (connector == nullptr)
        return;
}

void VanguardClient::SaveWindowPosition() {
}

String^ VanguardClient::GetSyncSettings() {
    auto wrapper = VanguardSettingsWrapper::GetVanguardSettingsFromCitra();
    auto ws = GetConfigAsJson(wrapper);
    return ws;
}

void VanguardClient::SetSyncSettings(String^ ss) {
    VanguardSettingsWrapper^ wrapper = nullptr;
    //Hack for now to maintain compatibility.
    if (ss == "N3DS") {
        wrapper = gcnew VanguardSettingsWrapper();
        wrapper->is_new_3ds = true;
    }
    else {
     wrapper = GetConfigFromJson(ss);
    }
    VanguardSettingsWrapper::SetSettingsFromWrapper(wrapper);
}

#pragma region MemoryDomains

//For some reason if we do these in another class, melon won't build
public
    ref class SystemBus : RTCV::CorruptCore::IMemoryDomain {
public:
    property System::String^ Name { virtual System::String^ get(); }
    property long long Size { virtual long long get(); }
    property int WordSize { virtual int get(); }
    property bool BigEndian { virtual bool get(); }
    virtual unsigned char PeekByte(long long addr);
    virtual cli::array<unsigned char>^ PeekBytes(long long address, int length);
    virtual void PokeByte(long long addr, unsigned char val);
	virtual String^ ToString() override
	{
		return "Raw System Bus";
	}
};
public ref class Scratchpad : RTCV::CorruptCore::IMemoryDomain {
public:
	property System::String^ Name { virtual System::String^ get(); }
	property long long Size { virtual long long get(); }
	property int WordSize { virtual int get(); }
	property bool BigEndian { virtual bool get(); }
	virtual unsigned char PeekByte(long long addr);
	virtual cli::array<unsigned char>^ PeekBytes(long long address, int length);
	virtual void PokeByte(long long addr, unsigned char val);
};
public ref class VRAM : RTCV::CorruptCore::IMemoryDomain {
public:
	property System::String^ Name { virtual System::String^ get(); }
	property long long Size { virtual long long get(); }
	property int WordSize { virtual int get(); }
	property bool BigEndian { virtual bool get(); }
	virtual unsigned char PeekByte(long long addr);
	virtual cli::array<unsigned char>^ PeekBytes(long long address, int length);
	virtual void PokeByte(long long addr, unsigned char val);
};
public ref class PhysicalRAM : RTCV::CorruptCore::IMemoryDomain {
public:
	property System::String^ Name { virtual System::String^ get(); }
	property long long Size { virtual long long get(); }
	property int WordSize { virtual int get(); }
	property bool BigEndian { virtual bool get(); }
	virtual unsigned char PeekByte(long long addr);
	virtual cli::array<unsigned char>^ PeekBytes(long long address, int length);
	virtual void PokeByte(long long addr, unsigned char val);
	virtual String^ ToString() override
	{
		return "Physical RAM";
	}
};
public ref class KernelMemory : RTCV::CorruptCore::IMemoryDomain {
public:
	property System::String^ Name { virtual System::String^ get(); }
	property long long Size { virtual long long get(); }
	property int WordSize { virtual int get(); }
	property bool BigEndian { virtual bool get(); }
	virtual unsigned char PeekByte(long long addr);
	virtual cli::array<unsigned char>^ PeekBytes(long long address, int length);
	virtual void PokeByte(long long addr, unsigned char val);
	virtual String^ ToString() override
	{
		return "Kernel Memory";
	}
};
public ref class UserMemory : RTCV::CorruptCore::IMemoryDomain {
public:
	property System::String^ Name { virtual System::String^ get(); }
	property long long Size { virtual long long get(); }
	property int WordSize { virtual int get(); }
	property bool BigEndian { virtual bool get(); }
	virtual unsigned char PeekByte(long long addr);
	virtual cli::array<unsigned char>^ PeekBytes(long long address, int length);
	virtual void PokeByte(long long addr, unsigned char val);
	virtual String^ ToString() override
	{
		return "User Memory";
	}
};

//public
//    ref class VRAM : RTCV::CorruptCore::IMemoryDomain {
//public:
//    property System::String^ Name { virtual System::String^ get(); }
//    property long long Size { virtual long long get(); }
//    property int WordSize { virtual int get(); }
//    property bool BigEndian { virtual bool get(); }
//    virtual unsigned char PeekByte(long long addr);
//    virtual cli::array<unsigned char>^ PeekBytes(long long address, int length);
//    virtual void PokeByte(long long addr, unsigned char val);
//};
//
//public
//    ref class DSP : RTCV::CorruptCore::IMemoryDomain {
//public:
//    property System::String^ Name { virtual System::String^ get(); }
//    property long long Size { virtual long long get(); }
//    property int WordSize { virtual int get(); }
//    property bool BigEndian { virtual bool get(); }
//    virtual unsigned char PeekByte(long long addr);
//    virtual cli::array<unsigned char>^ PeekBytes(long long address, int length);
//    virtual void PokeByte(long long addr, unsigned char val);
//};
//public
//    ref class N3DSExRam : RTCV::CorruptCore::IMemoryDomain {
//public:
//    property System::String^ Name { virtual System::String^ get(); }
//    property long long Size { virtual long long get(); }
//    property int WordSize { virtual int get(); }
//    property bool BigEndian { virtual bool get(); }
//    virtual unsigned char PeekByte(long long addr);
//    virtual cli::array<unsigned char>^ PeekBytes(long long address, int length);
//    virtual void PokeByte(long long addr, unsigned char val);
//};


#define WORD_SIZE 4
#define BIG_ENDIAN false
#define SCRATCHPAD_OFFSET 0x00010000;
#define SCRATCHPAD_SIZE 16384;
#define VRAM_OFFSET 0x04000000;
#define VRAM_SIZE 0x00200000;
#define COMBINED_VRAM_SIZE 0x00800000; //sometimes the memory region is in a different spot so I'll just include all the spots it could be in
#define PHYSICAL_RAM_OFFSET 0x08000000;
#define USER_RAM_OFFSET 0x08800000;
#define KERNEL_RAM_OFFSET 0x88000000;
#define PSP2000_RAM_SIZE 0x04000000;
#define COMBINED_PSP2000_RAM_SIZE 0x0FE00000;

delegate void MessageDelegate(Object ^);
#pragma region SystemBus
String ^ SystemBus::Name::get() {
    return "SystemBus";
}

long long SystemBus::Size::get() {
    return 0x8FE00000;
}

int SystemBus::WordSize::get() {
    return WORD_SIZE;
}

bool SystemBus::BigEndian::get() {
    return BIG_ENDIAN;
}

unsigned char SystemBus::PeekByte(long long addr) {
    //return ReadMem8(static_cast<u32>(addr));
    if (addr < SystemBus::Size)
    {
        return ManagedWrapper::peekbyte(addr);
    }
    else return 0;
}

void SystemBus::PokeByte(long long addr, unsigned char val) {
    //WriteMem8(static_cast<u32>(addr), val);
    if (addr < SystemBus::Size)
    {
        ManagedWrapper::pokebyte(addr, val);
    }
    else return;
}

cli::array<unsigned char> ^ SystemBus::PeekBytes(long long address, int length) {
    cli::array<unsigned char> ^ bytes = gcnew cli::array<unsigned char>(length);
    for (int i = 0; i < length; i++) {
        bytes[i] = PeekByte(address + i);
    }
    return bytes;
}
#pragma endregion

#pragma region Scratchpad
String^ Scratchpad::Name::get() {
	return "Scratchpad";
}

long long Scratchpad::Size::get() {
	return SCRATCHPAD_SIZE;
}

int Scratchpad::WordSize::get() {
	return WORD_SIZE;
}

bool Scratchpad::BigEndian::get() {
	return BIG_ENDIAN;
}

unsigned char Scratchpad::PeekByte(long long addr) {
	//return ReadMem8(static_cast<u32>(addr));
	if (addr < Scratchpad::Size)
	{
		uint32_t address = addr + SCRATCHPAD_OFFSET;
		return ManagedWrapper::peekbyte(address);
	}
	else return 0;
}

void Scratchpad::PokeByte(long long addr, unsigned char val) {
	//WriteMem8(static_cast<u32>(addr), val);
	if (addr < Scratchpad::Size)
	{
		uint32_t address = addr + SCRATCHPAD_OFFSET;
		ManagedWrapper::pokebyte(address, val);
	}
	else return;
}

cli::array<unsigned char>^ Scratchpad::PeekBytes(long long address, int length) {
	cli::array<unsigned char>^ bytes = gcnew cli::array<unsigned char>(length);
	for (int i = 0; i < length; i++) {
		bytes[i] = PeekByte(address + i);
	}
	return bytes;
}
#pragma endregion

#pragma region VRAM
String^ VRAM::Name::get() {
	return "VRAM";
}

long long VRAM::Size::get() {
	return VRAM_SIZE; //doubt any psp game will use any other spots so I'll just use the default spot
}

int VRAM::WordSize::get() {
	return WORD_SIZE;
}

bool VRAM::BigEndian::get() {
	return BIG_ENDIAN;
}

unsigned char VRAM::PeekByte(long long addr) {
	//return ReadMem8(static_cast<u32>(addr));
	if (addr < VRAM::Size)
	{
		uint32_t address = addr + VRAM_OFFSET;
		return ManagedWrapper::peekbyte(address);
	}
	else return 0;
}

void VRAM::PokeByte(long long addr, unsigned char val) {
	//WriteMem8(static_cast<u32>(addr), val);
	if (addr < VRAM::Size)
	{
		uint32_t address = addr + VRAM_OFFSET;
		ManagedWrapper::pokebyte(address, val);
	}
	else return;
}

cli::array<unsigned char>^ VRAM::PeekBytes(long long address, int length) {
	cli::array<unsigned char>^ bytes = gcnew cli::array<unsigned char>(length);
	for (int i = 0; i < length; i++) {
		bytes[i] = PeekByte(address + i);
	}
	return bytes;
}
#pragma endregion

#pragma region PhysicalRAM
String^ PhysicalRAM::Name::get() {
	return "PhysicalRAM";
}

long long PhysicalRAM::Size::get() {
	return 0x04000000;
}

int PhysicalRAM::WordSize::get() {
	return WORD_SIZE;
}

bool PhysicalRAM::BigEndian::get() {
	return BIG_ENDIAN;
}

unsigned char PhysicalRAM::PeekByte(long long addr) {
	//return ReadMem8(static_cast<u32>(addr));
	if (addr < PhysicalRAM::Size)
	{
		uint32_t address = addr + PHYSICAL_RAM_OFFSET;
		return ManagedWrapper::peekbyte(address);
	}
	else return 0;
}

void PhysicalRAM::PokeByte(long long addr, unsigned char val) {
	//WriteMem8(static_cast<u32>(addr), val);
	if (addr < PhysicalRAM::Size)
	{
		uint32_t address = addr + PHYSICAL_RAM_OFFSET;
		ManagedWrapper::pokebyte(address, val);
	}
	else return;
}

cli::array<unsigned char>^ PhysicalRAM::PeekBytes(long long address, int length) {
	cli::array<unsigned char>^ bytes = gcnew cli::array<unsigned char>(length);
	for (int i = 0; i < length; i++) {
		bytes[i] = PeekByte(address + i);
	}
	return bytes;
}
#pragma endregion

#pragma region KernelMemory
String^ KernelMemory::Name::get() {
	return "KernelMemory";
}

long long KernelMemory::Size::get() {
	return 0x800000;
}

int KernelMemory::WordSize::get() {
	return WORD_SIZE;
}

bool KernelMemory::BigEndian::get() {
	return BIG_ENDIAN;
}

unsigned char KernelMemory::PeekByte(long long addr) {
	//return ReadMem8(static_cast<u32>(addr));
	if (addr < KernelMemory::Size)
	{
		uint32_t address = addr + KERNEL_RAM_OFFSET;
		return ManagedWrapper::peekbyte(address);
	}
	else return 0;
}

void KernelMemory::PokeByte(long long addr, unsigned char val) {
	//WriteMem8(static_cast<u32>(addr), val);
	if (addr < KernelMemory::Size)
	{
		uint32_t address = addr + KERNEL_RAM_OFFSET;
		ManagedWrapper::pokebyte(address, val);
	}
	else return;
}

cli::array<unsigned char>^ KernelMemory::PeekBytes(long long address, int length) {
	cli::array<unsigned char>^ bytes = gcnew cli::array<unsigned char>(length);
	for (int i = 0; i < length; i++) {
		bytes[i] = PeekByte(address + i);
	}
	return bytes;
}
#pragma endregion

#pragma region UserMemory
String^ UserMemory::Name::get() {
	return "User Memory";
}

long long UserMemory::Size::get() {
	return 0x1800000;
}

int UserMemory::WordSize::get() {
	return WORD_SIZE;
}

bool UserMemory::BigEndian::get() {
	return BIG_ENDIAN;
}

unsigned char UserMemory::PeekByte(long long addr) {
	//return ReadMem8(static_cast<u32>(addr));
	if (addr < UserMemory::Size)
	{
		uint32_t address = addr + USER_RAM_OFFSET;
		return ManagedWrapper::peekbyte(address);
	}
	else return 0;
}

void UserMemory::PokeByte(long long addr, unsigned char val) {
	//WriteMem8(static_cast<u32>(addr), val);
	if (addr < UserMemory::Size)
	{
		uint32_t address = addr + USER_RAM_OFFSET;
		ManagedWrapper::pokebyte(address, val);
	}
	else return;
}

cli::array<unsigned char>^ UserMemory::PeekBytes(long long address, int length) {
	cli::array<unsigned char>^ bytes = gcnew cli::array<unsigned char>(length);
	for (int i = 0; i < length; i++) {
		bytes[i] = PeekByte(address + i);
	}
	return bytes;
}
#pragma endregion

//#pragma region VRAM
//String ^ VRAM::Name::get() {
//    return "VRAM";
//}
//
//long long VRAM::Size::get() {
//    return Memory::VRAM_SIZE;
//}
//
//int VRAM::WordSize::get() {
//    return WORD_SIZE;
//}
//
//bool VRAM::BigEndian::get() {
//    return BIG_ENDIAN;
//}
//
//unsigned char VRAM::PeekByte(long long addr) {
//    return UnmanagedWrapper::PADDR_PEEKBYTE(addr, Memory::VRAM_PADDR);
//}
//
//void VRAM::PokeByte(long long addr, unsigned char val) {
//    UnmanagedWrapper::PADDR_POKEBYTE(addr, val, Memory::VRAM_PADDR);
//}
//
//cli::array<unsigned char> ^ VRAM::PeekBytes(long long address, int length) {
//    cli::array<unsigned char> ^ bytes = gcnew cli::array<unsigned char>(length);
//    for (int i = 0; i < length; i++) {
//        bytes[i] = PeekByte(address + i);
//    }
//    return bytes;
//}
//#pragma endregion
//#pragma region DSP
//String ^ DSP::Name::get() {
//    return "DSP";
//}
//
//long long DSP::Size::get() {
//    return Memory::DSP_RAM_SIZE;
//}
//
//int DSP::WordSize::get() {
//    return WORD_SIZE;
//}
//
//bool DSP::BigEndian::get() {
//    return BIG_ENDIAN;
//}
//
//unsigned char DSP::PeekByte(long long addr) {
//    return UnmanagedWrapper::PADDR_PEEKBYTE(addr, Memory::DSP_RAM_PADDR);
//}
//
//void DSP::PokeByte(long long addr, unsigned char val) {
//    UnmanagedWrapper::PADDR_POKEBYTE(addr, val, Memory::DSP_RAM_PADDR);
//}
//
//cli::array<unsigned char> ^ DSP::PeekBytes(long long address, int length) {
//    cli::array<unsigned char> ^ bytes = gcnew cli::array<unsigned char>(length);
//    for (int i = 0; i < length; i++) {
//        bytes[i] = PeekByte(address + i);
//    }
//    return bytes;
//}
//#pragma endregion
//#pragma region N3DSExRam
//String ^ N3DSExRam::Name::get() {
//    return "N3DSExRam";
//}
//
//long long N3DSExRam::Size::get() {
//    return Memory::N3DS_EXTRA_RAM_SIZE;
//}
//
//int N3DSExRam::WordSize::get() {
//    return WORD_SIZE;
//}
//
//bool N3DSExRam::BigEndian::get() {
//    return BIG_ENDIAN;
//}
//
//unsigned char N3DSExRam::PeekByte(long long addr) {
//    return UnmanagedWrapper::PADDR_PEEKBYTE(addr, Memory::N3DS_EXTRA_RAM_PADDR);
//}
//
//void N3DSExRam::PokeByte(long long addr, unsigned char val) {
//    UnmanagedWrapper::PADDR_POKEBYTE(addr, val, Memory::N3DS_EXTRA_RAM_PADDR);
//}
//
//cli::array<unsigned char> ^ N3DSExRam::PeekBytes(long long address, int length) {
//    cli::array<unsigned char> ^ bytes = gcnew cli::array<unsigned char>(length);
//    for (int i = 0; i < length; i++) {
//        bytes[i] = PeekByte(address + i);
//    }
//    return bytes;
//}
//#pragma endregion


static cli::array<MemoryDomainProxy^>^ GetInterfaces() {

    if (String::IsNullOrWhiteSpace(AllSpec::VanguardSpec->Get<String^>(VSPEC::OPENROMFILENAME)))
        return gcnew cli::array<MemoryDomainProxy^>(0);

    cli::array<MemoryDomainProxy ^> ^ interfaces = gcnew cli::array<MemoryDomainProxy ^>(6);
	interfaces[0] = (gcnew MemoryDomainProxy(gcnew SystemBus));
	interfaces[1] = (gcnew MemoryDomainProxy(gcnew Scratchpad));
	interfaces[2] = (gcnew MemoryDomainProxy(gcnew VRAM));
	interfaces[3] = (gcnew MemoryDomainProxy(gcnew PhysicalRAM));
	interfaces[4] = (gcnew MemoryDomainProxy(gcnew UserMemory));
	interfaces[5] = (gcnew MemoryDomainProxy(gcnew KernelMemory));
    return interfaces;
}

static bool RefreshDomains(bool updateSpecs = true) {
    cli::array<MemoryDomainProxy^>^ oldInterfaces =
        AllSpec::VanguardSpec->Get<cli::array<MemoryDomainProxy^>^>(VSPEC::MEMORYDOMAINS_INTERFACES);
    cli::array<MemoryDomainProxy^>^ newInterfaces = GetInterfaces();

    // Bruteforce it since domains can c`   hange inconsistently in some configs and we keep code
    // consistent between implementations
    bool domainsChanged = false;
    if (oldInterfaces == nullptr)
        domainsChanged = true;
    else {
        domainsChanged = oldInterfaces->Length != newInterfaces->Length;
        for (int i = 0; i < oldInterfaces->Length; i++) {
            if (domainsChanged)
                break;
            if (oldInterfaces[i]->Name != newInterfaces[i]->Name)
                domainsChanged = true;
            if (oldInterfaces[i]->Size != newInterfaces[i]->Size)
                domainsChanged = true;
        }
    }

    if (updateSpecs) {
        AllSpec::VanguardSpec->Update(VSPEC::MEMORYDOMAINS_INTERFACES, newInterfaces, true, true);
        LocalNetCoreRouter::Route(Endpoints::CorruptCore,
                                  Commands::Remote::EventDomainsUpdated, domainsChanged,
                                  true);
    }

    return domainsChanged;
}

#pragma endregion

String ^ VanguardClient::GetConfigAsJson(VanguardSettingsWrapper ^ settings) {
    return JsonHelper::Serialize(settings);
}

VanguardSettingsWrapper ^ VanguardClient::GetConfigFromJson(String ^ str) {
    return JsonHelper::Deserialize<VanguardSettingsWrapper ^>(str);
}

static void STEP_CORRUPT() // errors trapped by CPU_STEP
{
    if (!VanguardClient::enableRTC)
        return;
    RtcClock::StepCorrupt(true, false);
}


#pragma region Hooks
void VanguardClientUnmanaged::CORE_STEP() {
    /*if (!VanguardClient::enableRTC)
        return;*/
    // Any step hook for corruption
    ActionDistributor::Execute("ACTION");
    STEP_CORRUPT();
}

// This is on the main thread not the emu thread
void VanguardClientUnmanaged::LOAD_GAME_START(std::string romPath) {
	RefreshDomains();
    if (!VanguardClient::enableRTC)
        return;
    LocalNetCoreRouter::Route(Endpoints::CorruptCore, Commands::Remote::ClearStepBlastUnits, false);
    RtcClock::ResetCount();

    String^ gameName = Helpers::utf8StringToSystemString(romPath);
    AllSpec::VanguardSpec->Update(VSPEC::OPENROMFILENAME, gameName, true, true);
}


void VanguardClientUnmanaged::LOAD_GAME_DONE() {
    if (!VanguardClient::enableRTC)
        return;
    PartialSpec^ gameDone = gcnew PartialSpec("VanguardSpec");

    try {
        gameDone->Set(VSPEC::SYSTEM, "PSP");
        gameDone->Set(VSPEC::SYSTEMPREFIX, "PSP");
        gameDone->Set(VSPEC::SYSTEMCORE, "PPSSPP");
        gameDone->Set(VSPEC::CORE_DISKBASED, true);

        String^ oldGame = AllSpec::VanguardSpec->Get<String^>(VSPEC::GAMENAME);

        String^ gameName = Helpers::utf8StringToSystemString(UnmanagedWrapper::VANGUARD_GETGAMENAME());

        char replaceChar = L'-';
        gameDone->Set(VSPEC::GAMENAME,
                      StringExtensions::MakeSafeFilename(gameName, replaceChar));

        String ^ syncsettings = VanguardClient::GetConfigAsJson(VanguardSettingsWrapper::GetVanguardSettingsFromCitra());
        gameDone->Set(VSPEC::SYNCSETTINGS, syncsettings);

        AllSpec::VanguardSpec->Update(gameDone, true, false);

        bool domainsChanged = RefreshDomains(true);

        if (oldGame != gameName) {
            LocalNetCoreRouter::Route(Endpoints::UI,
                                      Commands::Basic::ResetGameProtectionIfRunning, true);
        }
    } catch (Exception^ e) {
        Trace::WriteLine(e->ToString());
    }

    /*
    VanguardClient::event = Core::System::GetInstance().CoreTiming().RegisterEvent(
        "RTCV::run_event",
        [](u64 thread_id,
               s64 cycle_late)
        {
            RunCallback(
                thread_id, cycle_late);
        });

     Core::System::GetInstance().CoreTiming().ScheduleEvent(run_interval_ticks, VanguardClient::event);
     */
    VanguardClient::loading = false;

}


void VanguardClientUnmanaged::LOAD_STATE_DONE() {
    if (!VanguardClient::enableRTC)
        return;
    VanguardClient::stateLoading = false;
}

void VanguardClientUnmanaged::GAME_CLOSED() {
    if (!VanguardClient::enableRTC)
        return;
    AllSpec::VanguardSpec->Update(VSPEC::OPENROMFILENAME, "", true, true);
    RefreshDomains();
    RtcCore::InvokeGameClosed(true);
}


bool VanguardClientUnmanaged::RTC_OSD_ENABLED() {
    if (!VanguardClient::enableRTC)
        return true;
    if (RTCV::NetCore::Params::IsParamSet(RTCSPEC::CORE_EMULATOROSDDISABLED))
        return false;
    return true;
}

#pragma endregion

/*ENUMS FOR THE SWITCH STATEMENT*/
enum COMMANDS {
    SAVESAVESTATE,
    LOADSAVESTATE,
    REMOTE_LOADROM,
    REMOTE_CLOSEGAME,
    REMOTE_DOMAIN_GETDOMAINS,
    REMOTE_KEY_SETSYNCSETTINGS,
    REMOTE_KEY_SETSYSTEMCORE,
    REMOTE_EVENT_EMU_MAINFORM_CLOSE,
    REMOTE_EVENT_EMUSTARTED,
    REMOTE_ISNORMALADVANCE,
    REMOTE_EVENT_CLOSEEMULATOR,
    REMOTE_ALLSPECSSENT,
    REMOTE_POSTCORRUPTACTION,
    REMOTE_RESUMEEMULATION,
    UNKNOWN
};

inline COMMANDS CheckCommand(String^ inString) {
    if (inString == RTCV::NetCore::Commands::Basic::LoadSavestate)
        return LOADSAVESTATE;
    else if (inString == RTCV::NetCore::Commands::Basic::SaveSavestate)
        return SAVESAVESTATE;
    else if (inString == RTCV::NetCore::Commands::Remote::LoadROM)
        return REMOTE_LOADROM;
    else if (inString == RTCV::NetCore::Commands::Remote::CloseGame)
        return REMOTE_CLOSEGAME;
    else if (inString == RTCV::NetCore::Commands::Remote::AllSpecSent)
        return REMOTE_ALLSPECSSENT;
    else if (inString == RTCV::NetCore::Commands::Remote::DomainGetDomains)
        return REMOTE_DOMAIN_GETDOMAINS;
    else if (inString == RTCV::NetCore::Commands::Remote::KeySetSystemCore)
        return REMOTE_KEY_SETSYSTEMCORE;
    else if (inString == RTCV::NetCore::Commands::Remote::KeySetSyncSettings)
        return REMOTE_KEY_SETSYNCSETTINGS;
    else if (inString == RTCV::NetCore::Commands::Remote::EventEmuMainFormClose)
        return REMOTE_EVENT_EMU_MAINFORM_CLOSE;
    else if (inString == RTCV::NetCore::Commands::Remote::EventEmuStarted)
        return REMOTE_EVENT_EMUSTARTED;
    else if (inString == RTCV::NetCore::Commands::Remote::IsNormalAdvance)
        return REMOTE_ISNORMALADVANCE;
    else if (inString == RTCV::NetCore::Commands::Remote::EventCloseEmulator)
        return REMOTE_EVENT_CLOSEEMULATOR;
    else if (inString == RTCV::NetCore::Commands::Remote::EventCloseEmulator)
        return REMOTE_POSTCORRUPTACTION;
    else if (inString == RTCV::NetCore::Commands::Remote::ResumeEmulation)
        return REMOTE_RESUMEEMULATION;
    return UNKNOWN;
}

/* IMPLEMENT YOUR COMMANDS HERE */
void VanguardClient::LoadRom(String^ filename) {
    String ^ currentOpenRom = "";
    if (AllSpec::VanguardSpec->Get<String ^>(VSPEC::OPENROMFILENAME) != "")
        currentOpenRom = AllSpec::VanguardSpec->Get<String ^>(VSPEC::OPENROMFILENAME);

    // Game is not running
    if (currentOpenRom != filename) {
        std::string path = Helpers::systemStringToUtf8String(filename);
        loading = true;
        UnmanagedWrapper::VANGUARD_LOADGAME(path);
        // We have to do it this way to prevent deadlock due to synced calls. It sucks but it's
        // required at the moment
        while (loading) {
            Thread::Sleep(20);
            System::Windows::Forms::Application::DoEvents();
        }

        Thread::Sleep(10); // Give the emu thread a chance to recover
    }
}

bool VanguardClient::LoadState(std::string filename) {
    LocalNetCoreRouter::Route(Endpoints::CorruptCore, Commands::Remote::ClearStepBlastUnits, false);
    RtcClock::ResetCount();
    stateLoading = true;
    ManagedWrapper::loadsavestate(filename);
    // We have to do it this way to prevent deadlock due to synced calls. It sucks but it's required
    // at the moment
    //int i = 0;
    //do {
    //    Thread::Sleep(20);
    //    System::Windows::Forms::Application::DoEvents();

    //    // We wait for 20 ms every time. If loading a game takes longer than 10 seconds, break out.
    //    if (++i > 500) {
    //        stateLoading = false;
    //        return false;
    //    }
    //} while (stateLoading);
    RefreshDomains();
    return true;
}

bool VanguardClient::SaveState(String ^ filename, bool wait) {/*
    std::string s = Helpers::systemStringToUtf8String(filename);
    const char* converted_filename = s.c_str();
    VanguardClient::lastStateName = filename;*/
    ManagedWrapper::savesavestate(Helpers::systemStringToUtf8String(filename));
    VanguardClient::fileToCopy = filename;
    return true;
}

void VanguardClientUnmanaged::SAVE_STATE_DONE() {
    /*if (!VanguardClient::enableRTC || VanguardClient::fileToCopy == nullptr ||
        VanguardClient::fileToCopy == "")
        return;*/
    //Thread::Sleep(2000);
    System::IO::File::Copy(Helpers::utf8StringToSystemString(ManagedWrapper::getstatepath()), VanguardClient::lastStateName, true);
}

// No fun anonymous classes with closure here
#pragma region Delegates
void StopGame() {
    UnmanagedWrapper::VANGUARD_STOPGAME();
}

void Quit() {
    System::Environment::Exit(0);
}

void AllSpecsSent() {
    VanguardClient::LoadWindowPosition();
    RefreshDomains();
}
#pragma endregion

/* THIS IS WHERE YOU HANDLE ANY RECEIVED MESSAGES */
void VanguardClient::OnMessageReceived(Object^ sender, NetCoreEventArgs^ e) {
    NetCoreMessage^ message = e->message;
    NetCoreAdvancedMessage^ advancedMessage;

    if (Helpers::is<NetCoreAdvancedMessage^>(message))
        advancedMessage = static_cast<NetCoreAdvancedMessage^>(message);

    switch (CheckCommand(message->Type)) {
    case REMOTE_ALLSPECSSENT: {
        auto g = gcnew SyncObjectSingleton::GenericDelegate(&AllSpecsSent);
        SyncObjectSingleton::FormExecute(g);
        VanguardInitializationComplete = true;
    }
    break;

    case LOADSAVESTATE: {
        cli::array<Object^>^ cmd = static_cast<cli::array<Object^>^>(advancedMessage->objectValue);
        String^ path = static_cast<String^>(cmd[0]);
        std::string converted_path = Helpers::systemStringToUtf8String(path);

        // Load up the sync settings
        String^ settingStr = AllSpec::VanguardSpec->Get<String^>(VSPEC::SYNCSETTINGS);
        if (!String::IsNullOrEmpty(settingStr)) {
            VanguardClient::SetSyncSettings(settingStr);
        }
        bool success = LoadState(converted_path);
        // bool success = true;
        e->setReturnValue(success);
    }
    break;

    case SAVESAVESTATE: {
        String^ Key = (String^)(advancedMessage->objectValue);

        //Save the syncsettings
        AllSpec::VanguardSpec->Set(VSPEC::SYNCSETTINGS, VanguardClient::GetSyncSettings());

        // Build the shortname
        String^ quickSlotName = Key + ".timejump";
        // Get the prefix for the state

        String ^ gameName = Helpers::utf8StringToSystemString(UnmanagedWrapper::VANGUARD_GETGAMENAME());

        char replaceChar = L'-';
        String^ prefix = StringExtensions::MakeSafeFilename(gameName, replaceChar);
        prefix = prefix->Substring(prefix->LastIndexOf('\\') + 1);

        String^ path = nullptr;
        // Build up our path
        path = RtcCore::workingDir + IO::Path::DirectorySeparatorChar + "SESSION" + IO::Path::
               DirectorySeparatorChar + prefix + "." + quickSlotName + ".State";

        // If the path doesn't exist, make it
        IO::FileInfo^ file = gcnew IO::FileInfo(path);
        if (file->Directory != nullptr && file->Directory->Exists == false)
            file->Directory->Create();
        VanguardClient::SaveState(path, true);
        e->setReturnValue(path);
    }
    break;

    case REMOTE_LOADROM: {
        String^ filename = (String^)advancedMessage->objectValue;
        //Citra DEMANDS the rom is loaded from the main thread
        System::Action<String^>^a = gcnew Action<String^>(&LoadRom);
        SyncObjectSingleton::FormExecute<String ^>(a, filename);
    }
    break;

    case REMOTE_CLOSEGAME: {
        SyncObjectSingleton::GenericDelegate ^ g = gcnew SyncObjectSingleton::GenericDelegate(&StopGame);
        SyncObjectSingleton::FormExecute(g);
    }
    break;

    case REMOTE_DOMAIN_GETDOMAINS: {
        RefreshDomains();
    }
    break;

    case REMOTE_KEY_SETSYNCSETTINGS: {
        String^ settings = (String^)(advancedMessage->objectValue);
        AllSpec::VanguardSpec->Set(VSPEC::SYNCSETTINGS, settings);
    }
    break;

    case REMOTE_KEY_SETSYSTEMCORE: {
        // Do nothing
    }
    break;

    case REMOTE_EVENT_EMUSTARTED: {
        // Do nothing
    }
    break;

    case REMOTE_ISNORMALADVANCE: {
        // Todo - Dig out fast forward?
        e->setReturnValue(true);
    }
    break;
    case REMOTE_POSTCORRUPTACTION: {
    }
    break;

    case REMOTE_RESUMEEMULATION: {
        //UnmanagedWrapper::VANGUARD_RESUMEEMULATION();
    }
    break;

    case REMOTE_EVENT_EMU_MAINFORM_CLOSE:
    case REMOTE_EVENT_CLOSEEMULATOR: {
        //Don't allow re-entry on this
        Monitor::Enter(VanguardClient::GenericLockObject);
        {
            VanguardClient::SaveWindowPosition();
            Quit();
        }
        Monitor::Exit(VanguardClient::GenericLockObject);
    }
        break;

    default:
        break;
    }
}

VanguardSettingsWrapper ^ VanguardSettingsWrapper::GetVanguardSettingsFromCitra() {
    /*VanguardSettingsWrapper ^ vSettings = gcnew VanguardSettingsWrapper();

    UnmanagedWrapper::GetSettingsFromCitra();
    vSettings->is_new_3ds = UnmanagedWrapper::nSettings.is_new_3ds;
    vSettings->region_value = UnmanagedWrapper::nSettings.region_value;
    vSettings->init_clock = UnmanagedWrapper::nSettings.init_clock;
    vSettings->init_time = UnmanagedWrapper::nSettings.init_time;
    vSettings->shaders_accurate_mul = UnmanagedWrapper::nSettings.shaders_accurate_mul;
    vSettings->upright_screen = UnmanagedWrapper::nSettings.upright_screen;
    vSettings->enable_dsp_lle = UnmanagedWrapper::nSettings.enable_dsp_lle;
    vSettings->enable_dsp_lle_multithread = UnmanagedWrapper::nSettings.enable_dsp_lle_multithread;*/

    // settings->birthmonth = Service::PTM::Module::GetPlayCoins();
    // settings->birthday = Service::PTM::Module::GetPlayCoins();
    // settings->language_index = GetSystemLanguage()
    // settings->country = Service::PTM::Module::GetPlayCoins();
    // settings->play_coin = Service::PTM::Module::GetPlayCoins();

    //return vSettings;
    return nullptr;
}

void VanguardSettingsWrapper::SetSettingsFromWrapper(VanguardSettingsWrapper ^ vSettings) {
    /*UnmanagedWrapper::nSettings.is_new_3ds = vSettings->is_new_3ds;
    UnmanagedWrapper::nSettings.region_value = vSettings->region_value;
    UnmanagedWrapper::nSettings.init_clock = 1;
    UnmanagedWrapper::nSettings.init_time = vSettings->init_time;
    UnmanagedWrapper::nSettings.shaders_accurate_mul = vSettings->shaders_accurate_mul;
    UnmanagedWrapper::nSettings.upright_screen = vSettings->upright_screen;
    UnmanagedWrapper::nSettings.enable_dsp_lle = vSettings->enable_dsp_lle;
    UnmanagedWrapper::nSettings.enable_dsp_lle_multithread = vSettings->enable_dsp_lle_multithread;*/
    UnmanagedWrapper::SetSettingsFromUnmanagedWrapper();
}
