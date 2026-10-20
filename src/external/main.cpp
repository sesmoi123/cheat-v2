// Retrac OGFN - external. Cible : Fortnite 14.60 (Chapter 2 Season 4, UE4.26).
// Un seul exe, aucune injection : ReadProcessMemory / WriteProcessMemory,
// overlay GDI layered (ESP + panel), souris (aimbot + bullet TP).
//
// UE4.26 => FVector / FRotator sont des FLOATS (12 octets), pas des doubles.
// GWorld est un global non chiffre : trouve par signature, puis valide.

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

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

namespace cfg {

constexpr int kPanelKey = VK_INSERT;
constexpr int kQuitKey = VK_F8;

constexpr int kKeyBox = VK_F1;
constexpr int kKeyName = VK_F2;
constexpr int kKeyDistance = VK_F3;
constexpr int kKeyHealth = VK_F4;
constexpr int kKeySnapline = VK_F5;
constexpr int kKeyCrosshair = VK_F6;

constexpr int kAimbotKey = VK_RBUTTON;   // maintenu
constexpr int kBulletTpKey = 'Y';        // maintenu

constexpr float kAimbotFov = 200.0f;     // rayon en pixels
constexpr float kAimbotSmooth = 3.0f;    // 1 = instantane
constexpr float kMaxDistance = 300.0f;   // metres
constexpr float kAimHeight = 60.0f;      // cm au-dessus du root (poitrine)
constexpr float kCapsuleBottom = -88.0f; // pieds
constexpr float kCapsuleTop = 92.0f;     // tete
constexpr int kTargetFps = 120;

constexpr uint32_t kAccent = 0x3D8BFF;
constexpr uint32_t kPanelBack = 0x0B0F16;
constexpr uint32_t kPanelHead = 0x16233A;
constexpr uint32_t kText = 0xE8EDF5;
constexpr uint32_t kDim = 0x8A94A6;
constexpr uint32_t kKey = 0x7FB2FF;
constexpr uint32_t kOn = 0x35E07A;
constexpr uint32_t kOff = 0x6B7280;
constexpr uint32_t kLocal = 0x35E07A;
constexpr uint32_t kTeam = 0x00C8FF;
constexpr uint32_t kEnemy = 0xFF4D4D;
constexpr uint32_t kBot = 0xFFB300;

struct Settings {
    bool panel = true;
    bool box = true;
    bool name = true;
    bool distance = true;
    bool health = true;
    bool snapline = false;
    bool crosshair = true;
};

Settings g;

} // namespace cfg

// ---------------------------------------------------------------------------
// Diag
// ---------------------------------------------------------------------------

namespace diag {

bool attached = false;
bool worldOk = false;
bool cameraOk = false;
int offsetsCount = 0;
int patchCount = 0;
std::wstring logPath;

} // namespace diag

// ---------------------------------------------------------------------------
// Process / memoire
// ---------------------------------------------------------------------------

namespace mem {

HANDLE g_process = nullptr;
uintptr_t g_base = 0;
size_t g_imageSize = 0;
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

std::wstring lower(const wchar_t* raw) {
    std::wstring out;
    for (const wchar_t* cursor = raw; *cursor; ++cursor) {
        out.push_back(static_cast<wchar_t>(::towlower(*cursor)));
    }
    return out;
}

// Le process EAC (..._EAC.exe) est un stub qui lance le vrai jeu puis se ferme :
// on privilegie le nom exact.
DWORD find_process() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD exact = 0;
    DWORD loose = 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                continue;
            }

            const std::wstring name = lower(entry.szExeFile);
            if (name == L"fortniteclient-win64-shipping.exe") {
                exact = entry.th32ProcessID;
                break;
            }
            if (!loose && name.rfind(L"fortniteclient", 0) == 0) {
                loose = entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return exact ? exact : loose;
}

uintptr_t module_base(DWORD pid, const wchar_t* module, size_t* sizeOut) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    uintptr_t exact = 0;
    uintptr_t loose = 0;
    size_t exactSize = 0;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, module) == 0) {
                exact = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                exactSize = static_cast<size_t>(entry.modBaseSize);
                break;
            }
            if (!loose && wcsstr(entry.szModule, L"FortniteClient")) {
                loose = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                exactSize = static_cast<size_t>(entry.modBaseSize);
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    if (sizeOut) {
        *sizeOut = exactSize;
    }
    return exact ? exact : loose;
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

    for (int attempt = 0; attempt < 40 && !g_process; ++attempt) {
        for (const DWORD mask : kMasks) {
            g_process = OpenProcess(mask, FALSE, pid);
            if (g_process) {
                break;
            }
        }
        if (!g_process) {
            Sleep(250);
        }
    }
    if (!g_process) {
        return false;
    }

    g_pid = pid;
    g_base = module_base(pid, L"FortniteClient-Win64-Shipping.exe", &g_imageSize);
    if (!g_base) {
        CloseHandle(g_process);
        g_process = nullptr;
        return false;
    }
    return true;
}

void detach() {
    if (g_process) {
        CloseHandle(g_process);
        g_process = nullptr;
    }
    g_base = 0;
    g_pid = 0;
    g_window = nullptr;
}

bool alive() {
    return g_process && WaitForSingleObject(g_process, 0) == WAIT_TIMEOUT;
}

// --- sections PE + scan de signature --------------------------------------

struct Section {
    uintptr_t address = 0;
    size_t size = 0;
    uint32_t characteristics = 0;
};

std::vector<Section> sections() {
    std::vector<Section> out;

    uint8_t dos[0x40]{};
    SIZE_T bytes = 0;
    if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(g_base), dos, sizeof(dos), &bytes)) {
        return out;
    }
    if (dos[0] != 'M' || dos[1] != 'Z') {
        return out;
    }

    const uint32_t peOffset = *reinterpret_cast<uint32_t*>(dos + 0x3C);
    if (!peOffset || peOffset > 0x1000) {
        return out;
    }

    uint8_t header[0x400]{};
    if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(g_base + peOffset), header,
                           sizeof(header), &bytes)) {
        return out;
    }
    if (header[0] != 'P' || header[1] != 'E') {
        return out;
    }

    const uint16_t sectionCount = *reinterpret_cast<uint16_t*>(header + 4 + 2);
    const uint16_t optionalSize = *reinterpret_cast<uint16_t*>(header + 4 + 16);
    if (sectionCount == 0 || sectionCount > 96) {
        return out;
    }

    const size_t sectionTable = 4 + 20 + optionalSize;

    for (uint16_t index = 0; index < sectionCount; ++index) {
        uint8_t entry[40]{};
        const size_t offset = sectionTable + static_cast<size_t>(index) * 40;
        if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(g_base + peOffset + offset),
                               entry, sizeof(entry), &bytes)) {
            break;
        }

        Section section;
        section.size = *reinterpret_cast<uint32_t*>(entry + 0x08);
        section.address = g_base + *reinterpret_cast<uint32_t*>(entry + 0x0C);
        section.characteristics = *reinterpret_cast<uint32_t*>(entry + 0x24);
        if (section.size && section.address) {
            out.push_back(section);
        }
    }

    return out;
}

uintptr_t scan(const char* signature) {
    std::vector<uint8_t> pattern;
    std::vector<bool> wildcard;

    for (const char* cursor = signature; *cursor;) {
        if (*cursor == ' ') {
            ++cursor;
            continue;
        }
        if (*cursor == '?') {
            pattern.push_back(0);
            wildcard.push_back(true);
            ++cursor;
            if (*cursor == '?') {
                ++cursor;
            }
        } else {
            pattern.push_back(static_cast<uint8_t>(strtoul(cursor, nullptr, 16)));
            wildcard.push_back(false);
            cursor += 2;
        }
    }

    if (pattern.size() < 5) {
        return 0;
    }

    const size_t length = pattern.size();

    for (const Section& section : sections()) {
        if (!(section.characteristics & 0x20000000)) {
            continue;
        }

        constexpr size_t kChunk = 1u << 20;
        std::vector<uint8_t> buffer(kChunk + length);

        for (size_t offset = 0; offset < section.size; offset += kChunk) {
            const size_t want = (std::min)(kChunk + length, section.size - offset);
            SIZE_T got = 0;
            if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(section.address + offset),
                                   buffer.data(), want, &got) ||
                got < length) {
                continue;
            }

            for (size_t index = 0; index + length <= got; ++index) {
                bool match = true;
                for (size_t j = 0; j < length; ++j) {
                    if (!wildcard[j] && buffer[index + j] != pattern[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return section.address + offset + index;
                }
            }
        }
    }

    return 0;
}

} // namespace mem

// ---------------------------------------------------------------------------
// Offsets
// ---------------------------------------------------------------------------

namespace offsets {

struct Table {
    uintptr_t GWorld = 0;
    uintptr_t World_Levels = 0;
    uintptr_t World_OwningGameInstance = 0;
    uintptr_t Level_Actors = 0;
    uintptr_t GameInstance_LocalPlayers = 0;
    uintptr_t LocalPlayer_PlayerController = 0;
    uintptr_t PlayerController_AcknowledgedPawn = 0;
    uintptr_t PlayerController_PlayerCameraManager = 0;
    uintptr_t PlayerController_ControlRotation = 0;
    uintptr_t CameraManager_POV = 0;        // POV absolu depuis le camera manager
    uintptr_t Pawn_PlayerState = 0;
    uintptr_t Pawn_Mesh = 0;
    uintptr_t Pawn_IsDBNO = 0;
    uintptr_t Actor_RootComponent = 0;
    uintptr_t SceneComponent_RelativeLocation = 0;
    uintptr_t PlayerState_PlayerName = 0;
    uintptr_t PlayerState_TeamIndex = 0;
};

Table g;

// 14.60 (Chapter 2 Season 4 / UE4.26). Ces valeurs servent de point de depart :
// la resolution automatique les valide et corrige ce qui ne colle pas.
struct Default {
    const char* key;
    uintptr_t value;
};

const Default kDefaults[] = {
    {"World_Levels", 0x148},
    {"World_OwningGameInstance", 0x188},
    {"Level_Actors", 0x98},
    {"GameInstance_LocalPlayers", 0x38},
    {"LocalPlayer_PlayerController", 0x30},
    {"PlayerController_AcknowledgedPawn", 0x298},
    {"PlayerController_PlayerCameraManager", 0x2B8},
    {"PlayerController_ControlRotation", 0x280},
    {"CameraManager_POV", 0x12C0},
    {"Pawn_PlayerState", 0x238},
    {"Pawn_Mesh", 0x278},
    {"Pawn_IsDBNO", 0x53A},
    {"Actor_RootComponent", 0x130},
    {"SceneComponent_RelativeLocation", 0x11C},
    {"PlayerState_PlayerName", 0x3A0},
    {"PlayerState_TeamIndex", 0xE60},
};

std::string normalize(const std::string& text) {
    std::string out;
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

uintptr_t pick(const std::map<std::string, uintptr_t>& table, const char* key,
               std::initializer_list<const char*> names) {
    const uintptr_t value = lookup(table, names);
    if (value) {
        return value;
    }
    for (const Default& entry : kDefaults) {
        if (strcmp(entry.key, key) == 0) {
            return entry.value;
        }
    }
    return 0;
}

void dump_json(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w") != 0 || !file) {
        return;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"_cible\": \"Fortnite 14.60 (UE4.26) - les vecteurs sont des floats\",\n");
    fprintf(file, "  \"GWorld\": \"0x%llX\",\n", (unsigned long long)g.GWorld);
    fprintf(file, "  \"World_Levels\": \"0x%llX\",\n", (unsigned long long)g.World_Levels);
    fprintf(file, "  \"World_OwningGameInstance\": \"0x%llX\",\n", (unsigned long long)g.World_OwningGameInstance);
    fprintf(file, "  \"Level_Actors\": \"0x%llX\",\n", (unsigned long long)g.Level_Actors);
    fprintf(file, "  \"GameInstance_LocalPlayers\": \"0x%llX\",\n", (unsigned long long)g.GameInstance_LocalPlayers);
    fprintf(file, "  \"LocalPlayer_PlayerController\": \"0x%llX\",\n", (unsigned long long)g.LocalPlayer_PlayerController);
    fprintf(file, "  \"PlayerController_AcknowledgedPawn\": \"0x%llX\",\n", (unsigned long long)g.PlayerController_AcknowledgedPawn);
    fprintf(file, "  \"PlayerController_PlayerCameraManager\": \"0x%llX\",\n", (unsigned long long)g.PlayerController_PlayerCameraManager);
    fprintf(file, "  \"PlayerController_ControlRotation\": \"0x%llX\",\n", (unsigned long long)g.PlayerController_ControlRotation);
    fprintf(file, "  \"CameraManager_POV\": \"0x%llX\",\n", (unsigned long long)g.CameraManager_POV);
    fprintf(file, "  \"Pawn_PlayerState\": \"0x%llX\",\n", (unsigned long long)g.Pawn_PlayerState);
    fprintf(file, "  \"Pawn_Mesh\": \"0x%llX\",\n", (unsigned long long)g.Pawn_Mesh);
    fprintf(file, "  \"Pawn_IsDBNO\": \"0x%llX\",\n", (unsigned long long)g.Pawn_IsDBNO);
    fprintf(file, "  \"Actor_RootComponent\": \"0x%llX\",\n", (unsigned long long)g.Actor_RootComponent);
    fprintf(file, "  \"SceneComponent_RelativeLocation\": \"0x%llX\",\n", (unsigned long long)g.SceneComponent_RelativeLocation);
    fprintf(file, "  \"PlayerState_PlayerName\": \"0x%llX\",\n", (unsigned long long)g.PlayerState_PlayerName);
    fprintf(file, "  \"PlayerState_TeamIndex\": \"0x%llX\"\n", (unsigned long long)g.PlayerState_TeamIndex);
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

    g.GWorld = pick(table, "GWorld", {"GWorld", "UWorld", "uWorld"});
    g.World_Levels = pick(table, "World_Levels", {"World_Levels", "Levels"});
    g.World_OwningGameInstance = pick(table, "World_OwningGameInstance", {"World_OwningGameInstance", "OwningGameInstance"});
    g.Level_Actors = pick(table, "Level_Actors", {"Level_Actors", "Actors", "AActors"});
    g.GameInstance_LocalPlayers = pick(table, "GameInstance_LocalPlayers", {"GameInstance_LocalPlayers", "LocalPlayers"});
    g.LocalPlayer_PlayerController = pick(table, "LocalPlayer_PlayerController", {"LocalPlayer_PlayerController", "PlayerController"});
    g.PlayerController_AcknowledgedPawn = pick(table, "PlayerController_AcknowledgedPawn", {"PlayerController_AcknowledgedPawn", "AcknowledgedPawn"});
    g.PlayerController_PlayerCameraManager = pick(table, "PlayerController_PlayerCameraManager", {"PlayerController_PlayerCameraManager", "PlayerCameraManager"});
    g.PlayerController_ControlRotation = pick(table, "PlayerController_ControlRotation", {"PlayerController_ControlRotation", "ControlRotation"});
    g.CameraManager_POV = pick(table, "CameraManager_POV", {"CameraManager_POV", "CameraCache", "CameraCachePrivate"});
    g.Pawn_PlayerState = pick(table, "Pawn_PlayerState", {"Pawn_PlayerState", "PlayerState"});
    g.Pawn_Mesh = pick(table, "Pawn_Mesh", {"Pawn_Mesh", "Mesh"});
    g.Pawn_IsDBNO = pick(table, "Pawn_IsDBNO", {"Pawn_IsDBNO", "bIsDBNO", "IsDBNO"});
    g.Actor_RootComponent = pick(table, "Actor_RootComponent", {"Actor_RootComponent", "RootComponent"});
    g.SceneComponent_RelativeLocation = pick(table, "SceneComponent_RelativeLocation", {"SceneComponent_RelativeLocation", "RelativeLocation"});
    g.PlayerState_PlayerName = pick(table, "PlayerState_PlayerName", {"PlayerState_PlayerName", "PlayerName", "PlayerNamePrivate"});
    g.PlayerState_TeamIndex = pick(table, "PlayerState_TeamIndex", {"PlayerState_TeamIndex", "TeamIndex"});

    diag::offsetsCount = static_cast<int>(sizeof(kDefaults) / sizeof(kDefaults[0]));
    std::wprintf(L"  [ok] %d offsets de base (14.60)\n", diag::offsetsCount);
    return true;
}

} // namespace offsets

// ---------------------------------------------------------------------------
// SDK (UE4.26 : floats)
// ---------------------------------------------------------------------------

namespace sdk {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rot {
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
};

struct Player {
    uintptr_t pawn = 0;
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
float g_cameraFov = 90.0f;
std::vector<Player> g_players;
std::vector<Cache> g_cache;
ULONGLONG g_cacheTime = 0;
int g_screenWidth = 1920;
int g_screenHeight = 1080;

bool sane_float(float value) {
    return !std::isnan(value) && !std::isinf(value) && std::fabs(value) < 1.0e7f;
}

std::wstring read_fstring(uintptr_t address) {
    const uintptr_t data = mem::read<uintptr_t>(address);
    const int32_t count = mem::read<int32_t>(address + 8);
    if (!data || count <= 0 || count > 48) {
        return {};
    }

    wchar_t buffer[48]{};
    SIZE_T bytes = 0;
    ReadProcessMemory(mem::g_process, reinterpret_cast<LPCVOID>(data), buffer,
                      static_cast<SIZE_T>(count) * sizeof(wchar_t), &bytes);

    std::wstring name(buffer, bytes / sizeof(wchar_t));
    while (!name.empty() && name.back() == L'\0') {
        name.pop_back();
    }

    for (const wchar_t character : name) {
        if (character < 0x20 || character > 0x2500) {
            return {};
        }
    }
    return name;
}

// --- validation ------------------------------------------------------------

bool world_ok(uintptr_t world, uintptr_t levelsOffset) {
    if (!world) {
        return false;
    }

    const uintptr_t levels = mem::read<uintptr_t>(world + levelsOffset);
    if (!levels) {
        return false;
    }

    const uintptr_t data = mem::read<uintptr_t>(levels);
    const int32_t count = mem::read<int32_t>(levels + 8);
    if (!data || count <= 0 || count > 64) {
        return false;
    }

    const uintptr_t level = mem::read<uintptr_t>(data);
    if (!level) {
        return false;
    }

    const uintptr_t actors = mem::read<uintptr_t>(level + offsets::g.Level_Actors);
    const int32_t actorCount = mem::read<int32_t>(level + offsets::g.Level_Actors + 8);
    if (!actors || actorCount < 10 || actorCount > 40000) {
        return false;
    }

    const uintptr_t first = mem::read<uintptr_t>(actors);
    return first && mem::read<uintptr_t>(first + offsets::g.Actor_RootComponent);
}

// --- resolution automatique ------------------------------------------------

// Signature classique du global GWorld : mov rbx, [rip + disp32].
uintptr_t resolve_world() {
    static uintptr_t cached = 0;
    if (cached && world_ok(cached, offsets::g.World_Levels)) {
        return cached;
    }

    const uintptr_t match = mem::scan("48 8B 1D ?? ?? ?? ?? 48 85 DB 74 3B 41");
    if (match) {
        const int32_t displacement = mem::read<int32_t>(match + 3);
        const uintptr_t target = match + 7 + displacement;
        const uintptr_t world = mem::read<uintptr_t>(target);
        if (world && world_ok(world, offsets::g.World_Levels)) {
            cached = world;
            return world;
        }
    }

    if (offsets::g.GWorld) {
        const uintptr_t world = mem::read<uintptr_t>(mem::g_base + offsets::g.GWorld);
        if (world && world_ok(world, offsets::g.World_Levels)) {
            cached = world;
            return world;
        }
    }

    return 0;
}

uintptr_t resolve_levels_offset(uintptr_t world) {
    static const uintptr_t kCandidates[] = {0x148, 0x138, 0x1C0, 0x1B8, 0x1D8, 0x190};
    for (const uintptr_t offset : kCandidates) {
        if (world_ok(world, offset)) {
            return offset;
        }
    }
    return 0;
}

uintptr_t resolve_pawn() {
    const uintptr_t world = resolve_world();
    if (!world) {
        return 0;
    }

    const uintptr_t gameInstance =
        mem::read<uintptr_t>(world + offsets::g.World_OwningGameInstance);
    if (!gameInstance) {
        return 0;
    }

    const uintptr_t localPlayers =
        mem::read<uintptr_t>(gameInstance + offsets::g.GameInstance_LocalPlayers);
    if (!localPlayers) {
        return 0;
    }

    const uintptr_t localPlayer = mem::read<uintptr_t>(localPlayers);
    if (!localPlayer) {
        return 0;
    }

    const uintptr_t controller =
        mem::read<uintptr_t>(localPlayer + offsets::g.LocalPlayer_PlayerController);
    if (!controller) {
        return 0;
    }

    static const uintptr_t kPawnCandidates[] = {0x298, 0x2A0, 0x2A8, 0x2B0};
    for (const uintptr_t offset : kPawnCandidates) {
        const uintptr_t pawn = mem::read<uintptr_t>(controller + offset);
        if (!pawn) {
            continue;
        }
        const uintptr_t mesh = mem::read<uintptr_t>(pawn + offsets::g.Pawn_Mesh);
        if (mesh && mem::read<uintptr_t>(mesh + 0x10)) {
            return pawn;
        }
    }

    return controller ? mem::read<uintptr_t>(controller + offsets::g.PlayerController_AcknowledgedPawn)
                      : 0;
}

// La caméra : on balaie le PlayerCameraManager et on retient le POV dont la
// position colle au pawn local et dont le FOV est plausible.
uintptr_t resolve_pov(uintptr_t manager, const Vec3& pawnLocation) {
    uintptr_t best = 0;

    for (uintptr_t offset = 0x600; offset < 0x4000; offset += 4) {
        const Vec3 location = mem::read<Vec3>(manager + offset);
        const Rot rotation = mem::read<Rot>(manager + offset + 0xC);
        const float fov = mem::read<float>(manager + offset + 0x18);

        if (!sane_float(location.x) || !sane_float(location.y) || !sane_float(location.z)) {
            continue;
        }
        if (fov < 40.0f || fov > 140.0f) {
            continue;
        }
        if (rotation.pitch < -90.0f || rotation.pitch > 90.0f) {
            continue;
        }
        if (std::fabs(rotation.yaw) > 360.0f || std::fabs(rotation.roll) > 180.0f) {
            continue;
        }

        const float dx = location.x - pawnLocation.x;
        const float dy = location.y - pawnLocation.y;
        const float dz = location.z - pawnLocation.z;
        if (std::sqrt(dx * dx + dy * dy) > 250.0f || std::fabs(dz) > 400.0f) {
            continue;
        }

        if (location.x == 0.0f && location.y == 0.0f && location.z == 0.0f) {
            continue;
        }

        best = offset;
        break;
    }

    return best;
}

void read_camera(uintptr_t controller) {
    g_cameraValid = false;

    const uintptr_t manager =
        mem::read<uintptr_t>(controller + offsets::g.PlayerController_PlayerCameraManager);
    if (!manager) {
        return;
    }

    uintptr_t pov = offsets::g.CameraManager_POV;

    if (g_localPawn) {
        const uintptr_t root = mem::read<uintptr_t>(g_localPawn + offsets::g.Actor_RootComponent);
        if (root) {
            const Vec3 pawnLocation =
                mem::read<Vec3>(root + offsets::g.SceneComponent_RelativeLocation);
            const uintptr_t found = resolve_pov(manager, pawnLocation);
            if (found) {
                if (found != offsets::g.CameraManager_POV) {
                    offsets::g.CameraManager_POV = found;
                    std::wprintf(L"  [ok] camera auto : POV 0x%llX\n",
                                 static_cast<unsigned long long>(found));
                }
                pov = found;
            }
        }
    }

    g_cameraLocation = mem::read<Vec3>(manager + pov);
    g_cameraRotation = mem::read<Rot>(manager + pov + 0xC);
    g_cameraFov = mem::read<float>(manager + pov + 0x18);

    if (!sane_float(g_cameraLocation.x) || !sane_float(g_cameraLocation.y) ||
        !sane_float(g_cameraLocation.z)) {
        return;
    }
    if (g_cameraFov < 30.0f || g_cameraFov > 150.0f) {
        g_cameraFov = 90.0f;
    }

    g_cameraValid = true;
    diag::cameraOk = true;
}

// --- cache des joueurs -----------------------------------------------------

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

    for (int32_t index = 0; index < levelCount; ++index) {
        const uintptr_t level = mem::read<uintptr_t>(levelData + index * 8);
        if (!level) {
            continue;
        }

        const uintptr_t actors = mem::read<uintptr_t>(level + offsets::g.Level_Actors);
        const int32_t actorCount = mem::read<int32_t>(level + offsets::g.Level_Actors + 8);
        if (!actors || actorCount <= 0 || actorCount > 40000) {
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
            }
            g_cache.push_back(cache);
        }
    }

    g_cacheTime = GetTickCount64();
}

void update() {
    g_players.clear();
    g_cameraValid = false;

    if (GetTickCount64() - g_cacheTime > 1500 || g_cache.empty()) {
        refresh_cache();
    }

    g_localPawn = resolve_pawn();

    const uintptr_t world = resolve_world();
    if (world) {
        const uintptr_t gameInstance =
            mem::read<uintptr_t>(world + offsets::g.World_OwningGameInstance);
        const uintptr_t localPlayers =
            gameInstance ? mem::read<uintptr_t>(gameInstance + offsets::g.GameInstance_LocalPlayers)
                         : 0;
        const uintptr_t localPlayer = localPlayers ? mem::read<uintptr_t>(localPlayers) : 0;
        const uintptr_t controller =
            localPlayer
                ? mem::read<uintptr_t>(localPlayer + offsets::g.LocalPlayer_PlayerController)
                : 0;

        if (controller) {
            g_controller = controller;
            read_camera(controller);
        }
    }

    if (g_localPawn) {
        const uintptr_t state = mem::read<uintptr_t>(g_localPawn + offsets::g.Pawn_PlayerState);
        g_localTeam = state
                          ? static_cast<int>(mem::read<uint8_t>(state + offsets::g.PlayerState_TeamIndex))
                          : 0;
    }

    for (const Cache& cache : g_cache) {
        const uintptr_t root = mem::read<uintptr_t>(cache.pawn + offsets::g.Actor_RootComponent);
        if (!root) {
            continue;
        }

        Player player;
        player.pawn = cache.pawn;
        player.name = cache.name;
        player.location = mem::read<Vec3>(root + offsets::g.SceneComponent_RelativeLocation);

        if (!sane_float(player.location.x) || !sane_float(player.location.y) ||
            !sane_float(player.location.z)) {
            continue;
        }
        if (std::fabs(player.location.x) < 1.0f && std::fabs(player.location.y) < 1.0f &&
            std::fabs(player.location.z) < 1.0f) {
            continue;
        }

        player.team = static_cast<int>(
            mem::read<uint8_t>(cache.playerState + offsets::g.PlayerState_TeamIndex));
        player.dbno = offsets::g.Pawn_IsDBNO
                          ? (mem::read<uint8_t>(cache.pawn + offsets::g.Pawn_IsDBNO) & 0x01) != 0
                          : false;
        player.local = cache.pawn == g_localPawn;

        const float dx = player.location.x - g_cameraLocation.x;
        const float dy = player.location.y - g_cameraLocation.y;
        const float dz = player.location.z - g_cameraLocation.z;
        player.distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f;

        g_players.push_back(player);
    }

    std::sort(g_players.begin(), g_players.end(),
              [](const Player& a, const Player& b) { return a.distance < b.distance; });
}

bool world_to_screen(const Vec3& world, Vec2& out) {
    if (!g_cameraValid || g_screenWidth <= 0 || g_screenHeight <= 0) {
        return false;
    }

    constexpr float pi = 3.14159265358979323846f;
    const float pitch = g_cameraRotation.pitch * pi / 180.0f;
    const float yaw = g_cameraRotation.yaw * pi / 180.0f;

    const Vec3 forward{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                       std::sin(pitch)};
    const Vec3 right{-std::sin(yaw), std::cos(yaw), 0.0f};
    const Vec3 up{-std::cos(yaw) * std::sin(pitch), -std::sin(yaw) * std::sin(pitch),
                  std::cos(pitch)};

    const Vec3 delta{world.x - g_cameraLocation.x, world.y - g_cameraLocation.y,
                     world.z - g_cameraLocation.z};

    const float depth = delta.x * forward.x + delta.y * forward.y + delta.z * forward.z;
    if (depth < 1.0f) {
        return false;
    }

    const float horizontal = delta.x * right.x + delta.y * right.y;
    const float vertical = delta.x * up.x + delta.y * up.y + delta.z * up.z;

    // UE4 : FMinimalViewInfo::FOV est le FOV horizontal.
    const float tanHalfX = std::tan(g_cameraFov * pi / 360.0f);
    const float aspect = static_cast<float>(g_screenWidth) / static_cast<float>(g_screenHeight);
    const float tanHalfY = tanHalfX / aspect;

    out.x = static_cast<float>(g_screenWidth / 2.0f * (1.0f + (horizontal / depth) / tanHalfX));
    out.y = static_cast<float>(g_screenHeight / 2.0f * (1.0f - (vertical / depth) / tanHalfY));
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
// Overlay
// ---------------------------------------------------------------------------

namespace overlay {

HWND g_window = nullptr;
HDC g_dc = nullptr;
HBITMAP g_bitmap = nullptr;
uint32_t* g_pixels = nullptr;
int g_width = 0;
int g_height = 0;
HFONT g_font = nullptr;
HFONT g_fontSmall = nullptr;

bool g_visible = false;
bool g_anchored = false;
bool g_noGame = false;

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
    g_fontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
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
                g_noGame = false;
                g_anchored = true;
                return true;
            }
        }
    }

    constexpr int kBoxWidth = 372;
    constexpr int kBoxHeight = 452;
    if (resize(kBoxWidth, kBoxHeight)) {
        place(20, 20, kBoxWidth, kBoxHeight);
        sdk::g_screenWidth = kBoxWidth;
        sdk::g_screenHeight = kBoxHeight;
        g_noGame = true;
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

// Ecriture directe avec alpha (le canal alpha est preserve par present()).
void blend(int x, int y, int width, int height, uint32_t color, uint8_t alpha) {
    if (!g_pixels) {
        return;
    }

    const int x0 = (std::max)(0, x);
    const int y0 = (std::max)(0, y);
    const int x1 = (std::min)(g_width, x + width);
    const int y1 = (std::min)(g_height, y + height);

    const uint32_t r = (color >> 16) & 0xFF;
    const uint32_t g = (color >> 8) & 0xFF;
    const uint32_t b = color & 0xFF;

    for (int py = y0; py < y1; ++py) {
        uint32_t* row = g_pixels + static_cast<size_t>(py) * g_width;
        for (int px = x0; px < x1; ++px) {
            const uint32_t dst = row[px];
            const uint32_t dr = (dst >> 16) & 0xFF;
            const uint32_t dg = (dst >> 8) & 0xFF;
            const uint32_t db = dst & 0xFF;

            const uint32_t nr = (r * alpha + dr * (255 - alpha)) / 255;
            const uint32_t ng = (g * alpha + dg * (255 - alpha)) / 255;
            const uint32_t nb = (b * alpha + db * (255 - alpha)) / 255;

            row[px] = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
        }
    }
}

void present() {
    if (!g_pixels || !g_anchored) {
        return;
    }

    const size_t count = static_cast<size_t>(g_width) * g_height;
    for (size_t index = 0; index < count; ++index) {
        const uint32_t pixel = g_pixels[index];
        if ((pixel >> 24) != 0) {
            continue;   // ecrit par blend(), l'alpha est voulu
        }
        const uint32_t rgb = pixel & 0x00FFFFFFu;
        g_pixels[index] = rgb ? (rgb | 0xFF000000u) : 0u;
    }

    RECT rect{};
    GetWindowRect(g_window, &rect);

    POINT source{0, 0};
    POINT destination{rect.left, rect.top};
    SIZE size{g_width, g_height};
    BLENDFUNCTION blendFunction{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g_window, screen, &destination, &size, g_dc, &source, 0, &blendFunction,
                        ULW_ALPHA);
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

void text(int x, int y, const std::wstring& value, uint32_t color, bool small = false) {
    if (value.empty()) {
        return;
    }

    SelectObject(g_dc, small ? g_fontSmall : g_font);

    SIZE size{};
    GetTextExtentPoint32W(g_dc, value.c_str(), static_cast<int>(value.size()), &size);

    SetTextColor(g_dc, to_gdi(0x05070A));
    TextOutW(g_dc, x - 1, y, value.c_str(), static_cast<int>(value.size()));
    TextOutW(g_dc, x + 1, y, value.c_str(), static_cast<int>(value.size()));
    TextOutW(g_dc, x, y - 1, value.c_str(), static_cast<int>(value.size()));
    TextOutW(g_dc, x, y + 1, value.c_str(), static_cast<int>(value.size()));

    SetTextColor(g_dc, to_gdi(color));
    TextOutW(g_dc, x, y, value.c_str(), static_cast<int>(value.size()));
}

void text_right(int right, int y, const std::wstring& value, uint32_t color, bool small = false) {
    if (value.empty()) {
        return;
    }

    SelectObject(g_dc, small ? g_fontSmall : g_font);

    SIZE size{};
    GetTextExtentPoint32W(g_dc, value.c_str(), static_cast<int>(value.size()), &size);
    text(right - size.cx, y, value, color, small);
}

void text_center(int centerX, int y, const std::wstring& value, uint32_t color, bool small = false) {
    if (value.empty()) {
        return;
    }

    SelectObject(g_dc, small ? g_fontSmall : g_font);

    SIZE size{};
    GetTextExtentPoint32W(g_dc, value.c_str(), static_cast<int>(value.size()), &size);
    text(centerX - size.cx / 2, y, value, color, small);
}

void circle(int centerX, int centerY, int radius, uint32_t color) {
    HPEN pen = CreatePen(PS_SOLID, 1, to_gdi(color));
    HGDIOBJ oldPen = SelectObject(g_dc, pen);
    HGDIOBJ oldBrush = SelectObject(g_dc, GetStockObject(NULL_BRUSH));

    constexpr int kSegments = 64;
    MoveToEx(g_dc, centerX + radius, centerY, nullptr);
    for (int index = 1; index <= kSegments; ++index) {
        constexpr float pi = 3.14159265358979323846f;
        const float angle = index * 2.0f * pi / kSegments;
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

constexpr int kPanelWidth = 330;
constexpr int kPanelHeight = 412;
constexpr int kRowHeight = 22;

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
        constexpr float pi = 3.14159265358979323846f;
        const float dx = aim.x - sdk::g_cameraLocation.x;
        const float dy = aim.y - sdk::g_cameraLocation.y;
        const float dz = aim.z - sdk::g_cameraLocation.z;
        const float horizontal = std::sqrt(dx * dx + dy * dy);

        sdk::Rot rotation;
        rotation.pitch = std::atan2(dz, horizontal) * 180.0f / pi;
        rotation.yaw = std::atan2(dy, dx) * 180.0f / pi;
        rotation.roll = 0.0f;

        mem::write(sdk::g_controller + offsets::g.PlayerController_ControlRotation, rotation);
    }

    sdk::Vec2 screen{};
    if (sdk::world_to_screen(aim, screen)) {
        input::move_mouse(screen.x - sdk::g_screenWidth / 2.0f,
                          screen.y - sdk::g_screenHeight / 2.0f);
    }
}

void esp() {
    const int centerX = sdk::g_screenWidth / 2;

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
        if (height < 4.0f || height > 2000.0f) {
            continue;
        }

        const float width = height * 0.55f;
        const int left = static_cast<int>(std::lround(head.x - width / 2.0f));
        const int topY = static_cast<int>(std::lround(head.y));
        const int boxWidth = static_cast<int>(std::lround(width));
        const int boxHeight = static_cast<int>(std::lround(height));

        uint32_t color = cfg::kEnemy;
        if (sdk::g_localTeam != 0 && player.team == sdk::g_localTeam) {
            color = cfg::kTeam;
        }
        if (player.dbno) {
            color = cfg::kBot;
        }

        if (cfg::g.snapline) {
            overlay::filled(centerX, sdk::g_screenHeight - 2, 1, 2, color);
            HGDIOBJ oldBrush = SelectObject(overlay::g_dc, GetStockObject(NULL_BRUSH));
            HPEN pen = CreatePen(PS_SOLID, 1, overlay::to_gdi(color));
            HGDIOBJ oldPen = SelectObject(overlay::g_dc, pen);
            MoveToEx(overlay::g_dc, centerX, sdk::g_screenHeight, nullptr);
            LineTo(overlay::g_dc, left + boxWidth / 2, topY + boxHeight);
            SelectObject(overlay::g_dc, oldPen);
            DeleteObject(pen);
            SelectObject(overlay::g_dc, oldBrush);
        }

        if (cfg::g.box) {
            overlay::rect(left, topY, boxWidth, boxHeight, color, 1);
        }

        if (cfg::g.name && !player.name.empty()) {
            overlay::text_center(left + boxWidth / 2, topY - 17, player.name, color);
        }

        if (cfg::g.distance) {
            wchar_t buffer[48]{};
            swprintf_s(buffer, L"%.0f m", player.distance);
            overlay::text_center(left + boxWidth / 2, topY + boxHeight + 2, buffer, cfg::kText, true);
        }

        if (cfg::g.health && cfg::g.box) {
            overlay::filled(left - 6, topY - 1, 3, boxHeight + 1, 0x101418);
            overlay::filled(left - 6, topY, 3, boxHeight, 0x35E07A);
        }

        if (cfg::g.crosshair) {
            sdk::Vec2 aimScreen{};
            if (sdk::world_to_screen(sdk::aim_point(player), aimScreen)) {
                overlay::filled(static_cast<int>(aimScreen.x) - 2, static_cast<int>(aimScreen.y) - 2,
                                4, 4, color);
            }
        }
    }
}

void crosshair() {
    const int centerX = sdk::g_screenWidth / 2;
    const int centerY = sdk::g_screenHeight / 2;

    overlay::circle(centerX, centerY, static_cast<int>(cfg::kAimbotFov), 0x2A3442);
    overlay::filled(centerX - 1, centerY - 6, 2, 4, cfg::kAccent);
    overlay::filled(centerX - 1, centerY + 3, 2, 4, cfg::kAccent);
    overlay::filled(centerX - 6, centerY - 1, 4, 2, cfg::kAccent);
    overlay::filled(centerX + 3, centerY - 1, 4, 2, cfg::kAccent);
}

void row(int y, const wchar_t* key, const wchar_t* label, bool on) {
    overlay::text(14, y, key, cfg::kKey, true);
    overlay::text(66, y, label, cfg::kText, true);
    overlay::text_right(kPanelWidth - 14, y, on ? L"ON" : L"OFF", on ? cfg::kOn : cfg::kOff, true);
}

void section(int y, const wchar_t* label) {
    overlay::text(14, y, label, cfg::kAccent, true);
    overlay::filled(14, y + 16, kPanelWidth - 28, 1, 0x1E2836);
}

void panel(int fps) {
    const int height = kPanelHeight;

    overlay::blend(0, 0, kPanelWidth, height, cfg::kPanelBack, 235);
    overlay::blend(0, 0, kPanelWidth, 30, cfg::kPanelHead, 250);
    overlay::rect(0, 0, kPanelWidth - 1, height - 1, 0x2A3646, 1);
    overlay::filled(0, 0, 3, height, cfg::kAccent);

    overlay::text(14, 8, L"RETRAC OGFN", cfg::kText, false);
    overlay::text_right(kPanelWidth - 14, 9, L"14.60  [INS]", cfg::kDim, true);

    int y = 40;

    section(y, L"AIM");
    y += 24;
    row(y, L"[clic droit]", L"Aim assist", true);
    y += kRowHeight;
    row(y, L"[Y]", L"Bullet TP", true);
    y += kRowHeight;
    {
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"FOV %.0f px   smooth %.1f", cfg::kAimbotFov, cfg::kAimbotSmooth);
        overlay::text(14, y, buffer, cfg::kDim, true);
    }
    y += 30;

    section(y, L"VISUELS");
    y += 24;
    row(y, L"[F1]", L"Boites", cfg::g.box);
    y += kRowHeight;
    row(y, L"[F2]", L"Noms", cfg::g.name);
    y += kRowHeight;
    row(y, L"[F3]", L"Distance", cfg::g.distance);
    y += kRowHeight;
    row(y, L"[F4]", L"Barre de vie", cfg::g.health);
    y += kRowHeight;
    row(y, L"[F5]", L"Lignes", cfg::g.snapline);
    y += kRowHeight;
    row(y, L"[F6]", L"Crosshair + FOV", cfg::g.crosshair);
    y += 30;

    section(y, L"ETAT");
    y += 24;
    {
        wchar_t buffer[96]{};
        swprintf_s(buffer, L"monde %s   camera %s", diag::worldOk ? L"OK" : L"--",
                   diag::cameraOk ? L"OK" : L"--");
        overlay::text(14, y, buffer, diag::worldOk ? cfg::kOn : cfg::kOff, true);
        y += kRowHeight;

        swprintf_s(buffer, L"joueurs %d   %d fps", static_cast<int>(sdk::g_players.size()), fps);
        overlay::text(14, y, buffer, cfg::kDim, true);
        y += kRowHeight;

        overlay::text(14, y, L"F8 quitter", cfg::kDim, true);
    }

    if (overlay::g_noGame) {
        overlay::text_center(kPanelWidth / 2, height - 20,
                             L"fenetre de jeu absente - fenetre sans bordure", cfg::kBot, true);
    }
}

void status_compact() {
    overlay::text(12, 10, L"Retrac OGFN - external", cfg::kText, true);
    overlay::text(12, 28, L"INS : panel   F8 : quitter", cfg::kDim, true);
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
    freopen_s(&dummy, "CONIN$", "r", stdin);

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Retrac OGFN - external");
}

void wait_exit() {
    log(L"\n  Entree pour quitter...");
    getchar();
}

void handle_keys(bool& running) {
    if (input::pressed(cfg::kPanelKey)) {
        cfg::g.panel = !cfg::g.panel;
    }
    if (input::pressed(cfg::kKeyBox)) {
        cfg::g.box = !cfg::g.box;
    }
    if (input::pressed(cfg::kKeyName)) {
        cfg::g.name = !cfg::g.name;
    }
    if (input::pressed(cfg::kKeyDistance)) {
        cfg::g.distance = !cfg::g.distance;
    }
    if (input::pressed(cfg::kKeyHealth)) {
        cfg::g.health = !cfg::g.health;
    }
    if (input::pressed(cfg::kKeySnapline)) {
        cfg::g.snapline = !cfg::g.snapline;
    }
    if (input::pressed(cfg::kKeyCrosshair)) {
        cfg::g.crosshair = !cfg::g.crosshair;
    }
    if (input::pressed(cfg::kQuitKey)) {
        running = false;
    }
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

    log(L"\n  Retrac OGFN - external   (cible : Fortnite 14.60)\n");
    log(L"  --------------------------------------------\n");

    if (mem::enable_debug_privilege()) {
        log(L"  [ok] SeDebugPrivilege\n");
    } else {
        log(L"  [avert] SeDebugPrivilege indisponible (lance en administrateur)\n");
    }

    if (!offsets::load()) {
        log(L"  [erreur] offsets indisponibles\n");
        wait_exit();
        return 1;
    }

    if (!overlay::create(instance)) {
        log(L"  [erreur] overlay impossible\n");
        wait_exit();
        return 1;
    }

    log(L"  [info] recherche de Fortnite...\n");

    bool running = true;
    bool needAttach = true;
    DWORD pid = 0;
    int frames = 0;
    int fps = 0;
    ULONGLONG fpsTime = GetTickCount64();
    ULONGLONG windowRetry = 0;
    ULONGLONG attachRetry = 0;

    while (running) {
        if (needAttach && GetTickCount64() - attachRetry > 1000) {
            attachRetry = GetTickCount64();
            pid = mem::find_process();

            if (pid && mem::attach(pid)) {
                diag::attached = true;
                needAttach = false;
                windowRetry = 0;
                log(L"  [ok] attache : process %lu, base 0x%llX\n", pid,
                    static_cast<unsigned long long>(mem::g_base));
                log(L"       Inser = panel, F8 = quitter\n");
            }
        }

        if (!needAttach && !mem::alive()) {
            log(L"  [info] process termine, nouvelle recherche...\n");
            mem::detach();
            diag::attached = false;
            diag::worldOk = false;
            diag::cameraOk = false;
            needAttach = true;
            attachRetry = GetTickCount64();
        }

        if (!mem::g_window && GetTickCount64() - windowRetry > 1000) {
            windowRetry = GetTickCount64();
            if (pid) {
                mem::g_window = mem::find_window(pid);
            }
        }

        handle_keys(running);

        overlay::sync();
        overlay::clear();

        if (diag::attached && mem::alive()) {
            sdk::update();
            features::esp();
            if (cfg::g.crosshair) {
                features::crosshair();
            }
            features::aimbot(input::down(cfg::kAimbotKey));
            features::bullet_tp(input::down(cfg::kBulletTpKey));
        }

        if (cfg::g.panel) {
            features::panel(fps);
        } else if (!overlay::g_noGame) {
            features::status_compact();
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

    mem::detach();
    if (overlay::g_window) {
        DestroyWindow(overlay::g_window);
    }

    wait_exit();
    return 0;
}
