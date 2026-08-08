#include "vod_common.h"

#include <climits>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <fs/directory.h>

using namespace std;

bool sLastMsg = false;

std::string toNativeSeparators(const std::string& dirName)
{
    std::string rez = dirName;
    for (char& c : rez)
    {
        if (c == '\\' || c == '/')
            c = getDirSeparator();
    }
    return rez;
}

bool isFillerNullPacket(uint8_t* curBuf)
{
    const auto endPos = reinterpret_cast<uint32_t*>(curBuf + TS_FRAME_SIZE);
    for (auto curBuf32 = reinterpret_cast<uint32_t*>(curBuf + 4); curBuf32 < endPos; curBuf32++)
        if (*curBuf32 != UINT_MAX && *curBuf32 != 0)
            return false;
    return true;
}

std::string unquoteStr(const std::string& val)
{
    std::string tmp = val;
    if (!val.empty() && val[0] == '\"')
        tmp = tmp.substr(1, tmp.size());
    if (!val.empty() && val[val.size() - 1] == '\"')
        tmp = tmp.substr(0, tmp.size() - 1);
    return tmp;
}

std::string quoteStr(const std::string& val) { return "\"" + val + "\""; }

std::vector<std::string> extractFileList(const std::string& val)
{
    std::vector<std::string> rez;
    bool quoted = false;
    size_t lastStartPos = 0;
    for (size_t i = 0; i < val.size(); i++)
    {
        if (val[i] == '"')
            quoted = !quoted;
        else if (val[i] == '+' && !quoted)
        {
            if (i > lastStartPos)
            {
                std::string tmp = val.substr(lastStartPos, i - lastStartPos);
                tmp = trimStr(tmp);
                tmp = unquoteStr(tmp);
                if (!tmp.empty())
                    rez.push_back(tmp);
            }
            lastStartPos = i + 1;
        }
    }
    if (rez.empty())
        rez.push_back(unquoteStr(trimStr(val)));
    else
    {
        std::string tmp = val.substr(lastStartPos, val.size());
        tmp = trimStr(tmp);
        tmp = unquoteStr(tmp);
        if (!tmp.empty())
            rez.push_back(tmp);
    }
    return rez;
}

uint16_t AV_RB16(const uint8_t* buffer) { return static_cast<uint16_t>(*buffer << 8 | buffer[1]); }
uint32_t AV_RB32(uint8_t* buffer) { return my_ntohl(*reinterpret_cast<uint32_t*>(buffer)); }
uint32_t AV_RB24(const uint8_t* buffer) { return buffer[0] << 16 | buffer[1] << 8 | buffer[2]; }

void AV_WB16(uint8_t* buffer, const uint16_t value) { *reinterpret_cast<uint16_t*>(buffer) = my_htons(value); }

void AV_WB24(uint8_t* buffer, const uint32_t value)
{
    buffer[0] = static_cast<uint8_t>(value >> 16);
    buffer[1] = static_cast<uint8_t>(value >> 8);
    buffer[2] = static_cast<uint8_t>(value);
}
void AV_WB32(uint8_t* buffer, const uint32_t value) { *reinterpret_cast<uint32_t*>(buffer) = my_htonl(value); }
void AV_WB64(uint8_t* buffer, const uint64_t value) { *reinterpret_cast<uint64_t*>(buffer) = my_htonll(value); }

std::string floatToTime(const double time, const char msSeparator)
{
    int iTime = static_cast<int>(time);
    const int hour = iTime / 3600;
    iTime -= hour * 3600;
    const int min = iTime / 60;
    iTime -= min * 60;
    const int sec = iTime;
    const int msec = static_cast<int>((time - static_cast<int>(time)) * 1000.0);
    std::ostringstream str;
    str << strPadLeft(int32ToStr(hour), 2, '0') << ':';
    str << strPadLeft(int32ToStr(min), 2, '0') << ':';
    str << strPadLeft(int32ToStr(sec), 2, '0') << msSeparator << strPadLeft(int32ToStr(msec), 3, '0');
    return str.str();
}

double timeToFloat(const std::string& chapterStr)
{
    if (chapterStr.empty())
        return 0;
    const std::vector<std::string> timeParts = splitStr(chapterStr.c_str(), ':');
    double sec = 0;
    if (!timeParts.empty())
        sec = strToDouble(timeParts[timeParts.size() - 1].c_str());
    int min = 0;
    if (timeParts.size() > 1)
        min = strToInt32(timeParts[timeParts.size() - 2].c_str());
    int hour = 0;
    if (timeParts.size() > 2)
        hour = strToInt32(timeParts[timeParts.size() - 3].c_str());
    return hour * 3600 + min * 60 + sec;
}

double correctFps(const double fps)
{
    struct FPSCorrect
    {
        double from;
        double to;
    };

    FPSCorrect fpsCorrectList[] = {
        {5.994, 5.99400599400599},   {11.988, 11.98801198801198}, {23.976, 23.97602397602397},
        {47.952, 47.95204795204795}, {7.4925, 7.49250749250749},  {14.985, 14.98501498501498},
        {29.97, 29.97002997002997},  {59.94, 59.94005994005994},
    };

    for (const auto& i : fpsCorrectList)
    {
        if (fabs(fps - i.from) < 1e-4)
        {
            return i.to;
        }
    }
    return fps;
}

// ---------------------------------------------------------------------------------------
// Audio delay carried in a file name ("<name> DELAY -17ms.<ext>", the eac3to convention).
//
// Only "ms" is emitted, because that is what eac3to writes and what other tools expect, but
// the parser accepts the same suffixes the meta timeshift parameter does so that a value
// copied from a meta file behaves the same way in a name.
// ---------------------------------------------------------------------------------------

static const char DELAY_TOKEN[] = " DELAY ";

int64_t parseTimeshiftToMs(const std::string& value)
{
    // Order matters: "ns" contains "s", so it must be tested before the bare "s" case, and
    // "ms" likewise. Mirrors the parsing order used for the timeshift meta parameter.
    size_t pos;
    if ((pos = value.find("ns")) != std::string::npos)
        return strToInt64(value.substr(0, pos).c_str()) / 1000000;
    if ((pos = value.find("ms")) != std::string::npos)
        return strToInt64(value.substr(0, pos).c_str());
    if ((pos = value.find('s')) != std::string::npos)
        return strToInt64(value.substr(0, pos).c_str()) * 1000;
    return strToInt64(value.c_str());
}

// Locate the last " DELAY <n>ms" that sits immediately before the extension. Returns the
// offset of the token, or npos. strict form only: optional sign, digits, then "ms".
static size_t findDelayToken(const std::string& name, int64_t* valueOut)
{
    size_t best = std::string::npos;
    size_t from = 0;
    while (true)
    {
        const size_t at = name.find(DELAY_TOKEN, from);
        if (at == std::string::npos)
            break;
        size_t p = at + sizeof(DELAY_TOKEN) - 1;
        const size_t numStart = p;
        if (p < name.size() && (name[p] == '-' || name[p] == '+'))
            p++;
        const size_t digitsStart = p;
        while (p < name.size() && name[p] >= '0' && name[p] <= '9') p++;
        if (p > digitsStart && p + 1 < name.size() + 1 && name.compare(p, 2, "ms") == 0)
        {
            // Accept only when "ms" ends the name, or is followed by an extension. This
            // keeps a directory called "DELAY 5ms" in the middle of a path from matching.
            const size_t after = p + 2;
            if (after == name.size() || name[after] == '.')
            {
                best = at;
                if (valueOut)
                    *valueOut = strToInt64(name.substr(numStart, p - numStart).c_str());
            }
        }
        from = at + 1;
    }
    return best;
}

int64_t delayFromFileName(const std::string& fileName)
{
    std::string name = extractFileName(fileName);
    int64_t value = 0;
    if (findDelayToken(name, &value) == std::string::npos)
        return 0;
    return value;
}

std::string stripDelayToken(const std::string& fileName)
{
    const size_t at = findDelayToken(fileName, nullptr);
    if (at == std::string::npos)
        return fileName;
    return fileName.substr(0, at);
}
