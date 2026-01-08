#include "Tests/GeoSOTPathTester.h"
#include "Render/GeoSOTISMManager.h"
#include "Core/GeoSOTPathFinder.h"
#include "Core/GeoSOTUtils.h"
#include "Core/MyGeoSOTDatabase.h"
#include "DrawDebugHelpers.h" // <--- 必须引入
#include "Kismet/GameplayStatics.h"

AGeoSOTPathTester::AGeoSOTPathTester()
{
    PrimaryActorTick.bCanEverTick = true;
}

void AGeoSOTPathTester::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (bTriggerSearch)
    {
        bTriggerSearch = false;
        RunPathFindingTest();
    }
}

void AGeoSOTPathTester::DebugRoundTrip(FVector WorldPos)
{
    if (!GeoSOTManager || !GeoSOTManager->Georeference) return;

    //UE_LOG(LogTemp, Error, TEXT("========== GEO SOT DIAGNOSTIC START =========="));

    // [Step 1] 原始数据 (Unreal World)
    //UE_LOG(LogTemp, Warning, TEXT("1. Unreal World Pos: %s"), *WorldPos.ToString());

    // [Step 2] 经纬度转换 (Cesium)
    // 注意：Cesium 的 X通常是Longitude, Y是Latitude。必须确认这一点。
    FVector3d LLH = GeoSOTManager->Georeference->TransformUnrealPositionToLongitudeLatitudeHeight(FVector3d(WorldPos));
    double RealLon = LLH.X;
    double RealLat = LLH.Y;
    double RealH = LLH.Z;
    //UE_LOG(LogTemp, Warning, TEXT("2. Cesium Lat/Lon: Lat=%.6f, Lon=%.6f, H=%.2f"), RealLat, RealLon, RealH);

    // [Step 3] GeoSOT 网格化 (To Grid Index)
    int32 R, C;
    UGeoSOTUtils::LatLonToGeoSOT(RealLat, RealLon, 21, R, C);
    //UE_LOG(LogTemp, Warning, TEXT("3. Calculated Index: Row=%d, Col=%d"), R, C);

    // [Step 4] 编码生成 (To Code)
    // 假设高度层是 0 (排除高度干扰)
    int64 Code = UGeoSOTUtils::GenerateGeoSOTCode(R, C, 21, 0);
    // 打印二进制方便检查 (只看前几位)
    //UE_LOG(LogTemp, Warning, TEXT("4. Generated Code: %lld"), Code);

    // [Step 5] 解码回读 (From Code)
    int32 DecodedR, DecodedC, L, Alt;
    UGeoSOTUtils::DecodeGeoSOTCode(Code, DecodedR, DecodedC, L, Alt);
    //UE_LOG(LogTemp, Warning, TEXT("5. Decoded Index:  Row=%d, Col=%d (Should match Step 3)"), DecodedR, DecodedC);

    // [Step 6] 经纬度还原 (To Lat/Lon)
    double CheckLat, CheckLon;
    UGeoSOTUtils::GetGeoSOTCenter(DecodedR, DecodedC, 21, CheckLat, CheckLon);
    //UE_LOG(LogTemp, Warning, TEXT("6. Restored Lat/Lon: Lat=%.6f, Lon=%.6f"), CheckLat, CheckLon);

    // [Step 7] 误差分析
    double DiffLat = FMath::Abs(RealLat - CheckLat);
    double DiffLon = FMath::Abs(RealLon - CheckLon);
    // L21 级网格大概对应 0.000005 度左右
    //UE_LOG(LogTemp, Error, TEXT("7. Error Diff: dLat=%.8f, dLon=%.8f"), DiffLat, DiffLon);

    //UE_LOG(LogTemp, Error, TEXT("========== GEO SOT DIAGNOSTIC END =========="));
}



void AGeoSOTPathTester::RunPathFindingTest()
{
    // 0. 安全检查
    if (!GeoSOTManager)
    {
        GeoSOTManager = Cast<AGeoSOTISMManager>(UGameplayStatics::GetActorOfClass(GetWorld(), AGeoSOTISMManager::StaticClass()));
    }
    if (!GeoSOTManager)
    {
        // //UE_LOG(LogTemp, Error, TEXT("[Tester] GeoSOTManager NOT FOUND in scene!"));
        return;
    }

    UMyGeoSOTDatabase* DB = GetGameInstance()->GetSubsystem<UMyGeoSOTDatabase>();
    if (!DB)
    {
        // //UE_LOG(LogTemp, Error, TEXT("[Tester] Database Subsystem missing!"));
        return;
    }

    // //UE_LOG(LogTemp, Error, TEXT("========== 1. START POINT DIAGNOSTIC =========="));

    // 1. 获取起点 (摄像机位置)
    FVector CameraLoc = GetWorld()->GetFirstPlayerController()->PlayerCameraManager->GetCameraLocation();
    
    // 获取网格编码
    int64 StartCode = GeoSOTManager->GetGridCodeFromWorldLocation(CameraLoc);

    if (StartCode == -1)
    {
        // //UE_LOG(LogTemp, Error, TEXT("[Tester] Camera is out of valid range (Check Altitude/LatLon)."));
        return;
    }

    // 计算并打印起点偏差 (量化误差分析)
    FVector StartGridCenter = GeoSOTManager->GetWorldLocationFromGridCode(StartCode);
    float StartDeviation = FVector::Dist(CameraLoc, StartGridCenter);

    //UE_LOG(LogTemp, Warning, TEXT("Camera Real Pos: %s"), *CameraLoc.ToString());
    //UE_LOG(LogTemp, Warning, TEXT("Grid Center Pos: %s"), *StartGridCenter.ToString());
    //UE_LOG(LogTemp, Warning, TEXT("Quantization Error: %.2f meters"), StartDeviation / 100.0f);
    
    // 在画面上绘制对比点
    DrawDebugPoint(GetWorld(), CameraLoc, 15.0f, FColor::Red, false, 30.0f); // 红点：真实位置
    DrawDebugPoint(GetWorld(), StartGridCenter, 15.0f, FColor::Green, false, 30.0f); // 绿点：网格中心
    DrawDebugLine(GetWorld(), CameraLoc, StartGridCenter, FColor::Yellow, false, 30.0f, 0, 2.0f); // 连线

    //UE_LOG(LogTemp, Error, TEXT("========== 2. END POINT GENERATION =========="));

    // 2. 自动生成终点 (向东北方向寻找一个非障碍点)
    int32 R, C, L, Alt;
    UGeoSOTUtils::DecodeGeoSOTCode(StartCode, R, C, L, Alt);

    int64 EndCode = -1;
    // 尝试向 Row+50, Col+50 的方向移动，直到找到一个空气网格
    for (int i = 50; i < 150; i++) 
    {
        int64 TestCode = UGeoSOTUtils::GenerateGeoSOTCode(R + i, C + i, L, Alt+10);
        
        // 关键：必须确保终点本身不是障碍物，否则 A* 直接失败
        if (DB->IsGridWalkable(TestCode))
        {
            EndCode = TestCode;
            break;
        }
    }

    if (EndCode == -1)
    {
        //UE_LOG(LogTemp, Error, TEXT("[Tester] Could not find a valid Empty End Point nearby!"));
        return;
    }

    //UE_LOG(LogTemp, Log, TEXT("StartCode: %lld -> EndCode: %lld"), StartCode, EndCode);


    //UE_LOG(LogTemp, Error, TEXT("========== 3. A* EXECUTION & VERIFICATION =========="));

    // 3. 执行 A*
    TArray<int64> PathResult;
    bool bFound = UGeoSOTPathFinder::FindPath(this, StartCode, EndCode, PathResult);

    if (bFound)
    {
        //UE_LOG(LogTemp, Warning, TEXT("Path Found! Nodes: %d. Verifying validity..."), PathResult.Num());

        // 4. 路径全量反查 (验证是否存在穿墙 Bug)
        for (int i = 0; i < PathResult.Num(); i++)
        {
            int64 Code = PathResult[i];
            
            // A. 获取位置
            FVector NodePos = GeoSOTManager->GetWorldLocationFromGridCode(Code);
            
            // B. 数据库反查 (Database Truth Check)
            // IsGridWalkable 返回 false 表示是障碍
            bool bIsObstacleInDB = !DB->IsGridWalkable(Code); 
            
            // C. 状态判定
            FString StatusStr;
            FColor DebugColor;

            if (bIsObstacleInDB) 
            {
                StatusStr = TEXT("[ERROR: WALL]"); // 严重 Bug：A* 选了墙
                DebugColor = FColor::Red; 
            }
            else 
            {
                StatusStr = TEXT("[OK: Air]");     // 正常：A* 选了空气
                DebugColor = FColor::Green;
            }

            // D. 打印详细日志
            // 只打印前5个、后5个和所有错误节点，避免日志刷屏 (或者你可以全部打印)
            if (i < 5 || i > PathResult.Num() - 5 || bIsObstacleInDB)
            {
                int32 tR, tC, tL, tAlt;
                UGeoSOTUtils::DecodeGeoSOTCode(Code, tR, tC, tL, tAlt);
                
                // if (bIsObstacleInDB)
                // {
                //     //UE_LOG(LogTemp, Error, 
                //     TEXT("Node %d: %s Code=%lld | Idx=(%d,%d,%d) | Pos=%s"), 
                //     i, *StatusStr, Code, tR, tC, tAlt, *NodePos.ToString());
                // }
                // else
                // {
                //     //UE_LOG(LogTemp, Warning, 
                //     TEXT("Node %d: %s Code=%lld | Idx=(%d,%d,%d) | Pos=%s"), 
                //     i, *StatusStr, Code, tR, tC, tAlt, *NodePos.ToString());
                // }
                
            }

            // E. 视觉辅助：每个节点画一个小点
            DrawDebugPoint(GetWorld(), NodePos, 10.0f, DebugColor, false, 30.0f);
        }

        // 5. 渲染实体路径 (黄色方块)
        GeoSOTManager->DrawPath(PathResult);
    }
    else
    {
        //UE_LOG(LogTemp, Error, TEXT("A* Failed to find path!"));
    }

    //UE_LOG(LogTemp, Error, TEXT("========== DIAGNOSTIC COMPLETE =========="));
}

void AGeoSOTPathTester::DrawGridDebugInfo(int64 GridCode, FString Label, FColor Color)
{
    if (!GeoSOTManager) return;

    // 1. 解码索引
    int32 R, C, L, Alt;
    UGeoSOTUtils::DecodeGeoSOTCode(GridCode, R, C, L, Alt);

    // 2. 获取世界坐标中心 (逆变换)
    FVector CenterLoc = GeoSOTManager->GetWorldLocationFromGridCode(GridCode);

    // 3. 获取经纬度 (用于验证)
    double Lat, Lon;
    UGeoSOTUtils::GetGeoSOTCenter(R, C, L, Lat, Lon);

    // === 绘制 1: 物理位置球体 ===
    // 半径设为 0.5米，足够看清位置
    DrawDebugSphere(GetWorld(), CenterLoc, 50.0f, 12, Color, false, 20.0f, 0, 2.0f);
    
    // === 绘制 2: 连线到地面 (帮助判断高度是否正确) ===
    DrawDebugLine(GetWorld(), CenterLoc, CenterLoc - FVector(0,0,500), Color, false, 20.0f, 0, 1.0f);

    // === 绘制 3: 详细文字信息 ===
    // 格式化字符串
    FString DebugText = FString::Printf(
        TEXT("[%s]\nCode: %lld\nIndex: R=%d, C=%d, Alt=%d\nLat: %.6f\nLon: %.6f\nUE: %s"), 
        *Label, 
        GridCode, 
        R, C, Alt,
        Lat, Lon,
        *CenterLoc.ToString()
    );

    // 绘制文字 (位置在球体上方一点)
    DrawDebugString(GetWorld(), CenterLoc + FVector(0,0,100), DebugText, nullptr, Color, 20.0f, true, 1.5f);
}