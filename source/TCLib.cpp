// Requirement
// Install MSYS2 https://github.com/msys2/msys2-installer/releases/download/2024-01-13/msys2-x86_64-20240113.exe
// pacman -S --needed base-devel mingw-w64-ucrt-x86_64-toolchain
//-
//  How to compile?
//  g++ -static TCLib.cpp setup-funcs.cpp patcher.cpp -o ..\TCLib.exe
#include "setup-funcs.h"

#include <cstdlib>
#include <exception>

#if OS_Windows
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

void disableRawMode()
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode()
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
#endif

namespace
{

/// Mueve la seleccion una posicion, dando la vuelta en los extremos. La
/// longitud sale siempre de la tabla del menu, para que no se desfase.
std::size_t moveSelection(std::size_t selected, bool down)
{
    const std::size_t length = menuLength();
    return down ? (selected + 1) % length : (selected + length - 1) % length;
}

/// Espera a que el usuario elija: devuelve la opcion resaltada al apretar enter.
std::size_t readSelection(std::size_t selected, const std::string &lastOperation)
{
#if OS_Windows
    for (;;)
    {
        const int key = _getch();
        if (key == '\n' || key == '\r')
        {
            return selected;
        }
        if (key == 0 || key == 224)
        {
            const int arrow = _getch(); // Read the second character
            if (arrow == 80)            // Down arrow
            {
                selected = moveSelection(selected, true);
            }
            else if (arrow == 72) // Up arrow
            {
                selected = moveSelection(selected, false);
            }
            else
            {
                continue;
            }
            clearScreen();
            printMenu(selected, lastOperation);
        }
    }
#else
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1)
    {
        if (c == '\n')
        {
            return selected;
        }
        if (c != '\x1b')
        {
            continue;
        }

        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1 || read(STDIN_FILENO, &seq[1], 1) != 1)
        {
            return selected;
        }
        if (seq[0] != '[')
        {
            continue;
        }
        if (seq[1] == 'A') // Up
        {
            selected = moveSelection(selected, false);
        }
        else if (seq[1] == 'B') // Down
        {
            selected = moveSelection(selected, true);
        }
        else
        {
            continue;
        }
        clearScreen();
        printMenu(selected, lastOperation);
    }
    return selected;
#endif
}

} // namespace

int main()
{
    if (!hasResources())
    {
        std::cout << "Error: Missing resources. Please make sure the resources folder is in the "
                     "same directory as the executable."
                  << std::endl;
        std::string userInput;
        std::getline(std::cin, userInput);
        return -1;
    }

    if (!initLib())
    {
        return -1;
    }

    std::size_t selected = 0;
    std::string lastOperation = "Hit enter to apply";

    clearScreen();
    startMusic();
    printMenu(selected, lastOperation);

#if !OS_Windows
    enableRawMode();
#endif

    try
    {
        bool running = true;
        while (running)
        {
            selected = readSelection(selected, lastOperation);
            running = runMenuOption(selected, lastOperation);
            clearScreen();
            printMenu(selected, lastOperation);
        }
    }
    catch (const std::exception &e)
    {
        stopMusic();
        std::cout << std::endl << "  ERROR inesperado: " << e.what() << std::endl;
        std::cout << "  Los archivos originales quedaron con copia .tclib-bak." << std::endl;
        std::string userInput;
        std::getline(std::cin, userInput);
        return -1;
    }

    stopMusic();
    return 0;
}
