#ifndef SETUP_FUNCS_H
#define SETUP_FUNCS_H

#ifdef __unix__ /* __unix__ is usually defined by compilers targeting Unix systems */

#define OS_Windows 0

#elif defined(_WIN32) || defined(WIN32) /* _Win32 is usually defined by compilers targeting 32 or   64 bit Windows systems */

#define OS_Windows 1

#endif

#include <cstddef>
#include <iostream>
#include <string>

/// Resuelve las rutas de LTspice y verifica que esten los archivos del patcher.
/// Devuelve false y explica el motivo si no se puede seguir.
bool initLib();

/// Cantidad de opciones del menu. La tabla vive en setup-funcs.cpp: esta es la
/// unica fuente de verdad para navegarlo.
std::size_t menuLength();

/// Dibuja el menu con la opcion `selected` resaltada y el resultado de la
/// ultima accion.
void printMenu(std::size_t selected, const std::string &lastOperation);

/// Ejecuta la opcion `selected`, deja en `lastOperation` el mensaje a mostrar y
/// devuelve false si el usuario eligio salir.
bool runMenuOption(std::size_t selected, std::string &lastOperation);

void clearScreen();

/// true si el directorio del ejecutable tiene los archivos que el patcher copia.
bool hasResources();

/// Arranca la musica de fondo. Si falla, el resto del programa sigue andando y
/// las opciones de volumen quedan inertes.
void startMusic();
void stopMusic();

#endif // SETUP_FUNCS_H
