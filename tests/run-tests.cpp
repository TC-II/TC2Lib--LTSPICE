// Tests de la logica pura del patcher (source/patcher.cpp).
//
// Compilar y correr desde la raiz del repositorio:
//   g++ -std=c++17 -I source tests/run-tests.cpp source/patcher.cpp -o run-tests
//   ./run-tests
//
// No necesita LTspice instalado: usa los archivos de tests/data.

#include "patcher.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <iostream>
#include <string>
#include <vector>

namespace fsys = std::filesystem;
using patcher::TextFile;

namespace
{

int checksRun = 0;
int checksFailed = 0;
std::string currentTest;

void startTest(const std::string &name)
{
    currentTest = name;
}

void check(bool condition, const std::string &what)
{
    ++checksRun;
    if (!condition)
    {
        ++checksFailed;
        std::cout << "  FALLO  " << currentTest << ": " << what << std::endl;
    }
}

template <typename T>
void checkEqual(const T &actual, const T &expected, const std::string &what)
{
    ++checksRun;
    if (!(actual == expected))
    {
        ++checksFailed;
        std::cout << "  FALLO  " << currentTest << ": " << what << std::endl;
        std::cout << "         esperado: [" << expected << "]" << std::endl;
        std::cout << "         obtenido: [" << actual << "]" << std::endl;
    }
}

std::string dataPath(const std::string &name)
{
    return (fsys::path("tests") / "data" / name).string();
}

std::string tempPath(const std::string &name)
{
    const fsys::path directory = fsys::path("tests") / "tmp";
    fsys::create_directories(directory);
    return (directory / name).string();
}

std::string readBytes(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

std::vector<std::string> linesOf(const std::string &path)
{
    TextFile file;
    std::string error;
    if (!patcher::readTextFile(path, file, error))
    {
        std::cout << "  ERROR  no se pudo leer " << path << ": " << error << std::endl;
        ++checksFailed;
    }
    return file.lines;
}

int countModel(const std::vector<std::string> &lines, const std::string &name)
{
    int count = 0;
    for (const std::string &line : lines)
    {
        if (patcher::modelName(line) == name)
        {
            ++count;
        }
    }
    return count;
}

bool contains(const std::vector<std::string> &lines, const std::string &text)
{
    for (const std::string &line : lines)
    {
        if (line.find(text) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------------------------

void testModelName()
{
    startTest("modelName");
    checkEqual(patcher::modelName(".model 2N3904 NPN(IS=1E-14)"), std::string("2n3904"),
               "caso normal");
    checkEqual(patcher::modelName(".MODEL BC547C NPN(IS=1)"), std::string("bc547c"),
               "directiva en mayusculas");
    checkEqual(patcher::modelName(".MODEL  BD137 NPN(IS=1)"), std::string("bd137"),
               "dos espacios despues de la directiva");
    checkEqual(patcher::modelName(".model\tTIP31C NPN(IS=1)"), std::string("tip31c"),
               "tab despues de la directiva");
    checkEqual(patcher::modelName(".model BC337(IS=1)"), std::string("bc337"),
               "nombre pegado al parentesis");
    checkEqual(patcher::modelName("  .model 2N2222 NPN(IS=1)"), std::string("2n2222"),
               "directiva indentada");
    checkEqual(patcher::modelName(".model 2N2222 NPN(IS=1)\r"), std::string("2n2222"),
               "linea con CR al final");
    checkEqual(patcher::modelName("* .model 2N2222 es obsoleto"), std::string(),
               "mencion en un comentario no cuenta");
    checkEqual(patcher::modelName("+ BF=200 IKF=0.3"), std::string(), "linea de continuacion");
    checkEqual(patcher::modelName(""), std::string(), "linea vacia");
    checkEqual(patcher::modelName(".model"), std::string(), "directiva sin nombre");
    checkEqual(patcher::modelName(".model   "), std::string(), "directiva con espacios sueltos");

    check(patcher::isContinuation("+ BF=200"), "isContinuation con mas inicial");
    check(patcher::isContinuation("  +BF=200"), "isContinuation indentado");
    check(!patcher::isContinuation(".model X NPN(1)"), "isContinuation con directiva");
    check(!patcher::isContinuation(""), "isContinuation con linea vacia");
}

void testMergeDeduplica()
{
    startTest("merge sobre archivo virgen");
    const std::vector<std::string> reference = linesOf(dataPath("reference.ini"));
    std::vector<std::string> config = linesOf(dataPath("virgin.bjt"));

    check(countModel(config, "2n2222") == 1, "el archivo virgen trae el 2N2222 de fabrica");
    patcher::mergeCustomComponents(reference, config);

    checkEqual(countModel(config, "2n2222"), 1, "queda una sola definicion de 2N2222");
    checkEqual(countModel(config, "bd137"), 1, "queda una sola definicion de BD137");
    check(!contains(config, "DE_FABRICA"), "se descarto la definicion de fabrica completa");
    check(!contains(config, "BF=999"), "se descartaron tambien sus lineas de continuacion");
    checkEqual(countModel(config, "2n3904"), 1, "los modelos que no se redefinen quedan");
    check(contains(config, "* comentario del fabricante de BC337"),
          "no se come el comentario del modelo siguiente");
    checkEqual(countModel(config, "bc337"), 1, "el modelo siguiente sobrevive");
    check(contains(config, "* encabezado que estaba antes del bloque"),
          "conserva las lineas previas al marcador");
}

void testMergeIdempotente()
{
    startTest("merge idempotente");
    const std::vector<std::string> reference = linesOf(dataPath("reference.ini"));
    std::vector<std::string> once = linesOf(dataPath("virgin.bjt"));
    patcher::mergeCustomComponents(reference, once);

    std::vector<std::string> twice = once;
    patcher::mergeCustomComponents(reference, twice);
    check(once == twice, "la segunda corrida no cambia nada");

    std::vector<std::string> thrice = twice;
    patcher::mergeCustomComponents(reference, thrice);
    check(twice == thrice, "la tercera corrida tampoco");
}

void testMergeMarcadorSuelto()
{
    startTest("merge con marcador sin cierre");
    const std::vector<std::string> reference = linesOf(dataPath("reference.ini"));
    std::vector<std::string> config = {
        std::string("* ") + patcher::kCustomBlockMarker,
        ".model 2N3904 NPN(IS=1E-14)",
        ".model 2N5401 PNP(IS=1E-14)",
    };
    patcher::mergeCustomComponents(reference, config);
    checkEqual(countModel(config, "2n3904"), 1, "no borra el archivo ante un marcador suelto");
    checkEqual(countModel(config, "2n5401"), 1, "conserva el resto de los modelos");
}

void testCodificacionIdaYVuelta()
{
    startTest("lectura y escritura sin cambios");
    const std::string cases[] = {"virgin.bjt", "utf16le-bom.txt", "ansi-crlf.txt", "utf8-bom.txt"};
    for (const std::string &name : cases)
    {
        TextFile file;
        std::string error;
        check(patcher::readTextFile(dataPath(name), file, error), "se lee " + name);

        const std::string output = tempPath(name);
        check(patcher::writeTextFile(output, file, error), "se escribe " + name);
        checkEqual(readBytes(output), readBytes(dataPath(name)),
                   name + " se reescribe byte a byte igual");
    }

    TextFile utf16;
    std::string error;
    patcher::readTextFile(dataPath("virgin.bjt"), utf16, error);
    check(utf16.encoding == patcher::Encoding::Utf16LE, "detecta UTF-16LE sin BOM");
    check(!utf16.hasBom, "detecta la ausencia de BOM");
    check(!utf16.crlf, "detecta finales LF");

    TextFile ansi;
    patcher::readTextFile(dataPath("ansi-crlf.txt"), ansi, error);
    check(ansi.encoding == patcher::Encoding::Bytes, "no decodifica un archivo ANSI");
    check(ansi.crlf, "detecta finales CRLF");

    TextFile bom;
    patcher::readTextFile(dataPath("utf16le-bom.txt"), bom, error);
    check(bom.encoding == patcher::Encoding::Utf16LE && bom.hasBom, "detecta UTF-16LE con BOM");
}

void testEscrituraSegura()
{
    startTest("escritura segura");
    const std::string target = tempPath("seguro.txt");
    const std::string backup = target + ".tclib-bak";
    std::error_code ignored;
    fsys::remove(target, ignored);
    fsys::remove(backup, ignored);

    TextFile file;
    file.lines = {"original"};
    std::string error;
    check(patcher::writeTextFile(target, file, error), "crea el archivo");

    file.lines = {"parcheado"};
    check(patcher::writeTextFileSafely(target, file, error), "reemplaza el archivo");
    check(fsys::exists(backup), "deja una copia de seguridad");
    checkEqual(linesOf(backup).front(), std::string("original"), "la copia tiene el contenido previo");
    checkEqual(linesOf(target).front(), std::string("parcheado"), "el archivo quedo parcheado");

    file.lines = {"parcheado de nuevo"};
    check(patcher::writeTextFileSafely(target, file, error), "reemplaza por segunda vez");
    checkEqual(linesOf(backup).front(), std::string("original"),
               "la copia sigue siendo la del archivo intacto");
    check(!fsys::exists(target + ".tclib-tmp"), "no deja archivos temporales");
}

void testCopiaPisandoElDestino()
{
    // Regresion: std::filesystem::copy_file con overwrite_existing devuelve
    // "File exists" en libstdc++ sobre Windows, asi que copyFileOverwriting
    // tiene que borrar el destino antes de copiar.
    startTest("copia pisando el destino");
    const std::string source = tempPath("origen.txt");
    const std::string destination = tempPath("destino.txt");

    {
        std::ofstream(source) << "contenido nuevo";
        std::ofstream(destination) << "contenido viejo, mas largo que el nuevo";
    }

    std::string error;
    check(patcher::copyFileOverwriting(source, destination, error),
          "copia sobre un destino que ya existe: " + error);
    checkEqual(readBytes(destination), std::string("contenido nuevo"), "el destino quedo pisado");

    std::error_code ignored;
    fsys::remove(destination, ignored);
    check(patcher::copyFileOverwriting(source, destination, error),
          "copia cuando el destino no existe: " + error);
    checkEqual(readBytes(destination), std::string("contenido nuevo"), "el destino se creo");

    check(!patcher::copyFileOverwriting(tempPath("no-existe.txt"), destination, error),
          "avisa si el origen no existe");
}

void testSetIniKey()
{
    startTest("setIniKey");
    std::vector<std::string> lines = linesOf(dataPath("sample.ini"));

    check(patcher::setIniKey(lines, "Options", "PenWidth", "2"), "cambia PenWidth");
    check(contains(lines, "PenWidth=2"), "PenWidth quedo en 2");
    check(contains(lines, "DataPenWidth=1"),
          "no toca DataPenWidth, que contiene el nombre de la clave");
    check(contains(lines, "SchPenWidth=1"), "no toca SchPenWidth");
    checkEqual(std::count(lines.begin(), lines.end(), std::string("PenWidth=2")),
               static_cast<std::ptrdiff_t>(1), "no duplica la clave");

    check(!patcher::setIniKey(lines, "Options", "PenWidth", "2"),
          "aplicar el mismo valor no cambia nada");

    const std::size_t before = lines.size();
    check(patcher::setIniKey(lines, "Options", "ClaveNueva", "7"), "agrega una clave que falta");
    checkEqual(lines.size(), before + 1, "agrega exactamente una linea");
    check(contains(lines, "ClaveNueva=7"), "la clave nueva esta");

    startTest("setIniKey respeta las secciones");
    check(patcher::setIniKey(lines, "SchKeyBoardShortCut", "Draw_Lines", "J"),
          "cambia el atajo del esquematico");
    check(contains(lines, "Draw_Lines=J"), "el atajo del esquematico quedo en J");
    check(contains(lines, "Draw_Lines=L"), "el atajo del editor de simbolos sigue en L");

    startTest("setIniKey conserva el resto del archivo");
    check(lines.front() == "[Options]", "no se come la primera linea");
    check(contains(lines, "UUID=16208976118632128081"), "no se come la segunda linea");
    check(contains(lines, "[AsyKeyBoardShortCut]"), "conserva las cabeceras de seccion");

    startTest("setIniKey con seccion inexistente");
    std::vector<std::string> vacio;
    check(patcher::setIniKey(vacio, "Options", "PenWidth", "2"), "agrega la seccion");
    checkEqual(vacio.size(), static_cast<std::size_t>(2), "agrega cabecera y clave");
    checkEqual(vacio[0], std::string("[Options]"), "la cabecera es la correcta");
    checkEqual(vacio[1], std::string("PenWidth=2"), "la clave es la correcta");
}

void testReplaceIniSection()
{
    startTest("replaceIniSection");
    std::vector<std::string> lines = linesOf(dataPath("sample.ini"));
    const std::vector<std::string> theme = {"Grid=1", "WaveColor0=2"};

    check(patcher::replaceIniSection(lines, "Colors", theme), "reemplaza la seccion");
    check(contains(lines, "Grid=1"), "entro el contenido nuevo");
    check(!contains(lines, "Grid=6579300"), "salio el contenido viejo");
    check(contains(lines, "[SchKeyBoardShortCut]"), "la seccion siguiente sigue ahi");
    check(contains(lines, "PenWidth=1"), "la seccion anterior sigue ahi");

    check(!patcher::replaceIniSection(lines, "Colors", theme),
          "reemplazar por lo mismo no cambia nada");

    std::vector<std::string> sinColores = {"[Options]", "PenWidth=1"};
    check(patcher::replaceIniSection(sinColores, "Colors", theme), "agrega la seccion si falta");
    check(contains(sinColores, "[Colors]"), "quedo la cabecera");
    check(contains(sinColores, "Grid=1"), "quedo el contenido");
}

} // namespace

int main()
{
    if (!fsys::exists(dataPath("reference.ini")))
    {
        std::cout << "Correr desde la raiz del repositorio: no se encuentra tests/data."
                  << std::endl;
        return 2;
    }

    testModelName();
    testMergeDeduplica();
    testMergeIdempotente();
    testMergeMarcadorSuelto();
    testCodificacionIdaYVuelta();
    testEscrituraSegura();
    testCopiaPisandoElDestino();
    testSetIniKey();
    testReplaceIniSection();

    std::error_code ignored;
    fsys::remove_all(fsys::path("tests") / "tmp", ignored);

    std::cout << std::endl;
    if (checksFailed == 0)
    {
        std::cout << checksRun << " verificaciones, todo OK." << std::endl;
        return 0;
    }
    std::cout << checksFailed << " de " << checksRun << " verificaciones fallaron." << std::endl;
    return 1;
}
