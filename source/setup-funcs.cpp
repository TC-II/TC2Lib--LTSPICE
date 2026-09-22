#include "setup-funcs.h"

#include "ascii-art.h"
#include "patcher.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#if OS_Windows
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fsys = std::filesystem;

// ---------------------------------------------------------------------------
// Rutas
//
// Todo se resuelve una sola vez en initLib(). Los recursos se buscan junto al
// ejecutable y no en el directorio actual: el patcher se suele abrir desde un
// acceso directo o con "ejecutar como administrador", y ahi el directorio
// actual no es el del programa.
// ---------------------------------------------------------------------------

namespace
{

fsys::path resourcesDirectory;
fsys::path examplesDirectory;
fsys::path symbolsDirectory;
fsys::path subcircuitsDirectory;
fsys::path ltspiceDirectory;
fsys::path iniFile;
fsys::path backgroundFile;

ma_engine engine;
bool audioReady = false;
float volume = 0.2f;

fsys::path executableDirectory()
{
    std::error_code code;
#if OS_Windows
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return fsys::current_path(code);
        }
        if (length < buffer.size())
        {
            return fsys::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    const fsys::path self = fsys::read_symlink("/proc/self/exe", code);
    if (!code)
    {
        return self.parent_path();
    }
    return fsys::current_path(code);
#endif
}

#if OS_Windows
/// Variable de entorno como ruta nativa. Se usa la version ancha para no perder
/// caracteres si el nombre de usuario no es ASCII.
fsys::path environmentPath(const wchar_t *name)
{
    const wchar_t *value = _wgetenv(name);
    if (value == nullptr || value[0] == L'\0')
    {
        return fsys::path();
    }
    return fsys::path(value);
}
#endif

/// Mientras vive, la consola de Windows queda en modo UTF-16, que es lo unico
/// que dibuja bien el arte ASCII. Hay que volver a modo texto antes de escribir
/// con std::cout, asi que conviene que el cambio se deshaga solo.
class WideConsole
{
public:
    WideConsole()
    {
#if OS_Windows
        _setmode(_fileno(stdout), _O_WTEXT);
#endif
    }

    ~WideConsole()
    {
#if OS_Windows
        _setmode(_fileno(stdout), _O_TEXT);
#endif
    }

    WideConsole(const WideConsole &) = delete;
    WideConsole &operator=(const WideConsole &) = delete;
};

void waitForEnter()
{
    std::string ignored;
    std::getline(std::cin, ignored);
}

/// true si se puede crear un archivo en `directory`. Lo comprueba creando uno,
/// que es la unica forma confiable en Windows: los permisos efectivos dependen
/// de la ACL, de la herencia y de la virtualizacion de carpetas.
bool isWritable(const fsys::path &directory)
{
    const fsys::path probe = directory / "tclib-write-test.tmp";
    std::ofstream probeStream(probe);
    const bool writable = probeStream.is_open();
    probeStream.close();

    std::error_code ignored;
    fsys::remove(probe, ignored);
    return writable;
}

// ---------------------------------------------------------------------------
// Operaciones sobre archivos
// ---------------------------------------------------------------------------

/// Aplica `edit` sobre LTspice.ini y lo reescribe solo si hubo cambios.
using IniEdit = bool (*)(std::vector<std::string> &);

std::string editIniFile(IniEdit edit, const char *successMessage)
{
    patcher::TextFile file;
    std::string error;
    if (!patcher::readTextFile(iniFile.string(), file, error))
    {
        return "Error: LTspice.ini not found. Run LTspice once first.";
    }
    if (!edit(file.lines))
    {
        return std::string(successMessage) + " (already applied)";
    }
    if (!patcher::writeTextFileSafely(iniFile.string(), file, error))
    {
        return "Error: " + error;
    }
    return successMessage;
}

/// Reemplaza la seccion [Colors] del LTspice.ini por el contenido del tema.
std::string applyTheme(const char *themeFileName, const char *successMessage)
{
    patcher::TextFile theme;
    std::string error;
    if (!patcher::readTextFile((resourcesDirectory / themeFileName).string(), theme, error))
    {
        return "Error: " + error;
    }

    patcher::TextFile file;
    if (!patcher::readTextFile(iniFile.string(), file, error))
    {
        return "Error: LTspice.ini not found. Run LTspice once first.";
    }
    if (!patcher::replaceIniSection(file.lines, "Colors", theme.lines))
    {
        return std::string(successMessage) + " (already applied)";
    }
    if (!patcher::writeTextFileSafely(iniFile.string(), file, error))
    {
        return "Error: " + error;
    }
    return successMessage;
}

/// Inserta los modelos de `referenceFileName` en un standard.* de LTspice.
bool addComponents(const char *referenceFileName, const char *configFileName, std::string &error)
{
    patcher::TextFile reference;
    if (!patcher::readTextFile((resourcesDirectory / referenceFileName).string(), reference, error))
    {
        return false;
    }

    const fsys::path configFile = ltspiceDirectory / "lib" / "cmp" / configFileName;
    patcher::TextFile config;
    if (!patcher::readTextFile(configFile.string(), config, error))
    {
        return false;
    }

    patcher::mergeCustomComponents(reference.lines, config.lines);
    return patcher::writeTextFileSafely(configFile.string(), config, error);
}

bool copyFolder(const fsys::path &source, const fsys::path &destination, std::string &error)
{
    std::error_code code;
    fsys::create_directories(destination, code);
    if (code)
    {
        error = "no se pudo crear " + destination.string() + ": " + code.message();
        return false;
    }

    for (const auto &entry : fsys::directory_iterator(source, code))
    {
        const fsys::path &sourcePath = entry.path();
        const fsys::path destinationPath = destination / sourcePath.filename();

        if (fsys::is_directory(sourcePath, code))
        {
            if (!copyFolder(sourcePath, destinationPath, error))
            {
                return false;
            }
            continue;
        }

        if (!patcher::copyFileOverwriting(sourcePath.string(), destinationPath.string(), error))
        {
            return false;
        }
    }
    if (code)
    {
        error = "no se pudo leer " + source.string() + ": " + code.message();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Acciones del menu
// ---------------------------------------------------------------------------

bool editPenWidth(std::vector<std::string> &lines)
{
    return patcher::setIniKey(lines, "Options", "PenWidth", "2");
}

bool editBackground(std::vector<std::string> &lines)
{
    return patcher::setIniKey(lines, "Options", "MDIbackgroundImage", "3");
}

bool editShortcuts(std::vector<std::string> &lines)
{
    const bool lineChanged = patcher::setIniKey(lines, "SchKeyBoardShortCut", "Draw_Lines", "J");
    const bool rectChanged =
        patcher::setIniKey(lines, "SchKeyBoardShortCut", "Draw_Rectangles", "K");
    return lineChanged || rectChanged;
}

std::string setDarkTheme()
{
    return applyTheme("dark-theme.ini", "Dark theme applied");
}

std::string setLightTheme()
{
    return applyTheme("light-theme.ini", "White theme applied");
}

std::string setPenWidth()
{
    return editIniFile(editPenWidth, "Pen width fix applied");
}

std::string setShortcuts()
{
    return editIniFile(editShortcuts, "Custom shortcuts applied");
}

std::string loadCustomComponents()
{
    std::string error;
    if (!copyFolder(subcircuitsDirectory, ltspiceDirectory / "lib" / "sub", error) ||
        !copyFolder(symbolsDirectory, ltspiceDirectory / "lib" / "sym", error) ||
        !copyFolder(examplesDirectory, ltspiceDirectory / "examples", error))
    {
        return "Error: " + error;
    }

    static const std::array<std::pair<const char *, const char *>, 4> componentFiles = {{
        {"bjt.ini", "standard.bjt"},
        {"dio.ini", "standard.dio"},
        {"jft.ini", "standard.jft"},
        {"mos.ini", "standard.mos"},
    }};

    for (const auto &files : componentFiles)
    {
        if (!addComponents(files.first, files.second, error))
        {
            return "Error: " + error;
        }
    }
    return "Custom components and examples loaded";
}

std::string loadCustomBackground()
{
    std::string error;
    if (!patcher::copyFileOverwriting((resourcesDirectory / "LTspice.jpg").string(),
                                      backgroundFile.string(), error))
    {
        return "Error: " + error;
    }
    return editIniFile(editBackground, "Custom background applied");
}

std::string doTheThing()
{
    const std::string results[] = {setDarkTheme(), loadCustomComponents(), loadCustomBackground(),
                                   setPenWidth()};
    for (const std::string &result : results)
    {
        if (result.compare(0, 6, "Error:") == 0)
        {
            return result;
        }
    }
    return "All patches applied";
}

std::string printCredits()
{
    clearScreen();
#if OS_Windows
    {
        WideConsole wide;
        std::wcout << L"\033[34m" << art::kCreditsWide;
        std::wcout << L"\033[33m Credits:\n\n";
        std::wcout << L"\033[31m TC-Lib: Agustín Gullino, Javier Petrucci\n\n";
        std::wcout << L"\033[32m Patcher: Agustín Fisher, Agustín Gullino, Javier Petrucci\033[0m";
    }
    waitForEnter();
#else
    std::cout << art::kLogoNarrow;
    std::cout << "\n  TC-Lib: Agustín Gullino, Javier Petrucci\n";
    std::cout << "  Patcher: Agustín Fisher, Agustín Gullino, Javier Petrucci\n";
    char ignored;
    if (read(STDIN_FILENO, &ignored, 1) != 1)
    {
        return "";
    }
#endif
    return "";
}

std::string volumeUp()
{
    if (!audioReady)
    {
        return "No audio device available";
    }
    volume *= 1.4f;
    volume = volume > 1.0f ? 1.0f : volume;
    ma_engine_set_volume(&engine, volume);
    return "";
}

std::string volumeDown()
{
    if (!audioReady)
    {
        return "No audio device available";
    }
    volume *= 0.6f;
    ma_engine_set_volume(&engine, volume);
    return "";
}

std::string exitProgram()
{
    return "Exiting program. Goodbye!";
}

/// Tabla del menu: agregar una opcion es agregar una fila. El recorrido y el
/// despacho salen de aca, asi que no hay una longitud que mantener aparte.
struct MenuEntry
{
    const char *label;
    std::string (*action)();
};

const std::array<MenuEntry, 11> menu = {{
    {"Apply default patches", doTheThing},
    {"Apply Dark Theme", setDarkTheme},
    {"Apply White Theme", setLightTheme},
    {"Load custom components and examples", loadCustomComponents},
    {"Load custom background", loadCustomBackground},
    {"Adjust line width", setPenWidth},
    {"Apply custom shortcuts", setShortcuts},
    {"Credits", printCredits},
    {"Volume +", volumeUp},
    {"Volume -", volumeDown},
    {"Exit", exitProgram},
}};

} // namespace

// ---------------------------------------------------------------------------
// Interfaz publica
// ---------------------------------------------------------------------------

std::size_t menuLength()
{
    return menu.size();
}

bool runMenuOption(std::size_t selected, std::string &lastOperation)
{
    if (selected >= menu.size())
    {
        lastOperation = "Select a valid option";
        return true;
    }

    std::string result;
    try
    {
        result = menu[selected].action();
    }
    catch (const std::exception &e)
    {
        result = std::string("Error: ") + e.what();
    }

    if (!result.empty())
    {
        lastOperation = result;
    }
    return menu[selected].action != exitProgram;
}

void printMenu(std::size_t selected, const std::string &lastOperation)
{
#if OS_Windows
    {
        WideConsole wide;
        std::wcout << L"\033[36m\n\n" << art::kLogoWide;
        std::wcout << L"          Agustín Gullino, Javier Petrucci\n\n \033[0m";
    }
#else
    std::cout << art::kLogoNarrow;
    std::cout << "          Agustín Gullino, Javier Petrucci\n\n \033[0m";
#endif

    std::cout << "\n\n";
    std::cout << "    Use arrows to navigate.";
    std::cout << "\n\n";
    for (std::size_t i = 0; i < menu.size(); ++i)
    {
        if (i == selected)
        {
            std::cout << "\033[31m -> " << menu[i].label << "\n\033[0m";
        }
        else
        {
            std::cout << "    " << menu[i].label << "\n";
        }
    }

    std::cout << "\n\n    \033[32m->" << lastOperation << "<-\033[0m \n\n";
}

void clearScreen()
{
#if OS_Windows
    std::system("cls");
#else
    // Assume POSIX
    std::system("clear");
#endif
}

bool hasResources()
{
    static const char *const required[] = {"resources/dark-theme.ini", "resources/light-theme.ini",
                                           "resources/LTspice.jpg",    "resources/bjt.ini",
                                           "resources/dio.ini",        "resources/jft.ini",
                                           "resources/mos.ini",        "resources/Examples",
                                           "sym",                      "sub"};

    const fsys::path base = executableDirectory();
    for (const char *entry : required)
    {
        if (!fsys::exists(base / entry))
        {
            std::cout << "  Falta " << (base / entry).string() << std::endl;
            return false;
        }
    }
    return true;
}

void startMusic()
{
    if (ma_engine_init(nullptr, &engine) != MA_SUCCESS)
    {
        return;
    }
    audioReady = true;
    ma_engine_set_volume(&engine, volume);
    ma_engine_play_sound(&engine, (resourcesDirectory / "winrar_music.mp3").string().c_str(),
                         nullptr);
}

void stopMusic()
{
    if (audioReady)
    {
        ma_engine_uninit(&engine);
        audioReady = false;
    }
}

bool initLib()
{
    const fsys::path base = executableDirectory();
    resourcesDirectory = base / "resources";
    examplesDirectory = resourcesDirectory / "Examples";
    symbolsDirectory = base / "sym";
    subcircuitsDirectory = base / "sub";

#if OS_Windows
    fsys::path localAppData = environmentPath(L"LOCALAPPDATA");
    fsys::path roamingAppData = environmentPath(L"APPDATA");
    const fsys::path userProfile = environmentPath(L"USERPROFILE");

    if (userProfile.empty() && (localAppData.empty() || roamingAppData.empty()))
    {
        std::cout << "  ERROR: no se pudo determinar el perfil del usuario." << std::endl;
        return false;
    }
    if (localAppData.empty())
    {
        localAppData = userProfile / "AppData" / "Local";
    }
    if (roamingAppData.empty())
    {
        roamingAppData = userProfile / "AppData" / "Roaming";
    }

    ltspiceDirectory = localAppData / "LTspice";
    iniFile = roamingAppData / "LTspice.ini";
    backgroundFile = userProfile.empty() ? (localAppData / "LTspice.jpg")
                                         : (userProfile / "LTspice.jpg");
#else
    std::string wineUser;
    std::cout << "  Enter the path to the Wine user directory where LTspice is installed:";
    std::getline(std::cin, wineUser);
    std::cout << std::endl;

    if (wineUser.empty() || !fsys::exists(wineUser))
    {
        std::cerr << "  Error: Wine user directory does not exist." << std::endl;
        return false;
    }

    const fsys::path userProfile(wineUser);
    ltspiceDirectory = userProfile / "AppData" / "Local" / "LTspice";
    iniFile = userProfile / "AppData" / "Roaming" / "LTspice.ini";
    backgroundFile = userProfile / "LTspice.jpg";
#endif

    if (!fsys::exists(ltspiceDirectory))
    {
        std::cout << std::endl
                  << "  ERROR: no se encontro LTspice en " << ltspiceDirectory.string() << std::endl
                  << "  Instalalo y abrilo una vez antes de correr el patcher." << std::endl
                  << std::endl;
        return false;
    }

    // El patcher solo escribe en el perfil del usuario, asi que no hace falta
    // elevar privilegios; lo que si hace falta es poder escribir en los tres
    // lugares que toca. Se comprueban los tres antes de empezar, para avisar de
    // entrada en vez de fallar a la mitad del parche.
    for (const fsys::path &directory :
         {ltspiceDirectory, iniFile.parent_path(), backgroundFile.parent_path()})
    {
        if (isWritable(directory))
        {
            continue;
        }

#if OS_Windows
        {
            WideConsole wide;
            std::wcout << art::kLockedWide;
        }
#endif
        std::cout << std::endl
                  << "  ERROR: no hay permiso de escritura en " << directory.string() << std::endl
                  << "  Cerra LTspice y volve a intentar; si sigue, corre el patcher como"
                  << " administrador." << std::endl
                  << std::endl
                  << "  Press enter to exit..." << std::endl;
        waitForEnter();
        return false;
    }
    return true;
}
