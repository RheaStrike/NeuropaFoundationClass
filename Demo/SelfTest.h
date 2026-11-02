#pragma once

// 회귀 테스트 (CUI 빌드의 --selftest). 창 없이 돈다.
// 성공하면 "selftest ok"를 출력하고 0, 실패하면 원인을 출력하고 1.
class SelfTest {
public:
    static int Run();

private:
    // 그리기/기하 (App::Init 이후)
    static bool TestNinePatchGeometry(); // 9패치 영역/늘임 지정 기하
    static bool TestButtonSkinRegion();  // 버튼이 부모/기본 스킨에서 잘라 그림
    static bool TestParentChild();       // 다이얼로그가 죽으면 부모가 끊김
    static bool TestNinePatchCopy();     // NinePatch 값 복사
    static bool TestMapToSource();       // 9패치 역매핑
    static bool TestLabelSkin();         // 라벨: 부모 자리 스킨 / 자기 스킨
    static bool TestDefaultSkin();       // 기본 스킨 PNG 유효
    static bool TestButtonRegion();      // 버튼 영역이 한 패널 안
    // IDD 리소스 (DLGTEMPLATE 파서 / 컨트롤 등록부 / Bind)
    static bool TestDialogTemplate();    // 인메모리 템플릿: 개수·ID·위치·클래스 매핑·Bind
    static bool TestDialogTemplateSafety(); // 잘린 템플릿·모르는 클래스·쓰레기 데이터
    static bool TestDemoFormResource();  // 데모 .rc의 IDD_DEMO_FORM
};
