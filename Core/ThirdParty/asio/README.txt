standalone asio 1.30.2 (헤더 전용, Boost 불필요)
출처: https://github.com/chriskohlhoff/asio  태그 asio-1-30-2 의 asio/include 내용
      (asio.hpp + asio/ 폴더). 라이선스: Boost Software License 1.0 (LICENSE_1_0.txt)

사용: NFC_Core / NFC_Core_Static 의 모든 구성에서 NFC_USE_ASIO가 정의되고
이 폴더가 Additional Include Directories에 들어 있다.
asio를 include하는 곳은 Core/Network.cpp 한 곳뿐이다 (공개 헤더는 asio를 노출하지 않음).
ASIO_STANDALONE, ASIO_NO_DEPRECATED로 쓴다. 코루틴은 쓰지 않는다.
버전을 올릴 때는 이 폴더의 asio.hpp/asio/ 를 통째로 바꾸고 이 파일의 버전을 고칠 것.
