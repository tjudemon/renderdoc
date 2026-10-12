/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "hooks.h"
#include "common/common.h"

static rdcarray<LibraryHook *> &LibList()
{
  static rdcarray<LibraryHook *> libs;
  return libs;
}

LibraryHook::LibraryHook()
{
  LibList().push_back(this);
}

void LibraryHooks::RegisterHooks()
{
  BeginHookRegistration();

#if ENABLED(RDOC_WIN32)
  // Experimental diagnostics (exp I9/I10):
  //  GFXCAP_NO_SUBSYSTEMS=1   skips every subsystem's RegisterHooks (engine only).
  //  GFXCAP_SUBSYSTEMS="a,b"  whitelist - runs only the named subsystems' RegisterHooks.
  //  I9 proved the trigger lives in the subsystem registration loop, so this lets us
  //  bisect which subsystem's pure registration is flagged by ACE.
  wchar_t noSubBuf[8] = {};
  bool noSubsystems = false;
  if(GetEnvironmentVariableW(L"GFXCAP_NO_SUBSYSTEMS", noSubBuf, 8) > 0)
    noSubsystems = (noSubBuf[0] == L'1');

  rdcarray<rdcstr> allowList;
  char allowBuf[256] = {};
  int allowLen = GetEnvironmentVariableA("GFXCAP_SUBSYSTEMS", allowBuf, 256);
  if(allowLen > 0)
  {
    rdcstr allow = allowBuf;
    int start = 0;
    while(start < (int)allow.size())
    {
      int comma = allow.indexOf(',', start);
      if(comma < 0)
        comma = (int)allow.size();
      rdcstr token = allow.substr(start, comma - start).trimmed();
      if(!token.empty())
        allowList.push_back(token);
      start = comma + 1;
    }
  }

  const bool filtered = noSubsystems || !allowList.empty();

  if(filtered)
  {
    RDCLOG("Subsystem filter: NO_SUBSYSTEMS=%d allow=[%s]", noSubsystems ? 1 : 0, allowBuf);
    for(LibraryHook *lib : LibList())
    {
      rdcstr name = lib->GetName();
      RDCLOG("  - subsystem %s", name.c_str());
      if(!noSubsystems && allowList.contains(name))
        lib->RegisterHooks();
    }
  }
  else
#endif
  {
    for(LibraryHook *lib : LibList())
      lib->RegisterHooks();
  }

  EndHookRegistration();
}

void LibraryHooks::RemoveHookCallbacks()
{
  for(LibraryHook *lib : LibList())
    lib->RemoveHooks();
}

void LibraryHooks::OptionsUpdated()
{
  for(LibraryHook *lib : LibList())
    lib->OptionsUpdated();
}
