#include "Simulation/FormationMission.h"

// --- 模块引用 ---
#include "Render/GeoSOTISMManager.h"
#include "Core/GeoSOTPathFinder.h"
#include "Core/GeoSOTUtils.h"
#include "CesiumGeoreference.h"

// --- 引擎工具 ---
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"
#include "Networking.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "EngineUtils.h" 

AFormationMission::AFormationMission()
{
    PrimaryActorTick.bCanEverTick = true;

    // --- [配置] 刚性编队偏移量 ---
    // 基础飞行高度 (CM) -> 5米
    float FlyH = 100.0f; 

    // 定义 2x2 编队，间距 3米 (300cm)
    // 假设中心是 (150, 150), 这样 UAV_0(0,0) 和 UAV_3(300,300) 对称分布
    DroneOffsets.Add("UAV_0", FVector(0,     0,     FlyH));
    DroneOffsets.Add("UAV_1", FVector(300.0, 0,     FlyH));
    DroneOffsets.Add("UAV_2", FVector(0,     300.0, FlyH));
    DroneOffsets.Add("UAV_3", FVector(300.0, 300.0, FlyH));
}

void AFormationMission::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // --- 实时绘制拖尾 (只画最近 5 秒的轨迹，保持清晰) ---
    if (!bMissionRunning) return;

    for (TActorIterator<APawn> It(GetWorld()); It; ++It)
    {
        APawn* P = *It;
        FString Name = P->GetName();
        
        // 简单匹配 AirSim 生成的 Pawn
        if (Name.Contains("FlyingPawn") || Name.Contains("UAV"))
        {
            for (auto& Elem : DroneOffsets)
            {
                FString Key = Elem.Key; 
                // 匹配 UAV_0 等后缀
                if (Name.Contains(Key.RightChop(4))) 
                {
                    FVector CurrentPos = P->GetActorLocation();
                    if (LastPositions.Contains(Key))
                    {
                        FVector LastPos = LastPositions[Key];
                        if (FVector::DistSquared(LastPos, CurrentPos) > 100.0f) 
                        {
                            // 根据 ID 分配颜色
                            int32 ID = FCString::Atoi(*Key.RightChop(4));
                            FColor Col = FLinearColor::White.ToFColor(true);
                            if (ID==0) Col=FColor::Red; 
                            else if (ID==1) Col=FColor::Green; 
                            else if (ID==2) Col=FColor::Blue; 
                            else if (ID==3) Col=FColor::Yellow;

                            // 绘制拖尾：生命周期 5秒，宽度 3.0
                            DrawDebugLine(GetWorld(), LastPos, CurrentPos, Col, false, 5.0f, 0, 3.0f);
                            LastPositions[Key] = CurrentPos;
                        }
                    }
                }
            }
        }
    }
}

AActor* AFormationMission::FindActorByTag(FName Tag)
{
    TArray<AActor*> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), Tag, FoundActors);
    if (FoundActors.Num() > 0) return FoundActors[0];
    return nullptr;
}

void AFormationMission::RunFormationMission()
{
    UE_LOG(LogTemp, Warning, TEXT("========== FORMATION MISSION START (FIXED COORDINATES) =========="));

    // 1. 获取管理器引用
    if (!ISMManager) ISMManager = Cast<AGeoSOTISMManager>(UGameplayStatics::GetActorOfClass(GetWorld(), AGeoSOTISMManager::StaticClass()));
    if (!GeoRef) GeoRef = Cast<ACesiumGeoreference>(UGameplayStatics::GetActorOfClass(GetWorld(), ACesiumGeoreference::StaticClass()));
    
    if (!ISMManager || !GeoRef) 
    {
        UE_LOG(LogTemp, Error, TEXT("Missing ISMManager or GeoRef!"));
        return;
    }

    // 2. 确定编队的【几何中心】起点和终点
    AActor* StartActor = FindActorByTag(FName("Airport_Start"));
    AActor* TargetActor = FindActorByTag(FName("Airport_End_0")); // 所有飞机的大方向
    
    if (!StartActor || !TargetActor) 
    {
        UE_LOG(LogTemp, Error, TEXT("Missing Airport_Start or Airport_End_0 tags!"));
        return;
    }

    // 获取世界原点 (PlayerStart位置)
    FVector OriginLoc = StartActor->GetActorLocation();

    // 设定中心路径的起终点 (稍微抬高 Z 轴)
    // 假设编队中心相对于原点偏移 (150, 150)，位于 UAV_0 和 UAV_3 中间
    float PathHeight = 20.0f;
    FVector CenterStart = OriginLoc + FVector(150, 150, PathHeight); 
    FVector CenterEnd   = TargetActor->GetActorLocation() + FVector(150, 150, PathHeight);

    // 3. 计算【领航路径】(只算一次)
    TArray<FVector> CenterPathPoints;
    TArray<int64> CenterPathCodes;
    
    // 传入空 Set，因为这是唯一的路径，不需要避让别人
    TSet<int64> EmptySet; 
    bool bPathFound = CalculatePathForCenter(CenterStart, CenterEnd, CenterPathPoints, CenterPathCodes, EmptySet);

    if (!bPathFound)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to generate Center Path!"));
        return;
    }

    // 可视化 ISM Path (绘制红/绿网格)
    ISMManager->DrawPath(CenterPathCodes); 
    UE_LOG(LogTemp, Log, TEXT("ISM Path Visualized: %d grids"), CenterPathCodes.Num());

    // 4. 为每架无人机生成偏移路径并打包 JSON
    FString JsonDrones = "";
    int32 SuccessCount = 0;
    
    LastPositions.Empty();
    bMissionRunning = true;

    for (auto& Elem : DroneOffsets)
    {
        FString DroneName = Elem.Key;     // e.g. "UAV_0"
        FVector Offset    = Elem.Value;   // e.g. (300, 0, 500)

        // 计算该无人机相对于【中心路径】的偏移量
        // CenterStart 是 (Origin + 150, 150, 500)
        // MyStart 是 (Origin + Offset)
        // RelativeToCenter = MyStart - CenterStart = Offset - (150, 150, 500)
        FVector RelativeToCenter = Offset - FVector(150, 150, PathHeight); 

        // 记录初始位置 (用于 UE 拖尾可视化的起点)
        FVector RealStartPos = OriginLoc + Offset;
        LastPositions.Add(DroneName, RealStartPos);

        FString PathJson = "[";

        // --- [核心修复 1] 动态计算安全起飞点 ---
        // 之前写死的 {0,0,-5} 会导致所有飞机飞向中心相撞。
        // 现在我们让每架飞机在其 Offset 位置垂直起飞。
        // AirSim 坐标 = Offset (cm) / 100 -> 米
        float StartX_m = Offset.X / 100.0f;
        float StartY_m = Offset.Y / 100.0f;
        
        float TakeoffZ_m = -2.0f; // 默认保底 2米
        if (CenterPathPoints.Num() > 0)
        {
            // 计算该无人机第一个目标点的实际高度
            FVector FirstTarget = CenterPathPoints[0] + RelativeToCenter;
            FVector FirstRel = FirstTarget - OriginLoc;
            TakeoffZ_m = -FirstRel.Z / 100.0f; // UE(正) -> AirSim(负)
        }
        // Z = -5.0 (向上飞5米)
        PathJson += FString::Printf(TEXT("{\"x\":%.3f, \"y\":%.3f, \"z\":%.3f},"), StartX_m, StartY_m, TakeoffZ_m);

        // 生成跟随路径
        for(int i = 0; i < CenterPathPoints.Num(); i++)
        {
            // 刚性跟随：目标点 = 中心路径点 + 相对中心的偏移
            FVector TargetPos = CenterPathPoints[i] + RelativeToCenter;
            
            // --- [核心修复 2] 坐标相对性修正 ---
            // 之前的错误： Rel = TargetPos - RealStartPos (相对于自己)
            // 修正后：     Rel = TargetPos - OriginLoc  (相对于世界原点)
            // 这样 UAV_1 (at 300) 收到 (300) 的指令，就会保持在 x=300
            FVector Rel = TargetPos - OriginLoc;
            
            float X_m = Rel.X / 100.0f;
            float Y_m = Rel.Y / 100.0f;
            float Z_m = -Rel.Z / 100.0f; // UE Z-Up -> AirSim Z-Down
            
            PathJson += FString::Printf(TEXT("{\"x\":%.3f, \"y\":%.3f, \"z\":%.3f}"), X_m, Y_m, Z_m);
            if (i < CenterPathPoints.Num() - 1) PathJson += ",";
        }
        PathJson += "]";

        FString DroneEntry = FString::Printf(TEXT("\"%s\": { \"path\": %s }"), *DroneName, *PathJson);
        if (SuccessCount > 0) JsonDrones += ",";
        JsonDrones += DroneEntry;
        SuccessCount++;
    }

    // 5. 发送最终 JSON
    if (SuccessCount > 0)
    {
        FString FinalJson = FString::Printf(TEXT("{ \"mission_type\": \"formation\", \"drones\": { %s } }"), *JsonDrones);
        SendMissionToPython(FinalJson);
    }
}

// 专门为中心点算路的辅助函数
bool AFormationMission::CalculatePathForCenter(
    FVector StartPos, 
    FVector EndPos, 
    TArray<FVector>& OutPathPoints,
    TArray<int64>& OutPathCodes,
    TSet<int64>& OccupiedSet)
{
    if (!GeoRef) return false;

    int32 Level = 21; 
    double HSpan = UGeoSOTUtils::GetHeightSpan(Level);

    // 1. 转 GeoSOT 编码
    FVector3d StartLLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(FVector3d(StartPos));
    int32 R1, C1;
    UGeoSOTUtils::LatLonToGeoSOT(StartLLH.Y, StartLLH.X, Level, R1, C1);
    
    // 基于真实高度计算索引
    int32 Alt1 = (int32)(StartLLH.Z / HSpan);
    if(Alt1<0) Alt1=0;
    int64 StartCode = UGeoSOTUtils::GenerateGeoSOTCode(R1, C1, Level, Alt1);

    FVector3d EndLLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(FVector3d(EndPos));
    int32 R2, C2;
    UGeoSOTUtils::LatLonToGeoSOT(EndLLH.Y, EndLLH.X, Level, R2, C2);
    
    int32 Alt2 = (int32)(EndLLH.Z / HSpan);
    if(Alt2<0) Alt2=0;
    int64 EndCode = UGeoSOTUtils::GenerateGeoSOTCode(R2, C2, Level, Alt2);

    // 2. A* 寻路
    bool bFound = UGeoSOTPathFinder::FindPath(this, StartCode, EndCode, OutPathCodes, OccupiedSet);
    if (!bFound) return false;

    // 3. 还原为坐标点
    OutPathPoints.Empty();
    double MinH = 300.0; // 最小高度钳制 (3米)

    for (int64 Code : OutPathCodes)
    {
        int32 r, c, l, h;
        UGeoSOTUtils::DecodeGeoSOTCode(Code, r, c, l, h);
        double lat, lon;
        UGeoSOTUtils::GetGeoSOTCenter(r, c, l, lat, lon);
        
        double height = h * HSpan;
        if(height < MinH) height = MinH;

        FVector3d LLH(lon, lat, height);
        FVector Pos = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(LLH);
        OutPathPoints.Add(Pos);
    }
    return true;
}

void AFormationMission::SendMissionToPython(FString JsonPayload)
{
    ISocketSubsystem* SocketSubs = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    FSocket* Socket = SocketSubs->CreateSocket(NAME_Stream, TEXT("SwarmConnector"), false);
    if (!Socket) return;

    TSharedRef<FInternetAddr> Addr = SocketSubs->CreateInternetAddr();
    bool bValid;
    Addr->SetIp(TEXT("127.0.0.1"), bValid);
    Addr->SetPort(8888);
    
    if (Socket->Connect(*Addr))
    {
        TCHAR* SerializedChar = JsonPayload.GetCharArray().GetData();
        FTCHARToUTF8 Convert(SerializedChar);
        int32 Sent = 0;
        Socket->Send((uint8*)Convert.Get(), Convert.Length(), Sent);
        Socket->Close();
        UE_LOG(LogTemp, Warning, TEXT("[Mission] Sent %d bytes to Python."), Sent);
    }
    SocketSubs->DestroySocket(Socket);
}