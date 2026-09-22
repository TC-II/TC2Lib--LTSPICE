# TCLib para LTSpice

## Utilización

1. Clonar (o descargar y luego unzippear completamente) el repositorio.
2. Cerrar todas las instancias abiertas de LTSpice.
3. Ejecutar el patcher `TCLib.exe` (`TCLib` en linux) y usar el menú para aplicar los parches deseados.

El patcher solo escribe dentro del perfil del usuario (`%LOCALAPPDATA%\LTspice` y `%APPDATA%\LTspice.ini`), así que no hace falta ejecutarlo como administrador. Si por la configuración del equipo no tuviera permiso de escritura, avisa antes de tocar nada.

Antes de modificar un archivo de LTspice, el patcher guarda una copia del original con extensión `.tclib-bak` la primera vez que lo toca. Para volver atrás alcanza con renombrar esa copia sobre el archivo original.

## Desarrollo

### Estructura

| Archivo | Qué hace |
|---|---|
| `source/patcher.{h,cpp}` | Lógica pura: leer y escribir archivos conservando su codificación, fusionar modelos SPICE y editar `.ini`. No depende de Windows, de miniaudio ni de la consola. |
| `source/setup-funcs.{h,cpp}` | Rutas de LTspice, acciones del menú, audio y consola. |
| `source/TCLib.cpp` | Navegación del menú. |
| `source/ascii-art.h` | Arte ASCII de la interfaz. |
| `tests/` | Tests de `patcher.cpp` y sus archivos de ejemplo. |

Las opciones del menú están en una única tabla, en `source/setup-funcs.cpp`: agregar una opción es agregar una fila.

### Compilación

Con CMake, desde la raíz del repositorio:

```
cmake -S source -B build
cmake --build build
```

El ejecutable queda en la raíz del repositorio, al lado de `resources/`, `sym/` y `sub/`, que es donde el patcher los busca.

A mano, desde `source/`:

```
g++ -static TCLib.cpp setup-funcs.cpp patcher.cpp -o ../TCLib.exe
```

### Tests

Los tests no necesitan LTspice instalado. Con CMake:

```
cmake --build build --target run-tests
ctest --test-dir build --output-on-failure
```

O a mano, desde la raíz del repositorio:

```
g++ -static -std=c++17 -I source tests/run-tests.cpp source/patcher.cpp -o tests/run-tests.exe
./tests/run-tests.exe
```

Conviene correrlos antes de cada commit que toque `source/patcher.cpp`.

## Integridad de archivos

### Windows

Compilado en el entorno `MSYS2 UCRT64` en el directorio `/source` mediante el comando `g++ -static TCLib.cpp setup-funcs.cpp patcher.cpp -o ..\TCLib.exe` desde el directorio `source/`, con `g++.exe (Rev3, Built by MSYS2 project) 14.1.0`. Hash MD5 de `TCLib.exe` (obtenido con `certutil -hashfile TCLib.exe MD5`): `73f88116a00268c4fc66fb96f8975cda`.

### Linux

Compilado en el entorno `MSYS2 UCRT64` en el directorio `/source` mediante el comando `g++ -static TCLib.cpp setup-funcs.cpp patcher.cpp -o ../TCLib` desde el directorio `source/`, con `g++ (Ubuntu 11.4.0-1ubuntu1~22.04) 11.4.0`. Hash MD5 de `TCLib` (obtenido con `md5sum TCLib`): `ab928067155eaf59c17d21cc26c5364b`
