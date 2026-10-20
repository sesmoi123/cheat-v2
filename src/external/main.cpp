// Retrac OGFN - external. Un seul exe, aucune injection :
// ReadProcessMemory/WriteProcessMemory sur FortniteClient-Win64-Shipping.exe,
// overlay GDI layered par-dessus le jeu (ESP), souris (aimbot + bullet TP).
//
// Autonome : offsets par defaut embarques (build 42.40) + offsets.json ecrit
// a cote de l'exe pour les ajuster sans recompiler.

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

namespace cfg {

constexpr int kAimbotKey = VK_RBUTTON;   // maintenu
constexpr int kBulletTpKey = 'Y';        // maintenu
constexpr int kEspKey = VK_INSERT;       // bascule
constexpr int kQuitKey = VK_F8;

constexpr float kAimbotFov = 260.0f;     // rayon en pixels
constexpr float kAimbotSmooth = 3.0f;    // 1 = instantane
constexpr float kMaxDistance = 300.0f;   // metres
constexpr double kAimHeight = 60.0;      // cm au-dessus du root (poitrine)
constexpr double kCapsuleBottom = -88.0; // pieds
constexpr double kCapsuleTop = 92.0;     // tete
constexpr int kTargetFps = 120;

constexpr uint32_t kColorLocal = 0x00FF7A;
constexpr uint32_t kColorTeam = 0x00C8FF;
constexpr uint32_t kColorEnemy = 0xFF3B3B;
constexpr uint32_t kColorBot = 0xFFB300;
constexpr uint32_t kColorText = 0xE8E8E8;
constexpr uint32_t kColorDim = 0x9A9A9A;

} // namespace cfg

// ---------------------------------------------------------------------------
// Diag (panneau de statut)
// ---------------------------------------------------------------------------

namespace diag {

bool processOk = false;
bool worldOk = false;
int offsetsCount = 0;
std::wstring logPath;

} // namespace diag

// ---------------------------------------------------------------------------
// Process / memoire
// ---------------------------------------------------------------------------

namespace mem {

HANDLE g_process = nullptr;
uintptr_t g_base = 0;
DWORD g_pid = 0;
HWND g_window = nullptr;

template <typename T>
bool read(uintptr_t address, T& out) {
    if (!address || !g_process) {
        return false;
    }
    SIZE_T bytes = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(address), &out, sizeof(T),
                             &bytes) &&
           bytes == sizeof(T);
}

template <typename T>
T read(uintptr_t address) {
    T value{};
    read(address, value);
    return value;
}

template <typename T>
bool write(uintptr_t address, const T& value) {
    if (!address || !g_process) {
        return false;
    }
    SIZE_T bytes = 0;
    return WriteProcessMemory(g_process, reinterpret_cast<LPVOID>(address),
                              const_cast<T*>(&value), sizeof(T), &bytes);
}

bool enable_debug_privilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    const BOOL ok =
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    const DWORD error = GetLastError();
    CloseHandle(token);
    return ok && error == ERROR_SUCCESS;
}

bool is_fortnite(const std::wstring& raw) {
    std::wstring name;
    name.reserve(raw.size());
    for (const wchar_t character : raw) {
        name.push_back(static_cast<wchar_t>(::towlower(character)));
    }
    return name.rfind(L"fortniteclient", 0) == 0 ||
           name == L"fortniteclient-win64-shipping.exe";
}

DWORD find_process() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD found = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (is_fortnite(entry.szExeFile) && entry.th32ProcessID != GetCurrentProcessId()) {
                found = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

uintptr_t module_base(DWORD pid, const wchar_t* module) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    uintptr_t exact = 0;
    uintptr_t loose = 0;
    uintptr_t first = 0;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (!first) {
                first = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            }
            if (_wcsicmp(entry.szModule, module) == 0) {
                exact = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
            if (!loose && wcsstr(entry.szModule, L"FortniteClient")) {
                loose = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return exact ? exact : (loose ? loose : first);
}

HWND find_window(DWORD pid) {
    struct Search {
        DWORD pid = 0;
        HWND best = nullptr;
        LONG bestArea = 0;
    } search;
    search.pid = pid;

    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            Search* search = reinterpret_cast<Search*>(param);

            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != search->pid || !IsWindowVisible(window)) {
                return TRUE;
            }

            RECT client{};
            if (!GetClientRect(window, &client)) {
                return TRUE;
            }

            const LONG area = client.right * client.bottom;
            if (area > search->bestArea) {
                search->bestArea = area;
                search->best = window;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.best;
}

bool attach(DWORD pid) {
    static constexpr DWORD kMasks[] = {
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_LIMITED_INFORMATION,
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        PROCESS_ALL_ACCESS,
    };

    for (int attempt = 0; attempt < 120 && !g_process; ++attempt) {
        for (const DWORD mask : kMasks) {
            g_process = OpenProcess(mask, FALSE, pid);
            if (g_process) {
                break;
            }
        }
        if (!g_process) {
            Sleep(500);
        }
    }
    if (!g_process) {
        return false;
    }

    g_pid = pid;
    g_base = module_base(pid, L"FortniteClient-Win64-Shipping.exe");
    if (!g_base) {
        CloseHandle(g_process);
        g_process = nullptr;
        return false;
    }
    return true;
}

} // namespace mem

// ---------------------------------------------------------------------------
// Offsets : defauts embarques, surcharges par offsets.json si present
// ---------------------------------------------------------------------------

namespace offsets {

struct Table {
    uintptr_t GameEngine = 0;             // module : GEngine
    uintptr_t GameEngine_GameViewport = 0;
    uintptr_t GameViewport_World = 0;
    uintptr_t GWorld = 0;                 // global direct (chiffre sur 42.40)
    uintptr_t World_Levels = 0;
    uintptr_t World_OwningGameInstance = 0;
    uintptr_t Level_ActorCluster = 0;
    uintptr_t LevelActorContainer_Actors = 0;
    uintptr_t GameInstance_LocalPlayers = 0;
    uintptr_t LocalPlayer_PlayerController = 0;
    uintptr_t PlayerController_AcknowledgedPawn = 0;
    uintptr_t PlayerController_PlayerCameraManager = 0;
    uintptr_t PlayerController_ControlRotation = 0;
    uintptr_t CameraManager_CameraCache = 0;
    uintptr_t CameraCache_POV = 0;
    uintptr_t Pawn_PlayerState = 0;
    uintptr_t Pawn_Mesh = 0;
    uintptr_t Pawn_IsDBNO = 0;
    uintptr_t Actor_RootComponent = 0;
    uintptr_t SceneComponent_RelativeLocation = 0;
    uintptr_t PlayerState_PlayerName = 0;
    uintptr_t PlayerState_PlayerNameAlt = 0;
    uintptr_t PlayerState_TeamIndex = 0;
    uintptr_t PlayerState_IsABot = 0;
    uintptr_t Mesh_BoneArray = 0;
    uintptr_t Mesh_ComponentToWorld = 0;
};

Table g;

// Build 42.40-CL-59086525 (dumps du 15/10/2026). A ajuster apres un patch.
struct Default {
    const char* key;
    uintptr_t value;
};

const Default kDefaults[] = {
    {"GameEngine", 0x1BD9D378},
    {"GameEngine_GameViewport", 0xB70},
    {"GameViewport_World", 0x78},
    {"GWorld", 0x1BD9B9E8},
    {"World_Levels", 0x1F0},
    {"World_OwningGameInstance", 0x250},
    {"Level_ActorCluster", 0x98},
    {"LevelActorContainer_Actors", 0x50},
    {"GameInstance_LocalPlayers", 0x38},
    {"LocalPlayer_PlayerController", 0x30},
    {"PlayerController_AcknowledgedPawn", 0x318},
    {"PlayerController_PlayerCameraManager", 0x328},
    {"PlayerController_ControlRotation", 0x2E8},
    {"CameraManager_CameraCache", 0x1590},
    {"CameraCache_POV", 0x10},
    {"Pawn_PlayerState", 0x290},
    {"Pawn_Mesh", 0x2F0},
    {"Pawn_IsDBNO", 0x881},
    {"Actor_RootComponent", 0x1B0},
    {"SceneComponent_RelativeLocation", 0x140},
    {"PlayerState_PlayerName", 0x308},
    {"PlayerState_PlayerNameAlt", 0x340},
    {"PlayerState_TeamIndex", 0xF69},
    {"PlayerState_IsABot", 0x27A},
    {"Mesh_BoneArray", 0x660},
    {"Mesh_ComponentToWorld", 0x1E0},
};

std::string normalize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char character : text) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (isalnum(value)) {
            out.push_back(static_cast<char>(tolower(value)));
        }
    }
    return out;
}

std::map<std::string, uintptr_t> parse(const std::string& text) {
    std::map<std::string, uintptr_t> table;

    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t open = text.find('"', cursor);
        if (open == std::string::npos) {
            break;
        }
        const size_t close = text.find('"', open + 1);
        if (close == std::string::npos) {
            break;
        }

        const std::string key = text.substr(open + 1, close - open - 1);
        cursor = close + 1;

        const size_t colon = text.find(':', cursor);
        if (colon == std::string::npos) {
            break;
        }

        size_t value = colon + 1;
        while (value < text.size() && isspace(static_cast<unsigned char>(text[value]))) {
            ++value;
        }

        char* end = nullptr;
        const unsigned long long number = strtoull(text.c_str() + value, &end, 0);
        if (end && end != text.c_str() + value) {
            table[normalize(key)] = static_cast<uintptr_t>(number);
            cursor = static_cast<size_t>(end - text.c_str());
        }
    }

    return table;
}

bool read_file(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    out.assign(static_cast<size_t>(size.QuadPart), '\0');

    DWORD bytes = 0;
    const BOOL ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &bytes, nullptr);
    CloseHandle(file);

    out.resize(bytes);
    return ok && bytes != 0;
}

std::wstring exe_directory() {
    wchar_t exePath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH * 2);

    std::wstring directory = exePath;
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        directory.resize(slash);
    }
    return directory;
}

uintptr_t lookup(const std::map<std::string, uintptr_t>& table,
                 std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const auto found = table.find(normalize(name));
        if (found != table.end()) {
            return found->second;
        }
    }

    for (const char* name : names) {
        const std::string key = normalize(name);
        for (const auto& [candidate, value] : table) {
            if (candidate.size() >= key.size() &&
                candidate.compare(candidate.size() - key.size(), key.size(), key) == 0) {
                return value;
            }
        }
    }

    return 0;
}

uintptr_t pick(const std::map<std::string, uintptr_t>& table, uintptr_t fallback,
               std::initializer_list<const char*> names) {
    const uintptr_t value = lookup(table, names);
    return value ? value : fallback;
}

// Ecrit le jeu d'offsets resolu a cote de l'exe : sert de base editable.
void dump_json(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w") != 0 || !file) {
        return;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"_build\": \"42.40-CL-59086525\",\n");
    fprintf(file, "  \"_note\": \"ajuste ici si un patch casse la chaine, l'exe relit ce fichier au demarrage\",\n");
    fprintf(file, "  \"GameEngine\": \"0x%llX\",\n", (unsigned long long)g.GameEngine);
    fprintf(file, "  \"GameEngine_GameViewport\": \"0x%llX\",\n", (unsigned long long)g.GameEngine_GameViewport);
    fprintf(file, "  \"GameViewport_World\": \"0x%llX\",\n", (unsigned long long)g.GameViewport_World);
    fprintf(file, "  \"GWorld\": \"0x%llX\",\n", (unsigned long long)g.GWorld);
    fprintf(file, "  \"World_Levels\": \"0x%llX\",\n", (unsigned long long)g.World_Levels);
    fprintf(file, "  \"World_OwningGameInstance\": \"0x%llX\",\n", (unsigned long long)g.World_OwningGameInstance);
    fprintf(file, "  \"Level_ActorCluster\": \"0x%llX\",\n", (unsigned long long)g.Level_ActorCluster);
    fprintf(file, "  \"LevelActorContainer_Actors\": \"0x%llX\",\n", (unsigned long long)g.LevelActorContainer_Actors);
    fprintf(file, "  \"GameInstance_LocalPlayers\": \"0x%llX\",\n", (unsigned long long)g.GameInstance_LocalPlayers);
    fprintf(file, "  \"LocalPlayer_PlayerController\": \"0x%llX\",\n", (unsigned long long)g.LocalPlayer_PlayerController);
    fprintf(file, "  \"PlayerController_AcknowledgedPawn\": \"0x%llX\",\n", (unsigned long long)g.PlayerController_AcknowledgedPawn);
    fprintf(file, "  \"PlayerController_PlayerCameraManager\": \"0x%llX\",\n", (unsigned long long)g.PlayerController_PlayerCameraManager);
    fprintf(file, "  \"PlayerController_ControlRotation\": \"0x%llX\",\n", (unsigned long long)g.PlayerController_ControlRotation);
    fprintf(file, "  \"CameraManager_CameraCache\": \"0x%llX\",\n", (unsigned long long)g.CameraManager_CameraCache);
    fprintf(file, "  \"CameraCache_POV\": \"0x%llX\",\n", (unsigned long long)g.CameraCache_POV);
    fprintf(file, "  \"Pawn_PlayerState\": \"0x%llX\",\n", (unsigned long long)g.Pawn_PlayerState);
    fprintf(file, "  \"Pawn_Mesh\": \"0x%llX\",\n", (unsigned long long)g.Pawn_Mesh);
    fprintf(file, "  \"Pawn_IsDBNO\": \"0x%llX\",\n", (unsigned long long)g.Pawn_IsDBNO);
    fprintf(file, "  \"Actor_RootComponent\": \"0x%llX\",\n", (unsigned long long)g.Actor_RootComponent);
    fprintf(file, "  \"SceneComponent_RelativeLocation\": \"0x%llX\",\n", (unsigned long long)g.SceneComponent_RelativeLocation);
    fprintf(file, "  \"PlayerState_PlayerName\": \"0x%llX\",\n", (unsigned long long)g.PlayerState_PlayerName);
    fprintf(file, "  \"PlayerState_TeamIndex\": \"0x%llX\",\n", (unsigned long long)g.PlayerState_TeamIndex);
    fprintf(file, "  \"PlayerState_IsABot\": \"0x%llX\",\n", (unsigned long long)g.PlayerState_IsABot);
    fprintf(file, "  \"Mesh_BoneArray\": \"0x%llX\",\n", (unsigned long long)g.Mesh_BoneArray);
    fprintf(file, "  \"Mesh_ComponentToWorld\": \"0x%llX\"\n", (unsigned long long)g.Mesh_ComponentToWorld);
    fprintf(file, "}\n");

    fclose(file);
}

bool load() {
    std::map<std::string, uintptr_t> table;

    std::vector<std::wstring> candidates;
    candidates.push_back(exe_directory() + L"\\offsets.json");

    wchar_t localAppData[MAX_PATH * 2]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH * 2)) {
        candidates.push_back(std::wstring(localAppData) + L"\\RetracOGFN\\offsets.json");
    }

    for (const std::wstring& path : candidates) {
        std::string text;
        if (!read_file(path, text)) {
            continue;
        }

        const std::map<std::string, uintptr_t> parsed = parse(text);
        for (const auto& [key, value] : parsed) {
            table[key] = value;
        }
        std::wprintf(L"  [info] offsets externes : %s (%zu entrees)\n", path.c_str(), parsed.size());
    }

    // Surcharge fichier si present, sinon valeur par defaut embarquee.
    auto def = [](const char* key) -> uintptr_t {
        for (const Default& entry : kDefaults) {
            if (strcmp(entry.key, key) == 0) {
                return entry.value;
            }
        }
        return 0;
    };

    g.GameEngine = pick(table, def("GameEngine"), {"GameEngine", "gEngine"});
    g.GameEngine_GameViewport = pick(table, def("GameEngine_GameViewport"), {"GameEngine_GameViewport"});
    g.GameViewport_World = pick(table, def("GameViewport_World"), {"GameViewport_World"});
    g.GWorld = pick(table, def("GWorld"), {"GWorld", "UWorld", "World"});
    g.World_Levels = pick(table, def("World_Levels"), {"World_Levels", "Levels"});
    g.World_OwningGameInstance = pick(table, def("World_OwningGameInstance"), {"World_OwningGameInstance", "OwningGameInstance", "GameInstance"});
    g.Level_ActorCluster = pick(table, def("Level_ActorCluster"), {"Level_ActorCluster", "ActorCluster"});
    g.LevelActorContainer_Actors = pick(table, def("LevelActorContainer_Actors"), {"LevelActorContainer_Actors"});
    g.GameInstance_LocalPlayers = pick(table, def("GameInstance_LocalPlayers"), {"GameInstance_LocalPlayers", "LocalPlayers"});
    g.LocalPlayer_PlayerController = pick(table, def("LocalPlayer_PlayerController"), {"LocalPlayer_PlayerController", "PlayerController"});
    g.PlayerController_AcknowledgedPawn = pick(table, def("PlayerController_AcknowledgedPawn"), {"PlayerController_AcknowledgedPawn", "AcknowledgedPawn", "LocalPawn"});
    g.PlayerController_PlayerCameraManager = pick(table, def("PlayerController_PlayerCameraManager"), {"PlayerController_PlayerCameraManager", "PlayerCameraManager", "CameraManager"});
    g.PlayerController_ControlRotation = pick(table, def("PlayerController_ControlRotation"), {"PlayerController_ControlRotation", "ControlRotation"});
    g.CameraManager_CameraCache = pick(table, def("CameraManager_CameraCache"), {"CameraManager_CameraCache", "CameraCache", "CameraCachePrivate"});
    g.CameraCache_POV = pick(table, def("CameraCache_POV"), {"CameraCache_POV", "POV"});
    g.Pawn_PlayerState = pick(table, def("Pawn_PlayerState"), {"Pawn_PlayerState", "PlayerState"});
    g.Pawn_Mesh = pick(table, def("Pawn_Mesh"), {"Pawn_Mesh", "Mesh"});
    g.Pawn_IsDBNO = pick(table, def("Pawn_IsDBNO"), {"Pawn_IsDBNO", "bIsDBNO", "IsDBNO"});
    g.Actor_RootComponent = pick(table, def("Actor_RootComponent"), {"Actor_RootComponent", "RootComponent"});
    g.SceneComponent_RelativeLocation = pick(table, def("SceneComponent_RelativeLocation"), {"SceneComponent_RelativeLocation", "RelativeLocation"});
    g.PlayerState_PlayerName = pick(table, def("PlayerState_PlayerName"), {"PlayerState_PlayerName", "PlayerName", "PlayerNamePrivate"});
    g.PlayerState_PlayerNameAlt = pick(table, def("PlayerState_PlayerNameAlt"), {"PlayerState_PlayerNameAlt"});
    g.PlayerState_TeamIndex = pick(table, def("PlayerState_TeamIndex"), {"PlayerState_TeamIndex", "TeamIndex"});
    g.PlayerState_IsABot = pick(table, def("PlayerState_IsABot"), {"PlayerState_IsABot", "IsABot", "bIsABot"});
    g.Mesh_BoneArray = pick(table, def("Mesh_BoneArray"), {"Mesh_BoneArray", "BoneArray"});
    g.Mesh_ComponentToWorld = pick(table, def("Mesh_ComponentToWorld"), {"Mesh_ComponentToWorld", "ComponentToWorld"});

    diag::offsetsCount = static_cast<int>(sizeof(kDefaults) / sizeof(kDefaults[0]));

    const std::wstring dumpPath = exe_directory() + L"\\offsets.json";
    std::string probe;
    if (!read_file(dumpPath, probe)) {
        dump_json(dumpPath);
        std::wprintf(L"  [info] offsets.json genere : %s\n", dumpPath.c_str());
    }

    std::wprintf(L"  [ok] %d offsets resolus (defauts 42.40 + surcharges)\n", diag::offsetsCount);
    return true;
}

} // namespace offsets

// ---------------------------------------------------------------------------
// SDK
// ---------------------------------------------------------------------------

namespace sdk {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rot {
    double pitch = 0.0;
    double yaw = 0.0;
    double roll = 0.0;
};

struct Player {
    uintptr_t pawn = 0;
    uintptr_t mesh = 0;
    Vec3 location;
    std::wstring name;
    int team = 0;
    bool bot = false;
    bool dbno = false;
    bool local = false;
    float distance = 0.0f;
};

struct Cache {
    uintptr_t pawn = 0;
    uintptr_t playerState = 0;
    std::wstring name;
};

uintptr_t g_localPawn = 0;
uintptr_t g_controller = 0;
int g_localTeam = 0;
bool g_cameraValid = false;
Vec3 g_cameraLocation;
Rot g_cameraRotation;
double g_cameraFov = 90.0;
std::vector<Player> g_players;
std::vector<Cache> g_cache;
ULONGLONG g_cacheTime = 0;
int g_screenWidth = 1920;
int g_screenHeight = 1080;

std::wstring read_fstring(uintptr_t address) {
    const uintptr_t data = mem::read<uintptr_t>(address);
    const int32_t count = mem::read<int32_t>(address + 8);
    if (!data || count <= 0 || count > 64) {
        return {};
    }

    wchar_t buffer[64]{};
    SIZE_T bytes = 0;
    ReadProcessMemory(mem::g_process, reinterpret_cast<LPCVOID>(data), buffer,
                      static_cast<SIZE_T>(count) * sizeof(wchar_t), &bytes);

    std::wstring name(buffer, bytes / sizeof(wchar_t));
    while (!name.empty() && name.back() == L'\0') {
        name.pop_back();
    }
    return name;
}

// Le global UWorld est chiffre sur 42.40 : on passe par gEngine -> GameViewport -> World.
uintptr_t resolve_world() {
    if (offsets::g.GameEngine && offsets::g.GameEngine_GameViewport &&
        offsets::g.GameViewport_World) {
        const uintptr_t engine = mem::read<uintptr_t>(mem::g_base + offsets::g.GameEngine);
        if (engine) {
            const uintptr_t viewport =
                mem::read<uintptr_t>(engine + offsets::g.GameEngine_GameViewport);
            if (viewport) {
                const uintptr_t world =
                    mem::read<uintptr_t>(viewport + offsets::g.GameViewport_World);
                if (world) {
                    return world;
                }
            }
        }
    }

    if (offsets::g.GWorld) {
        const uintptr_t world = mem::read<uintptr_t>(mem::g_base + offsets::g.GWorld);
        if (world) {
            return world;
        }
    }

    return 0;
}

// ActorCluster -> Actors sur les builds recents, lecture directe en secours.
uintptr_t resolve_actors(uintptr_t level, int32_t& count) {
    count = 0;

    if (offsets::g.Level_ActorCluster && offsets::g.LevelActorContainer_Actors) {
        const uintptr_t cluster = mem::read<uintptr_t>(level + offsets::g.Level_ActorCluster);
        if (cluster) {
            const uintptr_t actors =
                mem::read<uintptr_t>(cluster + offsets::g.LevelActorContainer_Actors);
            const int32_t total =
                mem::read<int32_t>(cluster + offsets::g.LevelActorContainer_Actors + 8);
            if (actors && total > 0 && total < 40000) {
                count = total;
                return actors;
            }
        }
    }

    static const uintptr_t kDirect[] = {0x50, 0x98, 0xA0, 0xA8, 0xB0};
    for (const uintptr_t offset : kDirect) {
        const uintptr_t actors = mem::read<uintptr_t>(level + offset);
        const int32_t total = mem::read<int32_t>(level + offset + 8);
        if (!actors || total <= 0 || total > 40000) {
            continue;
        }

        const uintptr_t first = mem::read<uintptr_t>(actors);
        if (first && mem::read<uintptr_t>(first + offsets::g.Actor_RootComponent)) {
            count = total;
            return actors;
        }
    }

    return 0;
}

void refresh_cache() {
    g_cache.clear();

    const uintptr_t world = resolve_world();
    if (!world) {
        diag::worldOk = false;
        return;
    }
    diag::worldOk = true;

    const uintptr_t levels = mem::read<uintptr_t>(world + offsets::g.World_Levels);
    if (!levels) {
        return;
    }

    const uintptr_t levelData = mem::read<uintptr_t>(levels);
    const int32_t levelCount = mem::read<int32_t>(levels + 8);
    if (!levelData || levelCount <= 0 || levelCount > 64) {
        return;
    }

    for (int32_t levelIndex = 0; levelIndex < levelCount; ++levelIndex) {
        const uintptr_t level = mem::read<uintptr_t>(levelData + levelIndex * 8);
        if (!level) {
            continue;
        }

        int32_t actorCount = 0;
        const uintptr_t actors = resolve_actors(level, actorCount);
        if (!actors) {
            continue;
        }

        std::vector<uintptr_t> pointers(static_cast<size_t>(actorCount), 0);
        SIZE_T bytes = 0;
        if (!ReadProcessMemory(mem::g_process, reinterpret_cast<LPCVOID>(actors), pointers.data(),
                               pointers.size() * sizeof(uintptr_t), &bytes)) {
            continue;
        }

        for (const uintptr_t actor : pointers) {
            if (!actor) {
                continue;
            }

            const uintptr_t playerState = mem::read<uintptr_t>(actor + offsets::g.Pawn_PlayerState);
            const uintptr_t mesh = mem::read<uintptr_t>(actor + offsets::g.Pawn_Mesh);
            if (!playerState || !mesh) {
                continue;
            }

            Cache cache;
            cache.pawn = actor;
            cache.playerState = playerState;
            if (offsets::g.PlayerState_PlayerName) {
                cache.name = read_fstring(playerState + offsets::g.PlayerState_PlayerName);
                if (cache.name.empty() && offsets::g.PlayerState_PlayerNameAlt) {
                    cache.name = read_fstring(playerState + offsets::g.PlayerState_PlayerNameAlt);
                }
            }
            g_cache.push_back(cache);
        }
    }

    g_cacheTime = GetTickCount64();
}

void read_camera(uintptr_t controller) {
    g_cameraValid = false;

    const uintptr_t manager =
        mem::read<uintptr_t>(controller + offsets::g.PlayerController_PlayerCameraManager);
    if (!manager) {
        return;
    }

    const uintptr_t pov = manager + offsets::g.CameraManager_CameraCache + offsets::g.CameraCache_POV;

    g_cameraLocation = mem::read<Vec3>(pov);
    g_cameraRotation = mem::read<Rot>(pov + 0x18);

    // FMinimalViewInfo::FOV est un float en UE5, pas un double.
    g_cameraFov = mem::read<float>(pov + 0x30);
    if (g_cameraFov < 1.0f || g_cameraFov > 170.0f) {
        g_cameraFov = 90.0;
    }

    g_cameraValid = true;
}

void update() {
    g_players.clear();
    g_cameraValid = false;

    if (GetTickCount64() - g_cacheTime > 1500 || g_cache.empty()) {
        refresh_cache();
    }

    const uintptr_t world = resolve_world();
    if (!world) {
        return;
    }

    const uintptr_t gameInstance = mem::read<uintptr_t>(world + offsets::g.World_OwningGameInstance);
    if (!gameInstance) {
        return;
    }

    const uintptr_t localPlayers =
        mem::read<uintptr_t>(gameInstance + offsets::g.GameInstance_LocalPlayers);
    if (!localPlayers) {
        return;
    }

    const uintptr_t localPlayer = mem::read<uintptr_t>(localPlayers);
    if (!localPlayer) {
        return;
    }

    const uintptr_t controller =
        mem::read<uintptr_t>(localPlayer + offsets::g.LocalPlayer_PlayerController);
    if (!controller) {
        return;
    }

    g_controller = controller;
    g_localPawn = mem::read<uintptr_t>(controller + offsets::g.PlayerController_AcknowledgedPawn);

    read_camera(controller);

    if (g_localPawn) {
        const uintptr_t localState = mem::read<uintptr_t>(g_localPawn + offsets::g.Pawn_PlayerState);
        g_localTeam = localState
                          ? static_cast<int>(mem::read<uint8_t>(localState + offsets::g.PlayerState_TeamIndex))
                          : 0;
    }

    for (const Cache& cache : g_cache) {
        const uintptr_t mesh = mem::read<uintptr_t>(cache.pawn + offsets::g.Pawn_Mesh);
        if (!mesh) {
            continue;
        }

        const uintptr_t root = mem::read<uintptr_t>(cache.pawn + offsets::g.Actor_RootComponent);
        if (!root) {
            continue;
        }

        Player player;
        player.pawn = cache.pawn;
        player.mesh = mesh;
        player.name = cache.name;
        player.location = mem::read<Vec3>(root + offsets::g.SceneComponent_RelativeLocation);
        player.team = static_cast<int>(
            mem::read<uint8_t>(cache.playerState + offsets::g.PlayerState_TeamIndex));
        player.bot =
            (mem::read<uint8_t>(cache.playerState + offsets::g.PlayerState_IsABot) & 0x08) != 0;
        player.dbno = offsets::g.Pawn_IsDBNO
                          ? (mem::read<uint8_t>(cache.pawn + offsets::g.Pawn_IsDBNO) & 0x01) != 0
                          : false;
        player.local = cache.pawn == g_localPawn;
        player.distance = static_cast<float>(
            std::sqrt(std::pow(player.location.x - g_cameraLocation.x, 2) +
                      std::pow(player.location.y - g_cameraLocation.y, 2) +
                      std::pow(player.location.z - g_cameraLocation.z, 2)) /
            100.0);

        g_players.push_back(player);
    }

    std::sort(g_players.begin(), g_players.end(),
              [](const Player& a, const Player& b) { return a.distance < b.distance; });
}

bool world_to_screen(const Vec3& world, Vec2& out) {
    if (!g_cameraValid || g_screenWidth <= 0 || g_screenHeight <= 0) {
        return false;
    }

    constexpr double pi = 3.14159265358979323846;
    const double pitch = g_cameraRotation.pitch * pi / 180.0;
    const double yaw = g_cameraRotation.yaw * pi / 180.0;

    const Vec3 forward{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                       std::sin(pitch)};
    const Vec3 right{-std::sin(yaw), std::cos(yaw), 0.0};
    const Vec3 up{-std::cos(yaw) * std::sin(pitch), -std::sin(yaw) * std::sin(pitch),
                  std::cos(pitch)};

    const Vec3 delta{world.x - g_cameraLocation.x, world.y - g_cameraLocation.y,
                     world.z - g_cameraLocation.z};

    const double depth = delta.x * forward.x + delta.y * forward.y + delta.z * forward.z;
    if (depth < 1.0) {
        return false;
    }

    const double horizontal = delta.x * right.x + delta.y * right.y;
    const double vertical = delta.x * up.x + delta.y * up.y + delta.z * up.z;

    const double tanHalf = std::tan(g_cameraFov * pi / 360.0);
    const double aspect = static_cast<double>(g_screenWidth) / static_cast<double>(g_screenHeight);

    out.x = static_cast<float>(g_screenWidth / 2.0 *
                               (1.0 + horizontal / (depth * tanHalf * aspect)));
    out.y = static_cast<float>(g_screenHeight / 2.0 * (1.0 - vertical / (depth * tanHalf)));
    return true;
}

Vec3 aim_point(const Player& player) {
    return Vec3{player.location.x, player.location.y, player.location.z + cfg::kAimHeight};
}

bool is_enemy(const Player& player) {
    return !player.local && (g_localTeam == 0 || player.team != g_localTeam);
}

} // namespace sdk

// ---------------------------------------------------------------------------
// Overlay GDI (layered, click-through)
// ---------------------------------------------------------------------------

namespace overlay {

HWND g_window = nullptr;
HDC g_dc = nullptr;
HBITMAP g_bitmap = nullptr;
uint32_t* g_pixels = nullptr;
int g_width = 0;
int g_height = 0;
HFONT g_font = nullptr;

bool g_visible = false;
bool g_anchored = false;
bool g_statusMode = false;   // pas de fenetre de jeu : petite boite de statut

COLORREF to_gdi(uint32_t color) {
    return RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
}

bool resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (width == g_width && height == g_height && g_pixels) {
        return true;
    }

    if (g_bitmap) {
        DeleteObject(g_bitmap);
        g_bitmap = nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    g_bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&g_pixels),
                                nullptr, 0);
    if (!g_bitmap) {
        return false;
    }

    SelectObject(g_dc, g_bitmap);
    g_width = width;
    g_height = height;
    return true;
}

bool create(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"RetracExternalOverlay";
    RegisterClassExW(&windowClass);

    g_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                   WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               windowClass.lpszClassName, L"Retrac OGFN", WS_POPUP, 0, 0, 100, 100,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        return false;
    }

    g_dc = CreateCompatibleDC(nullptr);
    g_font = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         L"Segoe UI");
    SelectObject(g_dc, g_font);

    SetBkMode(g_dc, TRANSPARENT);
    return true;
}

void hide() {
    g_anchored = false;
    if (g_window && g_visible) {
        ShowWindow(g_window, SW_HIDE);
        g_visible = false;
    }
}

void show() {
    if (g_window && !g_visible) {
        ShowWindow(g_window, SW_SHOWNOACTIVATE);
        g_visible = true;
    }
}

void place(int x, int y, int width, int height) {
    RECT current{};
    GetWindowRect(g_window, &current);
    if (current.left != x || current.top != y || current.right - current.left != width ||
        current.bottom - current.top != height) {
        SetWindowPos(g_window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }
}

bool sync() {
    HWND game = mem::g_window;

    if (game && IsWindow(game) && !IsIconic(game)) {
        RECT client{};
        if (GetClientRect(game, &client) && client.right >= 320 && client.bottom >= 240) {
            POINT origin{0, 0};
            ClientToScreen(game, &origin);

            if (resize(client.right, client.bottom)) {
                place(origin.x, origin.y, client.right, client.bottom);
                sdk::g_screenWidth = client.right;
                sdk::g_screenHeight = client.bottom;
                g_statusMode = false;
                g_anchored = true;
                return true;
            }
        }
    }

    // Pas de jeu : petite boite de statut en haut a gauche, pour voir ou ca bloque.
    constexpr int kBoxWidth = 560;
    constexpr int kBoxHeight = 230;
    if (resize(kBoxWidth, kBoxHeight)) {
        place(24, 24, kBoxWidth, kBoxHeight);
        sdk::g_screenWidth = kBoxWidth;
        sdk::g_screenHeight = kBoxHeight;
        g_statusMode = true;
        g_anchored = true;
        return true;
    }

    hide();
    return false;
}

void clear() {
    if (g_pixels) {
        memset(g_pixels, 0, static_cast<size_t>(g_width) * g_height * 4);
    }
}

void present() {
    if (!g_pixels || !g_anchored) {
        return;
    }

    const size_t count = static_cast<size_t>(g_width) * g_height;
    for (size_t index = 0; index < count; ++index) {
        const uint32_t pixel = g_pixels[index] & 0x00FFFFFFu;
        g_pixels[index] = pixel ? (pixel | 0xFF000000u) : 0u;
    }

    RECT rect{};
    GetWindowRect(g_window, &rect);

    POINT source{0, 0};
    POINT destination{rect.left, rect.top};
    SIZE size{g_width, g_height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g_window, screen, &destination, &size, g_dc, &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);

    show();
}

void rect(int x, int y, int width, int height, uint32_t color, int thickness) {
    HGDIOBJ oldBrush = SelectObject(g_dc, GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, thickness, to_gdi(color));
    HGDIOBJ oldPen = SelectObject(g_dc, pen);

    Rectangle(g_dc, x, y, x + width, y + height);

    SelectObject(g_dc, oldPen);
    DeleteObject(pen);
    SelectObject(g_dc, oldBrush);
}

void filled(int x, int y, int width, int height, uint32_t color) {
    RECT area{x, y, x + width, y + height};
    HBRUSH brush = CreateSolidBrush(to_gdi(color));
    FillRect(g_dc, &area, brush);
    DeleteObject(brush);
}

void text(int x, int y, const std::wstring& value, uint32_t color, bool centered) {
    if (value.empty()) {
        return;
    }

    SIZE size{};
    GetTextExtentPoint32W(g_dc, value.c_str(), static_cast<int>(value.size()), &size);
    if (centered) {
        x -= size.cx / 2;
    }

    SetTextColor(g_dc, to_gdi(0x050505));
    TextOutW(g_dc, x - 1, y, value.c_str(), static_cast<int>(value.size()));
    TextOutW(g_dc, x + 1, y, value.c_str(), static_cast<int>(value.size()));
    TextOutW(g_dc, x, y - 1, value.c_str(), static_cast<int>(value.size()));
    TextOutW(g_dc, x, y + 1, value.c_str(), static_cast<int>(value.size()));

    SetTextColor(g_dc, to_gdi(color));
    TextOutW(g_dc, x, y, value.c_str(), static_cast<int>(value.size()));
}

void circle(int centerX, int centerY, int radius, uint32_t color) {
    HPEN pen = CreatePen(PS_SOLID, 1, to_gdi(color));
    HGDIOBJ oldPen = SelectObject(g_dc, pen);
    HGDIOBJ oldBrush = SelectObject(g_dc, GetStockObject(NULL_BRUSH));

    constexpr int kSegments = 48;
    MoveToEx(g_dc, centerX + radius, centerY, nullptr);
    for (int index = 1; index <= kSegments; ++index) {
        constexpr double pi = 3.14159265358979323846;
        const double angle = index * 2.0 * pi / kSegments;
        LineTo(g_dc, centerX + static_cast<int>(radius * std::cos(angle)),
               centerY + static_cast<int>(radius * std::sin(angle)));
    }

    SelectObject(g_dc, oldBrush);
    SelectObject(g_dc, oldPen);
    DeleteObject(pen);
}

} // namespace overlay

// ---------------------------------------------------------------------------
// Entrees
// ---------------------------------------------------------------------------

namespace input {

bool down(int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

bool pressed(int key) {
    static std::map<int, bool> previous;

    const bool now = down(key);
    const bool was = previous[key];
    previous[key] = now;
    return now && !was;
}

void move_mouse(float dx, float dy) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(std::lround(dx));
    input.mi.dy = static_cast<LONG>(std::lround(dy));
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

} // namespace input

// ---------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------

namespace features {

sdk::Player* pick_target(float fov, bool ignoreFov) {
    sdk::Player* best = nullptr;
    float bestScore = 1e9f;

    const float centerX = sdk::g_screenWidth / 2.0f;
    const float centerY = sdk::g_screenHeight / 2.0f;

    for (sdk::Player& player : sdk::g_players) {
        if (player.local || player.distance > cfg::kMaxDistance || !sdk::is_enemy(player)) {
            continue;
        }

        sdk::Vec2 screen{};
        if (!sdk::world_to_screen(sdk::aim_point(player), screen)) {
            continue;
        }

        const float dx = screen.x - centerX;
        const float dy = screen.y - centerY;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (!ignoreFov && distance > fov) {
            continue;
        }

        if (distance < bestScore) {
            bestScore = distance;
            best = &player;
        }
    }

    return best;
}

void aimbot(bool active) {
    if (!active) {
        return;
    }

    sdk::Player* target = pick_target(cfg::kAimbotFov, false);
    if (!target) {
        return;
    }

    sdk::Vec2 screen{};
    if (!sdk::world_to_screen(sdk::aim_point(*target), screen)) {
        return;
    }

    const float dx = screen.x - sdk::g_screenWidth / 2.0f;
    const float dy = screen.y - sdk::g_screenHeight / 2.0f;
    input::move_mouse(dx / cfg::kAimbotSmooth, dy / cfg::kAimbotSmooth);
}

void bullet_tp(bool active) {
    if (!active) {
        return;
    }

    sdk::Player* target = pick_target(cfg::kAimbotFov * 2.0f, true);
    if (!target) {
        return;
    }

    const sdk::Vec3 aim = sdk::aim_point(*target);

    if (offsets::g.PlayerController_ControlRotation && sdk::g_controller) {
        constexpr double pi = 3.14159265358979323846;
        const double dx = aim.x - sdk::g_cameraLocation.x;
        const double dy = aim.y - sdk::g_cameraLocation.y;
        const double dz = aim.z - sdk::g_cameraLocation.z;
        const double horizontal = std::sqrt(dx * dx + dy * dy);

        sdk::Rot rotation;
        rotation.pitch = std::atan2(dz, horizontal) * 180.0 / pi;
        rotation.yaw = std::atan2(dy, dx) * 180.0 / pi;
        rotation.roll = 0.0;

        mem::write(sdk::g_controller + offsets::g.PlayerController_ControlRotation, rotation);
    }

    sdk::Vec2 screen{};
    if (sdk::world_to_screen(aim, screen)) {
        input::move_mouse(screen.x - sdk::g_screenWidth / 2.0f,
                          screen.y - sdk::g_screenHeight / 2.0f);
    }
}

void esp(bool enabled) {
    for (const sdk::Player& player : sdk::g_players) {
        if (player.local || player.distance > cfg::kMaxDistance) {
            continue;
        }

        sdk::Vec2 feet{};
        sdk::Vec2 head{};
        const sdk::Vec3 bottom{player.location.x, player.location.y,
                               player.location.z + cfg::kCapsuleBottom};
        const sdk::Vec3 top{player.location.x, player.location.y,
                            player.location.z + cfg::kCapsuleTop};

        if (!sdk::world_to_screen(bottom, feet) || !sdk::world_to_screen(top, head)) {
            continue;
        }

        const float height = feet.y - head.y;
        if (height < 4.0f) {
            continue;
        }

        const float width = height * 0.55f;
        const int left = static_cast<int>(std::lround(head.x - width / 2.0f));
        const int topY = static_cast<int>(std::lround(head.y));
        const int boxWidth = static_cast<int>(std::lround(width));
        const int boxHeight = static_cast<int>(std::lround(height));

        uint32_t color = cfg::kColorEnemy;
        if (player.bot) {
            color = cfg::kColorBot;
        } else if (sdk::g_localTeam != 0 && player.team == sdk::g_localTeam) {
            color = cfg::kColorTeam;
        }

        overlay::rect(left, topY, boxWidth, boxHeight, color, 1);

        if (!player.name.empty()) {
            overlay::text(left + boxWidth / 2, topY - 17, player.name, color, true);
        }

        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%.0f m%s%s", player.distance, player.dbno ? L"  (a terre)" : L"",
                   player.bot ? L"  [bot]" : L"");
        overlay::text(left + boxWidth / 2, topY + boxHeight + 2, buffer, cfg::kColorText, true);

        if (enabled) {
            sdk::Vec2 aimScreen{};
            if (sdk::world_to_screen(sdk::aim_point(player), aimScreen)) {
                overlay::filled(static_cast<int>(aimScreen.x) - 2, static_cast<int>(aimScreen.y) - 2,
                                4, 4, color);
            }
        }
    }
}

void legend(bool espOn, int fps, bool memoryTp) {
    wchar_t buffer[128]{};

    overlay::text(10, 8, L"Retrac OGFN - external", cfg::kColorText, false);

    swprintf_s(buffer, L"Inser : ESP (%s)", espOn ? L"actif" : L"inactif");
    overlay::text(10, 26, buffer, espOn ? cfg::kColorLocal : cfg::kColorDim, false);

    overlay::text(10, 42, L"Clic droit : aimbot", cfg::kColorDim, false);
    swprintf_s(buffer, L"Y : bullet TP (%s)", memoryTp ? L"rotation memoire" : L"snap souris");
    overlay::text(10, 58, buffer, cfg::kColorDim, false);
    overlay::text(10, 74, L"F8 : quitter", cfg::kColorDim, false);

    swprintf_s(buffer, L"%d joueurs  |  %d fps", static_cast<int>(sdk::g_players.size()), fps);
    overlay::text(10, 98, buffer, cfg::kColorDim, false);
}

void status(int fps) {
    wchar_t buffer[192]{};
    int y = 10;

    overlay::text(12, y, L"Retrac OGFN - external  (pas de fenetre de jeu)", cfg::kColorText, false);
    y += 24;

    swprintf_s(buffer, L"process Fortnite : %s", diag::processOk ? L"attache" : L"en attente...");
    overlay::text(12, y, buffer, diag::processOk ? cfg::kColorLocal : cfg::kColorBot, false);
    y += 20;

    swprintf_s(buffer, L"offsets : %d (defauts 42.40 + offsets.json)", diag::offsetsCount);
    overlay::text(12, y, buffer, cfg::kColorDim, false);
    y += 20;

    swprintf_s(buffer, L"UWorld : %s", diag::worldOk ? L"OK" : L"en attente...");
    overlay::text(12, y, buffer, diag::worldOk ? cfg::kColorLocal : cfg::kColorBot, false);
    y += 20;

    swprintf_s(buffer, L"fenetre de jeu : %s", mem::g_window ? L"trouvee" : L"absente");
    overlay::text(12, y, buffer, mem::g_window ? cfg::kColorLocal : cfg::kColorBot, false);
    y += 20;

    swprintf_s(buffer, L"joueurs : %d   |   %d fps", static_cast<int>(sdk::g_players.size()), fps);
    overlay::text(12, y, buffer, cfg::kColorDim, false);
    y += 24;

    overlay::text(12, y, L"Fortnite doit etre en fenetre sans bordure, pas en plein ecran exclusif.",
                  cfg::kColorDim, false);
}

} // namespace features

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

namespace {

FILE* g_logFile = nullptr;

void log(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    std::vwprintf(format, args);
    va_end(args);
    fflush(stdout);

    if (!g_logFile && !diag::logPath.empty()) {
        _wfopen_s(&g_logFile, diag::logPath.c_str(), L"w, ccs=UTF-8");
    }
    if (g_logFile) {
        va_list fileArgs;
        va_start(fileArgs, format);
        vfwprintf(g_logFile, format, fileArgs);
        va_end(fileArgs);
        fflush(g_logFile);
    }
}

void attach_console() {
    if (!AllocConsole()) {
        return;
    }

    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    freopen_s(&dummy, "CONIN$", "r", stdin);   // sinon getchar() renvoie EOF direct

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Retrac OGFN - external");
}

void banner() {
    log(L"\n");
    log(L"  Retrac OGFN - external\n");
    log(L"  --------------------------------------------\n");
}

void wait_exit() {
    log(L"\n  Entree pour quitter...");
    getchar();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    attach_console();

    {
        wchar_t exePath[MAX_PATH * 2]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH * 2);
        std::wstring directory = exePath;
        const size_t slash = directory.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            directory.resize(slash);
        }
        diag::logPath = directory + L"\\RetracExternal.log";
    }

    banner();

    if (mem::enable_debug_privilege()) {
        log(L"  [ok] SeDebugPrivilege\n");
    } else {
        log(L"  [avert] SeDebugPrivilege indisponible (lance en administrateur)\n");
    }

    if (!offsets::load()) {
        log(L"  [erreur] chargement offsets impossible\n");
        wait_exit();
        return 1;
    }

    log(L"  [info] recherche de Fortnite...\n");

    DWORD pid = 0;
    for (int attempt = 0; attempt < 240 && !pid; ++attempt) {
        pid = mem::find_process();
        if (!pid) {
            if (attempt % 4 == 0) {
                log(L".");
            }
            Sleep(500);
        }
    }
    if (!pid) {
        log(L"\n  [erreur] FortniteClient-Win64-Shipping.exe introuvable\n");
        wait_exit();
        return 1;
    }

    if (!mem::attach(pid)) {
        log(L"\n  [erreur] attache au process %lu impossible\n", pid);
        wait_exit();
        return 1;
    }
    diag::processOk = true;

    log(L"\n  [ok] process %lu, base 0x%llX\n", pid,
        static_cast<unsigned long long>(mem::g_base));

    if (!overlay::create(instance)) {
        log(L"  [erreur] creation overlay impossible\n");
        wait_exit();
        return 1;
    }

    log(L"  [ok] overlay pret. Inser = ESP, clic droit = aimbot, Y = bullet TP, F8 = quitter\n");

    const bool memoryTp = offsets::g.PlayerController_ControlRotation != 0;
    bool espOn = true;
    int frames = 0;
    int fps = 0;
    ULONGLONG fpsTime = GetTickCount64();
    ULONGLONG windowRetry = GetTickCount64();
    bool announcedWindow = false;

    bool running = true;
    while (running) {
        if (!mem::g_process || WaitForSingleObject(mem::g_process, 0) != WAIT_TIMEOUT) {
            log(L"  [info] Fortnite ferme, sortie\n");
            break;
        }

        if (!mem::g_window && GetTickCount64() - windowRetry > 1000) {
            windowRetry = GetTickCount64();
            mem::g_window = mem::find_window(pid);
            if (mem::g_window && !announcedWindow) {
                log(L"  [ok] fenetre de jeu trouvee\n");
                announcedWindow = true;
            }
        }

        if (input::pressed(cfg::kEspKey)) {
            espOn = !espOn;
        }
        if (input::pressed(cfg::kQuitKey)) {
            running = false;
        }

        overlay::sync();
        overlay::clear();

        sdk::update();

        if (overlay::g_statusMode) {
            features::status(fps);
        } else {
            features::esp(espOn);
            features::aimbot(input::down(cfg::kAimbotKey));
            features::bullet_tp(input::down(cfg::kBulletTpKey));

            if (sdk::g_cameraValid) {
                overlay::circle(sdk::g_screenWidth / 2, sdk::g_screenHeight / 2,
                                static_cast<int>(cfg::kAimbotFov), 0x303030);
            }

            features::legend(espOn, fps, memoryTp);
        }

        overlay::present();

        ++frames;
        if (GetTickCount64() - fpsTime >= 1000) {
            fps = frames;
            frames = 0;
            fpsTime = GetTickCount64();
        }

        Sleep(1000 / cfg::kTargetFps > 0 ? 1000 / cfg::kTargetFps : 1);
    }

    if (mem::g_process) {
        CloseHandle(mem::g_process);
    }
    if (overlay::g_window) {
        DestroyWindow(overlay::g_window);
    }

    wait_exit();
    return 0;
}
