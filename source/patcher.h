#ifndef PATCHER_H
#define PATCHER_H

// Logica pura del patcher: leer y escribir archivos de texto conservando su
// formato, fusionar modelos SPICE y editar archivos .ini.
//
// Este modulo no depende de Windows, de miniaudio ni de la interfaz: se compila
// y se testea solo (ver tests/run-tests.cpp).

#include <string>
#include <vector>

namespace patcher
{

// ---------------------------------------------------------------------------
// Archivos de texto
// ---------------------------------------------------------------------------

/// LTspice mezcla codificaciones entre sus propios archivos: standard.bjt es
/// UTF-16LE sin BOM y standard.dio es ANSI de un byte. En vez de fijarla por
/// archivo, se detecta al leer y se conserva al escribir.
enum class Encoding
{
    Bytes,   ///< ANSI, Latin-1 o UTF-8: se trata como bytes opacos, sin decodificar.
    Utf16LE,
    Utf16BE
};

/// Contenido de un archivo mas todo lo necesario para reescribirlo igual.
///
/// `lines` no incluye los terminadores de linea. En archivos UTF-16 el texto se
/// guarda transcodificado a UTF-8 y se vuelve a codificar al escribir; en los
/// demas se guardan los bytes tal cual, porque no siempre son UTF-8 valido.
struct TextFile
{
    Encoding encoding = Encoding::Bytes;
    bool hasBom = false;
    bool crlf = false;
    bool trailingNewline = true;
    std::vector<std::string> lines;
};

/// Lee `path` detectando codificacion, fin de linea y BOM.
bool readTextFile(const std::string &path, TextFile &file, std::string &error);

/// Escribe `file` en `path` con el formato que trae. Sobrescribe sin red: usar
/// writeTextFileSafely() para modificar archivos del usuario.
bool writeTextFile(const std::string &path, const TextFile &file, std::string &error);

/// Reemplaza `path` de forma segura: escribe un temporal completo, guarda una
/// copia del original la primera vez (.tclib-bak) y recien ahi reemplaza. Si algo
/// falla en el camino, el archivo original queda intacto.
bool writeTextFileSafely(const std::string &path, const TextFile &file, std::string &error);

/// Copia `source` sobre `destination`, exista o no.
///
/// No alcanza con std::filesystem::copy_options::overwrite_existing: en
/// libstdc++ sobre Windows esa opcion devuelve "File exists" igual cuando el
/// destino ya esta, asi que hay que borrarlo primero. Verificado con g++ 13.2 y
/// 14.1 de MinGW; hay un test que lo cubre.
bool copyFileOverwriting(const std::string &source, const std::string &destination,
                         std::string &error);

// ---------------------------------------------------------------------------
// Modelos SPICE
// ---------------------------------------------------------------------------

/// Marcador que delimita el bloque propio dentro de los standard.* de LTspice.
/// El error de tipeo es historico: cambiarlo dejaria dos bloques en los archivos
/// ya parcheados de todos los que vienen usando la lib.
extern const char *const kCustomBlockMarker;

/// Nombre (en minusculas) del modelo que declara `line`, o "" si la linea no es
/// una directiva .model. Tolera espacios o tabs de sobra y nombres pegados al
/// parentesis: ".MODEL  BC547C NPN(...)" y ".model BC547C(...)" son validos. Se
/// exige que la directiva abra la linea: un comentario que la mencione no
/// declara nada. Los nombres de modelo en SPICE no distinguen mayusculas, por
/// eso siempre se devuelven en minusculas.
std::string modelName(const std::string &line);

/// true si la linea continua la declaracion anterior: SPICE usa un mas inicial.
bool isContinuation(const std::string &line);

/// Deja en `configLines` el bloque propio seguido de la lista original de
/// LTspice sin los modelos que el bloque redefine, y sin el bloque que pueda
/// haber dejado una corrida anterior. Es idempotente.
void mergeCustomComponents(const std::vector<std::string> &referenceLines,
                           std::vector<std::string> &configLines);

// ---------------------------------------------------------------------------
// Archivos .ini
// ---------------------------------------------------------------------------

/// Deja `key=value` dentro de `section`, sin tocar claves de otras secciones ni
/// claves cuyo nombre contenga a `key` (PenWidth no es DataPenWidth). Si la
/// clave no esta se agrega al final de la seccion, y si la seccion no esta se
/// agrega al final del archivo. Devuelve true si el archivo cambio.
bool setIniKey(std::vector<std::string> &lines, const std::string &section,
               const std::string &key, const std::string &value);

/// Reemplaza el cuerpo de `section` por `content`. Si la seccion no existe la
/// agrega al final. Devuelve true si el archivo cambio.
bool replaceIniSection(std::vector<std::string> &lines, const std::string &section,
                       const std::vector<std::string> &content);

} // namespace patcher

#endif // PATCHER_H
