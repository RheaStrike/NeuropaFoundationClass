#pragma once
#include "CObject.h"
#include <functional>
#include <string>
#include <vector>

// 공개 헤더. asio를 include하지 않는다 (asio는 Network.cpp 한 곳에만).

namespace nfc {

typedef std::vector<unsigned char> Bytes;

// 호출 단위 실행 모드 (Socket::Create, HttpRequest::mode)
//  Delegate(기본): 전담 NetworkManager에 위임. 공용 io_context/스레드 풀, HTTP는 공용 세션+동시 요청 상한 큐.
//  Own: 그 창/컨트롤이 자기 것을 소유. 소켓은 자기 io_context+전용 스레드(소멸 시 stop+join),
//       HTTP는 요청 전용 WinHTTP 세션(요청이 끝나면 닫힘, 동시 상한 큐를 거치지 않음).
// 두 모드 모두 타임아웃·크기 제한·취소·소유자 소멸 정리·weak_ptr 규칙이 같고,
// 매니저가 등록을 관리하므로 Shutdown 때 함께 정리된다. 별도 추상화 계층은 없다.
enum class Mode { Delegate = 0, Own = 1 };

struct NetStats {
    int sockets;          // 살아 있는 소켓·리스너 수 (매니저 등록)
    int connections;      // 지금 연결된 소켓 수
    long pendingHandlers; // 아직 완료되지 않은 비동기 작업 수
    long exceptions;      // I/O 핸들러에서 잡은 예외 수 (해당 소켓만 닫힘)
    long totalConnects;   // 누적 연결 성공 수
};

// 네트워크 전담 매니저. 앱 전체에 io_context 하나 + 스레드 풀.
// App::Init에서 Start, App::Shutdown에서 Shutdown이 자동으로 불린다.
// Shutdown: 등록된 모든 소켓·타이머 close/cancel → 핸들러 정리(최대 3초) → io_context stop → join.
// 이후 새 소켓 생성·연결·RunAfter는 거부된다 (다시 Start하면 재개).
class NFC_API NetworkManager {
public:
    static void SetThreadCount(unsigned n); // Start 전에. 0 = 자동(2~4)
    static bool Start();
    static void Shutdown();
    static bool IsRunning();
    static NetStats GetStats();
    // ms 뒤에 I/O 스레드에서 f 실행 (UI를 직접 건드리지 말 것). 실행 중이 아니면 false.
    // Shutdown 시 취소된다.
    static bool RunAfter(int ms, std::function<void()> f);
};

} // namespace nfc
