// HelpUtils.cpp

#include "StdAfx.h"

#include "HelpUtils.h"

#if defined(UNDER_CE) || defined(__MINGW32_VERSION)

void ShowHelpWindow(LPCSTR)
{
}

#else

/* USE_EXTERNAL_HELP creates new help process window for each HtmlHelp() call.
   HtmlHelp() call uses one window. */

#if defined(__MINGW32_VERSION) /* || defined(Z7_OLD_WIN_SDK) */
#define USE_EXTERNAL_HELP
#endif

// #define USE_EXTERNAL_HELP

#ifdef USE_EXTERNAL_HELP

#include "../../../Windows/ProcessUtils.h"
#include "../../../Windows/FileDir.h"
#include "../../../Windows/FileName.h"

#else
#include <HtmlHelp.h>
#endif

#include <shellapi.h>
#include <string.h>

#include "../../../Common/StringConvert.h"

#include "../../../Windows/DLL.h"
#include "../../../Windows/FileFind.h"

#include "../../MyVersion.h"

#define kHelpFileName "7-zip.chm::/"

/* Heptazip does not distribute 7-zip.chm (the help of 7-Zip): if the file is not next
   to the program, the help is the wiki of the project, in Italian if the language of
   Windows is Italian, in English otherwise. */

static const char *GetWikiPage(LPCSTR topic, bool italian)
{
  if (topic)
  {
    if (strstr(topic, "add.htm"))
      return italian ? "Quale-metodo-usare" : "Which-method-to-use";
    if (strstr(topic, "extract.htm"))
      return italian ? "Guida-rapida" : "Quick-start";
    if (strstr(topic, "benchmark.htm"))
      return italian ? "Prestazioni" : "Performance";
  }
  return italian ? "Home" : "Home-en";
}

#ifdef Z7_LANG
extern UString g_LangID;   // LangUtils: name of the language file ("it"), "-" English, empty: default
#endif

static bool HelpIsItalian()
{
  #ifdef Z7_LANG
  if (!g_LangID.IsEmpty())
    return g_LangID.IsPrefixedBy_Ascii_NoCase("it");
  #endif
  return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_ITALIAN;
}

static void ShowWiki(LPCSTR topic)
{
  const bool italian = HelpIsItalian();
  AString url (HZ_HOME_PAGE "/wiki/");
  url += GetWikiPage(topic, italian);
  ::ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

void ShowHelpWindow(LPCSTR topicFile)
{
  {
    FString chm = NWindows::NDLL::GetModuleDirPrefix();
    chm += "7-zip.chm";
    if (!NWindows::NFile::NFind::DoesFileExist_Raw(chm))
    {
      ShowWiki(topicFile);
      return;
    }
  }
  FString path = NWindows::NDLL::GetModuleDirPrefix();
  path += kHelpFileName;
  path += topicFile;
 #ifdef USE_EXTERNAL_HELP
  FString prog;

  #ifdef UNDER_CE
    prog = "\\Windows\\";
  #else
    if (!NWindows::NFile::NDir::GetWindowsDir(prog))
      return;
    NWindows::NFile::NName::NormalizeDirPathPrefix(prog);
  #endif
  prog += "hh.exe";

  UString params;
  params += '"';
  params += fs2us(path);
  params += '"';

  NWindows::CProcess process;
  const WRes wres = process.Create(fs2us(prog), params, NULL); // curDir);
  if (wres != 0)
  {
    /*
    HRESULT hres = HRESULT_FROM_WIN32(wres);
    ErrorMessageHRESULT(hres, imageName);
    return hres;
    */
  }
 #else
  // HWND hwnd = NULL;
  HtmlHelp(NULL, GetSystemString(fs2us(path)), HH_DISPLAY_TOPIC, 0);
 #endif
}

#endif
