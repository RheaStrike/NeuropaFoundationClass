// NFC_Demo 공용 인클루드. 모든 .cpp는 이 파일을 가장 먼저 포함한다.
#pragma once

#include "targetver.h"
#define WIN32_LEAN_AND_MEAN             // 거의 쓰지 않는 내용은 제외
#include <windows.h>
#include <windowsx.h>                   // GET_X_LPARAM 등
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>
#include <stdio.h>
#include <objbase.h>                    // IStream 등. 반드시 gdiplus.h보다 먼저
#include <gdiplus.h>                    // 9패치·레이어드 렌더러
#pragma comment(lib, "gdiplus.lib")
