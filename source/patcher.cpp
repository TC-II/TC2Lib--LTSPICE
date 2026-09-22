#include "patcher.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

namespace fsys = std::filesystem;

namespace patcher
{

const char *const kCustomBlockMarker = "[Custon Components]";

namespace
{

const char *const kBlanks = " \t\r";

char lowerChar(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string toLower(const std::string &text)
{
    std::string lowered(text.size(), '\0');
    std::transform(text.begin(), text.end(), lowered.begin(), lowerChar);
    return lowered;
}

std::string trim(const std::string &text)
{
    const auto first = text.find_first_not_of(kBlanks);
    if (first == std::string::npos)
    {
        return std::string();
    }
    return text.substr(first, text.find_last_not_of(kBlanks) - first + 1);
}

bool equalsIgnoringCase(const std::string &a, const std::string &b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return lowerChar(x) == lowerChar(y);
           });
}

// --- Transcodificacion UTF-16 <-> UTF-8 ------------------------------------
//
// Cada unidad de codigo UTF-16 se codifica por separado, sin aparear
// subrogados. No es UTF-8 estricto para caracteres fuera del BMP, pero el ida y
// vuelta es exacto para cualquier entrada, que es lo unico que importa aca: el
// patcher solo compara ASCII y tiene que devolver el resto sin tocar.

void appendUtf8(std::string &out, unsigned int unit)
{
    if (unit < 0x80)
    {
        out.push_back(static_cast<char>(unit));
    }
    else if (unit < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
        out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    }
}

std::string utf16ToUtf8(const std::string &bytes, bool littleEndian)
{
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
    {
        const unsigned int low = static_cast<unsigned char>(bytes[i]);
        const unsigned int high = static_cast<unsigned char>(bytes[i + 1]);
        appendUtf8(out, littleEndian ? (high << 8) | low : (low << 8) | high);
    }
    return out;
}

std::string utf8ToUtf16(const std::string &text, bool littleEndian)
{
    std::string out;
    out.reserve(text.size() * 2);
    for (std::size_t i = 0; i < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        unsigned int unit = lead;
        std::size_t extra = 0;
        if ((lead & 0xE0) == 0xC0)
        {
            unit = lead & 0x1F;
            extra = 1;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            unit = lead & 0x0F;
            extra = 2;
        }
        if (i + extra >= text.size())
        {
            extra = 0;
            unit = lead;
        }
        for (std::size_t k = 1; k <= extra; ++k)
        {
            unit = (unit << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        i += extra + 1;

        const unsigned int first = littleEndian ? (unit & 0xFF) : (unit >> 8);
        const unsigned int second = littleEndian ? (unit >> 8) : (unit & 0xFF);
        out.push_back(static_cast<char>(first));
        out.push_back(static_cast<char>(second));
    }
    return out;
}

/// Parte el contenido ya decodificado en lineas, registrando el estilo de fin
/// de linea que traia.
void splitLines(const std::string &content, TextFile &file)
{
    file.crlf = content.find("\r\n") != std::string::npos;
    file.trailingNewline = !content.empty() && content.back() == '\n';

    std::string line;
    for (const char c : content)
    {
        if (c == '\n')
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            file.lines.push_back(line);
            line.clear();
        }
        else
        {
            line.push_back(c);
        }
    }
    if (!line.empty())
    {
        file.lines.push_back(line);
    }
}

std::string joinLines(const TextFile &file)
{
    const std::string newline = file.crlf ? "\r\n" : "\n";
    std::string content;
    for (std::size_t i = 0; i < file.lines.size(); ++i)
    {
        content += file.lines[i];
        if (i + 1 < file.lines.size() || file.trailingNewline)
        {
            content += newline;
        }
    }
    return content;
}

/// Indice de la linea que abre `section`, o npos.
std::size_t findSection(const std::vector<std::string> &lines, const std::string &section)
{
    const std::string header = "[" + section + "]";
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (equalsIgnoringCase(trim(lines[i]), header))
        {
            return i;
        }
    }
    return std::string::npos;
}

/// Indice de la primera linea despues del cuerpo de la seccion que abre en
/// `header`: la proxima cabecera, o el final del archivo.
std::size_t findSectionEnd(const std::vector<std::string> &lines, std::size_t header)
{
    for (std::size_t i = header + 1; i < lines.size(); ++i)
    {
        const std::string content = trim(lines[i]);
        if (!content.empty() && content.front() == '[')
        {
            return i;
        }
    }
    return lines.size();
}

/// Nombre de la clave que define `line` (lo que va antes del primer '='), o ""
/// si la linea no define ninguna.
std::string iniKeyOf(const std::string &line)
{
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos)
    {
        return std::string();
    }
    const std::string key = trim(line.substr(0, equals));
    if (!key.empty() && (key.front() == '[' || key.front() == ';'))
    {
        return std::string();
    }
    return key;
}

} // namespace

// ---------------------------------------------------------------------------
// Archivos de texto
// ---------------------------------------------------------------------------

bool readTextFile(const std::string &path, TextFile &file, std::string &error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "no se pudo abrir " + path;
        return false;
    }

    const std::string bytes((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());
    stream.close();

    file = TextFile();

    std::size_t offset = 0;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE)
    {
        file.encoding = Encoding::Utf16LE;
        file.hasBom = true;
        offset = 2;
    }
    else if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
             static_cast<unsigned char>(bytes[1]) == 0xFF)
    {
        file.encoding = Encoding::Utf16BE;
        file.hasBom = true;
        offset = 2;
    }
    else if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
             static_cast<unsigned char>(bytes[1]) == 0xBB &&
             static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        file.encoding = Encoding::Bytes;
        file.hasBom = true;
        offset = 3;
    }
    else if (bytes.size() >= 2 && bytes[0] != '\0' && bytes[1] == '\0')
    {
        // UTF-16LE sin BOM: los standard.* de LTspice arrancan con un caracter
        // ASCII, asi que el segundo byte es nulo.
        file.encoding = Encoding::Utf16LE;
    }
    else if (bytes.size() >= 2 && bytes[0] == '\0' && bytes[1] != '\0')
    {
        file.encoding = Encoding::Utf16BE;
    }

    const std::string body = bytes.substr(offset);
    switch (file.encoding)
    {
    case Encoding::Utf16LE:
        splitLines(utf16ToUtf8(body, true), file);
        break;
    case Encoding::Utf16BE:
        splitLines(utf16ToUtf8(body, false), file);
        break;
    case Encoding::Bytes:
        splitLines(body, file);
        break;
    }
    return true;
}

bool writeTextFile(const std::string &path, const TextFile &file, std::string &error)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        error = "no se pudo escribir " + path;
        return false;
    }

    std::string content = joinLines(file);
    switch (file.encoding)
    {
    case Encoding::Utf16LE:
        if (file.hasBom)
        {
            stream.write("\xFF\xFE", 2);
        }
        content = utf8ToUtf16(content, true);
        break;
    case Encoding::Utf16BE:
        if (file.hasBom)
        {
            stream.write("\xFE\xFF", 2);
        }
        content = utf8ToUtf16(content, false);
        break;
    case Encoding::Bytes:
        if (file.hasBom)
        {
            stream.write("\xEF\xBB\xBF", 3);
        }
        break;
    }

    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.close();
    if (!stream)
    {
        error = "no se pudo escribir " + path;
        return false;
    }
    return true;
}

bool writeTextFileSafely(const std::string &path, const TextFile &file, std::string &error)
{
    const fsys::path target(path);
    const fsys::path temporary = fsys::path(path + ".tclib-tmp");
    const fsys::path backup = fsys::path(path + ".tclib-bak");

    if (!writeTextFile(temporary.string(), file, error))
    {
        std::error_code ignored;
        fsys::remove(temporary, ignored);
        return false;
    }

    // La copia de seguridad se hace una sola vez: en la segunda corrida el
    // original ya esta parcheado y pisarla perderia el archivo intacto.
    std::error_code code;
    if (fsys::exists(target) && !fsys::exists(backup))
    {
        fsys::copy_file(target, backup, code);
        code.clear();
    }

    fsys::rename(temporary, target, code);
    if (code)
    {
        fsys::remove(target, code);
        fsys::rename(temporary, target, code);
    }
    if (code)
    {
        error = "no se pudo reemplazar " + path + ": " + code.message();
        std::error_code ignored;
        fsys::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool copyFileOverwriting(const std::string &source, const std::string &destination,
                         std::string &error)
{
    std::error_code code;
    if (fsys::exists(destination))
    {
        fsys::remove(destination, code);
        if (code)
        {
            error = "no se pudo reemplazar " + destination + ": " + code.message();
            return false;
        }
    }

    fsys::copy_file(source, destination, code);
    if (code)
    {
        error = "no se pudo copiar " + destination + ": " + code.message();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Modelos SPICE
// ---------------------------------------------------------------------------

std::string modelName(const std::string &line)
{
    static const std::string directiveText = ".model";
    static const std::string nameEnd = " \t\r(";

    const std::string lowered = toLower(line);
    const std::size_t directive = lowered.find(directiveText);
    if (directive == std::string::npos || lowered.find_first_not_of(kBlanks) != directive)
    {
        return std::string();
    }

    const std::size_t first = lowered.find_first_not_of(kBlanks, directive + directiveText.size());
    if (first == std::string::npos)
    {
        return std::string();
    }

    const std::size_t last = lowered.find_first_of(nameEnd, first);
    return lowered.substr(first, last == std::string::npos ? std::string::npos : last - first);
}

bool isContinuation(const std::string &line)
{
    const std::size_t first = line.find_first_not_of(kBlanks);
    return first != std::string::npos && line[first] == '+';
}

void mergeCustomComponents(const std::vector<std::string> &referenceLines,
                           std::vector<std::string> &configLines)
{
    std::set<std::string> ownModels;
    for (const std::string &referenceLine : referenceLines)
    {
        const std::string name = modelName(referenceLine);
        if (!name.empty())
        {
            ownModels.insert(name);
        }
    }

    // Sacar el bloque propio que dejo una corrida anterior. Si el archivo trae
    // un marcador suelto, sin cierre, no se toca nada: mejor dejar un duplicado
    // que vaciarle la biblioteca al usuario.
    const auto isMarker = [](const std::string &line) {
        return line.find(kCustomBlockMarker) != std::string::npos;
    };
    const auto blockBegin = std::find_if(configLines.begin(), configLines.end(), isMarker);
    if (blockBegin != configLines.end())
    {
        const auto blockEnd = std::find_if(blockBegin + 1, configLines.end(), isMarker);
        if (blockEnd != configLines.end())
        {
            configLines.erase(blockBegin, blockEnd + 1);
        }
    }

    std::vector<std::string> merged = referenceLines;
    merged.reserve(referenceLines.size() + configLines.size());

    bool skipping = false;
    for (const std::string &configLine : configLines)
    {
        // Una declaracion se extiende por sus lineas de continuacion, asi que el
        // descarte tiene que abarcarlas. Cualquier otra linea (comentario, linea
        // en blanco) lo corta, para no llevarse puesto lo que viene despues.
        if (!isContinuation(configLine))
        {
            const std::string name = modelName(configLine);
            skipping = !name.empty() && ownModels.count(name) != 0;
        }

        if (!skipping)
        {
            merged.push_back(configLine);
        }
    }

    configLines.swap(merged);
}

// ---------------------------------------------------------------------------
// Archivos .ini
// ---------------------------------------------------------------------------

bool setIniKey(std::vector<std::string> &lines, const std::string &section,
               const std::string &key, const std::string &value)
{
    const std::string entry = key + "=" + value;

    const std::size_t header = findSection(lines, section);
    if (header == std::string::npos)
    {
        lines.push_back("[" + section + "]");
        lines.push_back(entry);
        return true;
    }

    const std::size_t end = findSectionEnd(lines, header);
    for (std::size_t i = header + 1; i < end; ++i)
    {
        if (equalsIgnoringCase(iniKeyOf(lines[i]), key))
        {
            if (lines[i] == entry)
            {
                return false;
            }
            lines[i] = entry;
            return true;
        }
    }

    // La clave no estaba: va al final del cuerpo de la seccion, salteando las
    // lineas en blanco que la separan de la siguiente.
    std::size_t insertAt = end;
    while (insertAt > header + 1 && trim(lines[insertAt - 1]).empty())
    {
        --insertAt;
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), entry);
    return true;
}

bool replaceIniSection(std::vector<std::string> &lines, const std::string &section,
                       const std::vector<std::string> &content)
{
    const std::size_t header = findSection(lines, section);
    if (header == std::string::npos)
    {
        lines.push_back("[" + section + "]");
        lines.insert(lines.end(), content.begin(), content.end());
        return true;
    }

    const std::size_t end = findSectionEnd(lines, header);

    // El cuerpo actual puede terminar en lineas en blanco que separan de la
    // siguiente seccion: se conservan para no cambiar el aspecto del archivo.
    std::size_t bodyEnd = end;
    while (bodyEnd > header + 1 && trim(lines[bodyEnd - 1]).empty())
    {
        --bodyEnd;
    }

    const std::vector<std::string> current(lines.begin() + static_cast<std::ptrdiff_t>(header) + 1,
                                           lines.begin() + static_cast<std::ptrdiff_t>(bodyEnd));
    if (current == content)
    {
        return false;
    }

    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(header) + 1,
                lines.begin() + static_cast<std::ptrdiff_t>(bodyEnd));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(header) + 1, content.begin(),
                 content.end());
    return true;
}

} // namespace patcher
