#include "Misc/AutomationTest.h"
#include "Core/GeoSOTMath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoSOTMathTest, "GeoSOTSystem.Core.MathTest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeoSOTMathTest::RunTest(const FString& Parameters)
{
    // ==========================================
    // 终极验证：北京世纪坛中心坐标
    // ==========================================
    // 坐标：39°54′37.01″N, 116°18′54.82″E，50m高
    // 转换十进制：39 + 54/60 + 37.01/3600 = 39.9102805555
    //           116 + 18/60 + 54.82/3600 = 116.315227777
    double TestLat = 39.9102805555;
    double TestLon = 116.315227777;
    double TestAlt = 50.0;

    // ---------------------------------------------------------
    // 路线 A 测试：验证是否生成国标字符串 (G001310322-230230-310312)
    // ---------------------------------------------------------
    FString RouteA_String = UGeoSOTMath::EncodeToGeoSOTString(TestLat, TestLon, TestAlt, 21, false);
	
    // 断言：字符串必须绝对等于论文参考值！
    TestEqual("Route A String Match", RouteA_String, TEXT("G001310322-230230-310312"));
    UE_LOG(LogTemp, Warning, TEXT("[Route A] National Standard String: %s"), *RouteA_String);

    // ---------------------------------------------------------
    // 路线 B 测试：验证底层 Morton 码的双向转换闭环
    // ---------------------------------------------------------
    int64 RouteB_Morton = UGeoSOTMath::EncodeToMorton3D(TestLat, TestLon, TestAlt, 21);
    TestTrueExpr(RouteB_Morton != 0);

    double OutLat, OutLon, OutAlt;
    UGeoSOTMath::DecodeFromMorton3D(RouteB_Morton, 21, OutLat, OutLon, OutAlt);

    // 验证解码回来的坐标，误差不能超过网格半径 (Level 21 约 30米)
    bool bLatMatch = FMath::IsNearlyEqual(OutLat, TestLat, 0.0002);
    bool bLonMatch = FMath::IsNearlyEqual(OutLon, TestLon, 0.0002);
    TestTrueExpr(bLatMatch && bLonMatch);

    UE_LOG(LogTemp, Warning, TEXT("[Route B] 3D Morton Machine Code: %lld"), RouteB_Morton);
    UE_LOG(LogTemp, Warning, TEXT("[Route B] Decoded Coordinates: Lat=%.5f, Lon=%.5f"), OutLat, OutLon);

    return true;
}