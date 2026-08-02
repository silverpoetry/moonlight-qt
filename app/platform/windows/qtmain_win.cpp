/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Windows main function of the Qt Toolkit.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**
****************************************************************************/

// Derived without functional changes from Qt 5.15.2's BSD-licensed
// src/winmain/qtmain_win.cpp. Keeping the entry point in this target ensures
// that it receives the same exploit-mitigation compiler flags as Moonlight.

#include <windows.h>
#include <shellapi.h>

#if defined(QT_NEEDS_QMAIN)
int qMain(int, char **);
#define main qMain
#else
extern "C" int main(int, char **);
#endif

static char *wideToMulti(unsigned int codePage, const wchar_t *wideString)
{
    const int required = WideCharToMultiByte(
        codePage,
        0,
        wideString,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr);
    char *result = new char[required];
    WideCharToMultiByte(
        codePage,
        0,
        wideString,
        -1,
        result,
        required,
        nullptr,
        nullptr);
    return result;
}

extern "C" int APIENTRY WinMain(
    HINSTANCE,
    HINSTANCE,
    LPSTR,
    int)
{
    int argc = 0;
    wchar_t **argvWide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvWide == nullptr) {
        return -1;
    }

    char **argv = new char *[argc + 1];
    for (int i = 0; i != argc; ++i) {
        argv[i] = wideToMulti(CP_ACP, argvWide[i]);
    }
    argv[argc] = nullptr;
    LocalFree(argvWide);

    const int exitCode = main(argc, argv);
    for (int i = 0; i != argc && argv[i] != nullptr; ++i) {
        delete[] argv[i];
    }
    delete[] argv;
    return exitCode;
}
