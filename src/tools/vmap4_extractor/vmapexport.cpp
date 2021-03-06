/*
 * Copyright (C) 2008-2017 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "adtfile.h"
#include "Banner.h"
#include "Common.h"
#include "cascfile.h"
#include "DB2CascFileSource.h"
#include "DB2Meta.h"
#include "StringFormat.h"
#include "vmapexport.h"
#include "wdtfile.h"
#include "wmo.h"
#include <CascLib.h>
#include <boost/filesystem/operations.hpp>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>

#ifdef WIN32
    #include <direct.h>
    #define mkdir _mkdir
#else
    #define ERROR_PATH_NOT_FOUND ERROR_FILE_NOT_FOUND
#endif

//------------------------------------------------------------------------------
// Defines

#define MPQ_BLOCK_SIZE 0x1000

//-----------------------------------------------------------------------------

CASC::StorageHandle CascStorage;

typedef struct
{
    char name[64];
    unsigned int id;
}map_id;

std::vector<map_id> map_ids;
uint32 map_count;
boost::filesystem::path input_path;
bool preciseVectorData = false;

struct MapLoadInfo
{
    static DB2FileLoadInfo const* Instance()
    {
        static DB2FieldMeta const fields[] =
        {
            { false, FT_INT, "ID" },
            { false, FT_STRING_NOT_LOCALIZED, "Directory" },
            { false, FT_INT, "Flags1" },
            { false, FT_INT, "Flags2" },
            { false, FT_FLOAT, "MinimapIconScale" },
            { false, FT_FLOAT, "CorpsePosX" },
            { false, FT_FLOAT, "CorpsePosY" },
            { false, FT_STRING, "MapName" },
            { false, FT_STRING, "MapDescription0" },
            { false, FT_STRING, "MapDescription1" },
            { false, FT_STRING, "ShortDescription" },
            { false, FT_STRING, "LongDescription" },
            { false, FT_SHORT, "AreaTableID" },
            { false, FT_SHORT, "LoadingScreenID" },
            { true, FT_SHORT, "CorpseMapID" },
            { false, FT_SHORT, "TimeOfDayOverride" },
            { true, FT_SHORT, "ParentMapID" },
            { true, FT_SHORT, "CosmeticParentMapID" },
            { false, FT_SHORT, "WindSettingsID" },
            { false, FT_BYTE, "InstanceType" },
            { false, FT_BYTE, "unk5" },
            { false, FT_BYTE, "ExpansionID" },
            { false, FT_BYTE, "MaxPlayers" },
            { false, FT_BYTE, "TimeOffset" },
        };
        static char const* types = "siffssssshhhhhhhbbbbb";
        static uint8 const arraySizes[21] = { 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
        static DB2FieldDefault const fieldDefaults[21] = { "", uint32(0), float(0), float(0), "", "", "", "", "", uint16(0), uint16(0), uint16(0), uint16(0), uint16(0), uint16(0), uint16(0), uint8(0), uint8(0), uint8(0), uint8(0), uint8(0) };
        static DB2Meta const meta(-1, 21, 0xC34CD39B, types, arraySizes, fieldDefaults);
        static DB2FileLoadInfo const loadInfo(&fields[0], std::extent<decltype(fields)>::value, &meta);
        return &loadInfo;
    }
};

// Constants

//static const char * szWorkDirMaps = ".\\Maps";
const char* szWorkDirWmo = "./Buildings";
const char* szRawVMAPMagic = "VMAP044";

#define CASC_LOCALES_COUNT 17
char const* CascLocaleNames[CASC_LOCALES_COUNT] =
{
    "none", "enUS",
    "koKR", "unknown",
    "frFR", "deDE",
    "zhCN", "esES",
    "zhTW", "enGB",
    "enCN", "enTW",
    "esMX", "ruRU",
    "ptBR", "itIT",
    "ptPT"
};

uint32 WowLocaleToCascLocaleFlags[12] =
{
    CASC_LOCALE_ENUS | CASC_LOCALE_ENGB,
    CASC_LOCALE_KOKR,
    CASC_LOCALE_FRFR,
    CASC_LOCALE_DEDE,
    CASC_LOCALE_ZHCN,
    CASC_LOCALE_ZHTW,
    CASC_LOCALE_ESES,
    CASC_LOCALE_ESMX,
    CASC_LOCALE_RURU,
    0,
    CASC_LOCALE_PTBR | CASC_LOCALE_PTPT,
    CASC_LOCALE_ITIT,
};

uint32 ReadBuild(int locale)
{
    // include build info file also
    std::string filename = std::string("component.wow-") + localeNames[locale] + ".txt";
    //printf("Read %s file... ", filename.c_str());

    CASC::FileHandle dbcFile = CASC::OpenFile(CascStorage, filename.c_str(), CASC_LOCALE_ALL);
    if (!dbcFile)
    {
        printf("Locale %s not installed.\n", localeNames[locale]);
        return 0;
    }

    char buff[512];
    DWORD readBytes = 0;
    CASC::ReadFile(dbcFile, buff, 512, &readBytes);
    if (!readBytes)
    {
        printf("Fatal error: Not found %s file!\n", filename.c_str());
        exit(1);
    }

    std::string text = std::string(buff, readBytes);

    size_t pos = text.find("version=\"");
    size_t pos1 = pos + strlen("version=\"");
    size_t pos2 = text.find("\"", pos1);
    if (pos == text.npos || pos2 == text.npos || pos1 >= pos2)
    {
        printf("Fatal error: Invalid  %s file format!\n", filename.c_str());
        exit(1);
    }

    std::string build_str = text.substr(pos1, pos2 - pos1);

    int build = atoi(build_str.c_str());
    if (build <= 0)
    {
        printf("Fatal error: Invalid  %s file format!\n", filename.c_str());
        exit(1);
    }

    return build;
}

bool OpenCascStorage(int locale)
{
    try
    {
        boost::filesystem::path const storage_dir(boost::filesystem::canonical(input_path) / "Data");
        CascStorage = CASC::OpenStorage(storage_dir, WowLocaleToCascLocaleFlags[locale]);
        if (!CascStorage)
        {
            printf("error opening casc storage '%s' locale %s\n", storage_dir.string().c_str(), localeNames[locale]);
            return false;
        }

        return true;
    }
    catch (boost::filesystem::filesystem_error& error)
    {
        printf("error opening casc storage : %s\n", error.what());
        return false;
    }
}

// Local testing functions
bool FileExists(const char* file)
{
    if (FILE* n = fopen(file, "rb"))
    {
        fclose(n);
        return true;
    }
    return false;
}

void strToLower(char* str)
{
    while(*str)
    {
        *str=tolower(*str);
        ++str;
    }
}

bool ExtractSingleWmo(std::string& fname)
{
    // Copy files from archive

    char szLocalFile[1024];
    const char * plain_name = GetPlainName(fname.c_str());
    sprintf(szLocalFile, "%s/%s", szWorkDirWmo, plain_name);
    FixNameCase(szLocalFile, strlen(szLocalFile));
    FixNameSpaces(szLocalFile, strlen(szLocalFile));

    if (FileExists(szLocalFile))
        return true;

    int p = 0;
    // Select root wmo files
    char const* rchr = strrchr(plain_name, '_');
    if (rchr != NULL)
    {
        char cpy[4];
        memcpy(cpy, rchr, 4);
        for (int i = 0; i < 4; ++i)
        {
            int m = cpy[i];
            if (isdigit(m))
                p++;
        }
    }

    if (p == 3)
        return true;

    bool file_ok = true;
    printf("Extracting %s\n", fname.c_str());
    WMORoot froot(fname);
    if(!froot.open())
    {
        printf("Couldn't open RootWmo!!!\n");
        return true;
    }
    FILE *output = fopen(szLocalFile,"wb");
    if(!output)
    {
        printf("couldn't open %s for writing!\n", szLocalFile);
        return false;
    }
    froot.ConvertToVMAPRootWmo(output);
    int Wmo_nVertices = 0;
    //printf("root has %d groups\n", froot->nGroups);
    for (std::size_t i = 0; i < froot.groupFileDataIDs.size(); ++i)
    {
        std::string s = Trinity::StringFormat("FILE%08X.xxx", froot.groupFileDataIDs[i]);
        WMOGroup fgroup(s);
        if(!fgroup.open())
        {
            printf("Could not open all Group file for: %s\n", plain_name);
            file_ok = false;
            break;
        }

        Wmo_nVertices += fgroup.ConvertToVMAPGroupWmo(output, &froot, preciseVectorData);
    }

    fseek(output, 8, SEEK_SET); // store the correct no of vertices
    fwrite(&Wmo_nVertices,sizeof(int),1,output);
    fclose(output);

    // Delete the extracted file in the case of an error
    if (!file_ok)
        remove(szLocalFile);
    return true;
}

void ParsMapFiles()
{
    char fn[512];
    //char id_filename[64];
    char id[10];
    for (unsigned int i = 0; i < map_ids.size(); ++i)
    {
        sprintf(id, "%04u", map_ids[i].id);
        sprintf(fn,"World\\Maps\\%s\\%s.wdt", map_ids[i].name, map_ids[i].name);
        WDTFile WDT(fn,map_ids[i].name);
        if(WDT.init(id, map_ids[i].id))
        {
            printf("Processing Map %u\n[", map_ids[i].id);
            for (int x=0; x<64; ++x)
            {
                for (int y=0; y<64; ++y)
                {
                    if (ADTFile *ADT = WDT.GetMap(x,y))
                    {
                        //sprintf(id_filename,"%02u %02u %04u",x,y,map_ids[i].id);//!!!!!!!!!
                        ADT->init(map_ids[i].id, x, y);
                        delete ADT;
                    }
                }
                printf("#");
                fflush(stdout);
            }
            printf("]\n");
        }
    }
}

bool processArgv(int argc, char ** argv, const char *versionString)
{
    bool result = true;
    preciseVectorData = false;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp("-s", argv[i]) == 0)
        {
            preciseVectorData = false;
        }
        else if (strcmp("-d", argv[i]) == 0)
        {
            if ((i + 1) < argc)
            {
                input_path = boost::filesystem::path(argv[i + 1]);
                ++i;
            }
            else
            {
                result = false;
            }
        }
        else if (strcmp("-?", argv[1]) == 0)
        {
            result = false;
        }
        else if(strcmp("-l",argv[i]) == 0)
        {
            preciseVectorData = true;
        }
        else
        {
            result = false;
            break;
        }
    }

    if (!result)
    {
        printf("Extract %s.\n",versionString);
        printf("%s [-?][-s][-l][-d <path>]\n", argv[0]);
        printf("   -s : (default) small size (data size optimization), ~500MB less vmap data.\n");
        printf("   -l : large size, ~500MB more vmap data. (might contain more details)\n");
        printf("   -d <path>: Path to the vector data source folder.\n");
        printf("   -? : This message.\n");
    }

    return result;
}

//xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
// Main
//
// The program must be run with two command line arguments
//
// Arg1 - The source MPQ name (for testing reading and file find)
// Arg2 - Listfile name
//

int main(int argc, char ** argv)
{
    Trinity::Banner::Show("VMAP data extractor", [](char const* text) { printf("%s\n", text); }, nullptr);

    bool success = true;
    const char *versionString = "V4.03 2015_05";

    // Use command line arguments, when some
    if (!processArgv(argc, argv, versionString))
        return 1;

    // some simple check if working dir is dirty
    else
    {
        std::string sdir = std::string(szWorkDirWmo) + "/dir";
        std::string sdir_bin = std::string(szWorkDirWmo) + "/dir_bin";
        struct stat status;
        if (!stat(sdir.c_str(), &status) || !stat(sdir_bin.c_str(), &status))
        {
            printf("Your output directory seems to be polluted, please use an empty directory!\n");
            printf("<press return to exit>");
            char garbage[2];
            return scanf("%c", garbage);
        }
    }

    printf("Extract %s. Beginning work ....\n\n", versionString);
    //xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
    // Create the working directory
    if (mkdir(szWorkDirWmo
#if defined(__linux__) || defined(__APPLE__)
                    , 0711
#endif
                    ))
            success = (errno == EEXIST);

    int FirstLocale = -1;
    for (int i = 0; i < TOTAL_LOCALES; ++i)
    {
        if (i == LOCALE_none)
            continue;

        if (!OpenCascStorage(i))
            continue;

        FirstLocale = i;
        uint32 build = ReadBuild(i);
        if (!build)
        {
            CascStorage.reset();
            continue;
        }

        printf("Detected client build: %u\n\n", build);
        break;
    }

    if (FirstLocale == -1)
    {
        printf("FATAL ERROR: No locales defined, unable to continue.\n");
        return 1;
    }

    // Extract models, listed in GameObjectDisplayInfo.dbc
    ExtractGameobjectModels();

    //xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
    //map.dbc
    if (success)
    {
        printf("Read Map.dbc file... ");

        DB2CascFileSource source(CascStorage, "DBFilesClient\\Map.db2");
        DB2FileLoader db2;
        if (!db2.Load(&source, MapLoadInfo::Instance()))
        {
            printf("Fatal error: Invalid Map.db2 file format! %s\n", CASC::HumanReadableCASCError(GetLastError()));
            exit(1);
        }

        map_ids.resize(db2.GetRecordCount());
        std::unordered_map<uint32, uint32> idToIndex;
        for (uint32 x = 0; x < db2.GetRecordCount(); ++x)
        {
            DB2Record record = db2.GetRecord(x);
            map_ids[x].id = record.GetId();

            const char* map_name = record.GetString("Directory");
            size_t max_map_name_length = sizeof(map_ids[x].name);
            if (strlen(map_name) >= max_map_name_length)
            {
                printf("Fatal error: Map name too long!\n");
                exit(1);
            }

            strncpy(map_ids[x].name, map_name, max_map_name_length);
            map_ids[x].name[max_map_name_length - 1] = '\0';
            idToIndex[map_ids[x].id] = x;
        }

        for (uint32 x = 0; x < db2.GetRecordCopyCount(); ++x)
        {
            DB2RecordCopy copy = db2.GetRecordCopy(x);
            auto itr = idToIndex.find(copy.SourceRowId);
            if (itr != idToIndex.end())
            {
                map_id id;
                id.id = copy.NewRowId;
                strcpy(id.name, map_ids[itr->second].name);
                map_ids.push_back(id);
            }
        }

        printf("Done! (" SZFMTD " maps loaded)\n", map_ids.size());
        ParsMapFiles();
    }

    CascStorage.reset();

    printf("\n");
    if (!success)
    {
        printf("ERROR: Extract %s. Work NOT complete.\n   Precise vector data=%d.\nPress any key.\n", versionString, preciseVectorData);
        getchar();
    }

    printf("Extract %s. Work complete. No errors.\n", versionString);
    return 0;
}
