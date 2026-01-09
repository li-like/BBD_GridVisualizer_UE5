#include "Simulation/MultiSwarmMission.h"

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

// 构造函数
AMultiSwarmMission::AMultiSwarmMission()
{
    PrimaryActorTick.bCanEverTick = false; // 不需要 Tick，事件驱动

    // --- [配置] 无人机阵列偏移量 ---
    // 必须与 AirSim/settings.json 中的 Vehicles 配置完全一致
    // 假设 Airport_Start 圆盘位于地面，无人机起飞高度设为 200cm (2米)
    float FlyH = 500.0f; 

    // AirSim (米) -> UE (厘米)
    // UAV_0: (0, 0)
    DroneOffsets.Add("UAV_0", FVector(0,     0,     FlyH));
    // UAV_1: (3, 0)
    DroneOffsets.Add("UAV_1", FVector(300.0, 0,     FlyH));
    // UAV_2: (0, 3)
    DroneOffsets.Add("UAV_2", FVector(0,     300.0, FlyH));
    // UAV_3: (3, 3)
    DroneOffsets.Add("UAV_3", FVector(300.0, 300.0, FlyH));
}

void AMultiSwarmMission::BeginPlay()
{
    Super::BeginPlay();
    
    // 自动获取场景中的关键组件
    ISMManager = Cast<AGeoSOTISMManager>(UGameplayStatics::GetActorOfClass(GetWorld(), AGeoSOTISMManager::StaticClass()));
    GeoRef = Cast<ACesiumGeoreference>(UGameplayStatics::GetActorOfClass(GetWorld(), ACesiumGeoreference::StaticClass()));

    if (!ISMManager) UE_LOG(LogTemp, Error, TEXT("[MultiMission] Warning: GeoSOTISMManager missing!"));
    if (!GeoRef) UE_LOG(LogTemp, Error, TEXT("[MultiMission] Warning: CesiumGeoreference missing!"));
}

AActor* AMultiSwarmMission::FindActorByTag(FName Tag)
{
    TArray<AActor*> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), Tag, FoundActors);
    if (FoundActors.Num() > 0) return FoundActors[0];
    return nullptr;
}

void AMultiSwarmMission::RunMission()
{
    UE_LOG(LogTemp, Warning, TEXT("========== SWARM MISSION START =========="));

    // 如果是在编辑器模式下点击按钮，可能 BeginPlay 还没运行，需要手动获取引用
    if (!ISMManager) ISMManager = Cast<AGeoSOTISMManager>(UGameplayStatics::GetActorOfClass(GetWorld(), AGeoSOTISMManager::StaticClass()));
    if (!GeoRef) GeoRef = Cast<ACesiumGeoreference>(UGameplayStatics::GetActorOfClass(GetWorld(), ACesiumGeoreference::StaticClass()));

    if (!ISMManager || !GeoRef) 
    {
        UE_LOG(LogTemp, Error, TEXT("[MultiMission] Manager or GeoRef is NULL! Cannot run mission."));
        return;
    }

    // 1. 找到起始机场中心 (作为物理基准点)
    AActor* StartAirport = FindActorByTag(FName("Airport_Start"));
    if (!StartAirport)
    {
        UE_LOG(LogTemp, Error, TEXT("[MultiMission] FAILED: Could not find actor with tag 'Airport_Start'"));
        return;
    }
    FVector OriginLoc = StartAirport->GetActorLocation();

    // 准备 JSON 字符串构建器
    FString JsonDrones = "";
    int32 SuccessCount = 0;
    
    // [新增] 全局占用表：记录所有已经规划好的路径节点
    TSet<int64> GlobalOccupiedSet;

    // 2. 遍历所有定义的无人机，分别执行任务
    for (auto& Elem : DroneOffsets)
    {
        FString DroneName = Elem.Key;     // e.g., "UAV_0"
        FVector Offset    = Elem.Value;   // e.g., (300, 0, 200)

        // A. 计算该无人机的【实际起点】(UE 世界坐标)
        FVector MyStartPos = OriginLoc + Offset;

        // B. 找到对应的【终点机场】
        // 解析 UAV_0 -> 0，然后找 Airport_End_0
        FString IndexStr = DroneName.RightChop(4); // 从 "UAV_" 后截取
        FName EndTag = FName(*FString::Printf(TEXT("Airport_End_%s"), *IndexStr));
        
        AActor* EndAirport = FindActorByTag(EndTag);
        if (!EndAirport)
        {
            UE_LOG(LogTemp, Warning, TEXT("[MultiMission] Skip %s: No End Airport found with tag %s"), *DroneName, *EndTag.ToString());
            continue;
        }
        
        // 终点稍微抬高一点，防止算出地面以下的坐标
        FVector MyEndPos = EndAirport->GetActorLocation() + FVector(0, 0, 200);

        // C. 执行寻路
        TArray<FVector> PathPoints;
        if (CalculatePathForDrone(DroneName, MyStartPos, MyEndPos, PathPoints,GlobalOccupiedSet))
        {
            // D. 拼装该无人机的 JSON 片段
            // 格式: "UAV_0": { "path": [ {"x":.., "y":.., "z":..}, ... ] }
            
            FString PathJson = "[";
            for(int i = 0; i < PathPoints.Num(); i++)
            {
                // [核心] 坐标系转换: UE世界坐标 -> AirSim相对坐标
                // 必须计算相对于【该无人机出生点】的偏移量
                FVector Rel = PathPoints[i] - MyStartPos;
                
                // 转换规则: 
                // 1. 厘米 -> 米 (/100)
                // 2. Z轴取反 (UE Up -> AirSim Down)
                float X_m = Rel.X / 100.0f;
                float Y_m = Rel.Y / 100.0f;
                float Z_m = -Rel.Z / 100.0f; 

                // 保留3位小数
                PathJson += FString::Printf(TEXT("{\"x\":%.3f, \"y\":%.3f, \"z\":%.3f}"), X_m, Y_m, Z_m);
                
                if (i < PathPoints.Num() - 1) PathJson += ",";
            }
            PathJson += "]";

            // 拼接到总 JSON 中
            FString DroneEntry = FString::Printf(TEXT("\"%s\": { \"path\": %s }"), *DroneName, *PathJson);
            
            if (SuccessCount > 0) JsonDrones += ",";
            JsonDrones += DroneEntry;
            SuccessCount++;
            
            UE_LOG(LogTemp, Log, TEXT("[MultiMission] %s Path Calculated: %d nodes"), *DroneName, PathPoints.Num());
        }
    }

    // 3. 打包并发送最终 JSON
    if (SuccessCount > 0)
    {
        FString FinalJson = FString::Printf(TEXT("{ \"mission_type\": \"swarm\", \"drones\": { %s } }"), *JsonDrones);
        SendMissionToPython(FinalJson);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[MultiMission] No paths generated. Check start/end positions."));
    }
    
    UE_LOG(LogTemp, Warning, TEXT("========== SWARM MISSION END =========="));
}

bool AMultiSwarmMission::CalculatePathForDrone(FString DroneName, FVector StartPos, FVector EndPos, TArray<FVector>& OutPathPoints,TSet<int64>& GlobalOccupiedSet)
{
    // 1. 将 世界坐标 转换为 GeoSOT 编码
    if (!GeoRef) return false;

    // --- [核心修正] 基于真实物理高度计算索引 ---
    
    // 获取当前层级的高度跨度
    int32 TargetLevel = 21; 
    double HSpan = UGeoSOTUtils::GetHeightSpan(TargetLevel);

    // ================== 起点处理 ==================
    // UE World -> Cesium (Lon, Lat, Height_Meters)
    FVector3d StartLLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(FVector3d(StartPos));
    
    int32 R1, C1;
    UGeoSOTUtils::LatLonToGeoSOT(StartLLH.Y, StartLLH.X, TargetLevel, R1, C1);
    
    // Height_Meters / HSpan = Index
    int32 StartAltLayer = (int32)(StartLLH.Z / HSpan);
    if (StartAltLayer < 0) StartAltLayer = 0; 

    int64 StartCode = UGeoSOTUtils::GenerateGeoSOTCode(R1, C1, TargetLevel, StartAltLayer);

    // ================== 终点处理 ==================
    FVector3d EndLLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(FVector3d(EndPos));
    
    int32 R2, C2;
    UGeoSOTUtils::LatLonToGeoSOT(EndLLH.Y, EndLLH.X, TargetLevel, R2, C2);
    
    int32 EndAltLayer = (int32)(EndLLH.Z / HSpan);
    if (EndAltLayer < 0) EndAltLayer = 0;

    int64 EndCode = UGeoSOTUtils::GenerateGeoSOTCode(R2, C2, TargetLevel, EndAltLayer);

    // ---------------- A* 寻路 ----------------

    TArray<int64> PathCodes;
    bool bFound = UGeoSOTPathFinder::FindPath(this, StartCode, EndCode, PathCodes,GlobalOccupiedSet);

    if (!bFound || PathCodes.Num() == 0) 
    {
        UE_LOG(LogTemp, Error, TEXT("[MultiMission] A* Failed for %s. StartAlt: %d, EndAlt: %d"), 
            *DroneName, StartAltLayer, EndAltLayer);
        return false;
    }

    // 3. 可视化配置
    int32 ID = FCString::Atoi(*DroneName.RightChop(4)); 
    FLinearColor DebugColor;
    switch(ID)
    {
        case 0: DebugColor = FLinearColor::Red; break;
        case 1: DebugColor = FLinearColor::Green; break;
        case 2: DebugColor = FLinearColor::Blue; break;
        case 3: DebugColor = FLinearColor::Yellow; break;
        default: DebugColor = FLinearColor::White; break;
    }

    // 4. 解析路径编码回世界坐标
    OutPathPoints.Empty();
    
    for (int i = 0; i < PathCodes.Num(); i++)
    {
        
        int64 Code = PathCodes[i];
        
        GlobalOccupiedSet.Add(Code);
        
        // 解码
        int32 r, c, l, h;
        UGeoSOTUtils::DecodeGeoSOTCode(Code, r, c, l, h);
        
        // 获取中心经纬度
        double lat, lon;
        UGeoSOTUtils::GetGeoSOTCenter(r, c, l, lat, lon);
        
        // 获取高度 (还原：Index * Span)
        double h_span = UGeoSOTUtils::GetHeightSpan(l);
        double height = h * h_span;
        
        // 转回 UE 坐标
        FVector3d TargetLLH(lon, lat, height);
        // 使用标准的转换函数名称
        FVector WorldPos = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(TargetLLH);
        
        OutPathPoints.Add(WorldPos);

        // 绘制路径点
        DrawDebugPoint(GetWorld(), WorldPos, 50.0f, DebugColor.ToFColor(true), false, -1);
    }
    
    // 绘制连线
    if (OutPathPoints.Num() > 1)
    {
        for (int i = 0; i < OutPathPoints.Num() - 1; i++)
        {
            DrawDebugLine(GetWorld(), OutPathPoints[i], OutPathPoints[i+1], DebugColor.ToFColor(true), false, 60.0f, 0, 3.0f);
        }
    }

    return true;
}

void AMultiSwarmMission::SendMissionToPython(FString JsonPayload)
{
    // 创建 Socket
    ISocketSubsystem* SocketSubs = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    FSocket* Socket = SocketSubs->CreateSocket(NAME_Stream, TEXT("SwarmConnector"), false);
    
    if (!Socket) return;

    TSharedRef<FInternetAddr> Addr = SocketSubs->CreateInternetAddr();
    bool bValid;
    // 目标: 本地 Python 脚本监听的 8888 端口
    Addr->SetIp(TEXT("127.0.0.1"), bValid);
    Addr->SetPort(8888);
    
    UE_LOG(LogTemp, Log, TEXT("[MultiMission] Connecting to Python Server..."));

    if (Socket->Connect(*Addr))
    {
        // 转换数据为 UTF8
        TCHAR* SerializedChar = JsonPayload.GetCharArray().GetData();
        FTCHARToUTF8 Convert(SerializedChar);
        
        int32 Sent = 0;
        bool bSuccess = Socket->Send((uint8*)Convert.Get(), Convert.Length(), Sent);
        
        if (bSuccess)
        {
            UE_LOG(LogTemp, Warning, TEXT("[MultiMission] SUCCESS: Sent swarm plan (%d bytes) to Python."), Sent);
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("[MultiMission] Failed to send data."));
        }
        
        // 发送完立即关闭
        Socket->Close();
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[MultiMission] Connection Failed. Is Python script running?"));
    }
    
    SocketSubs->DestroySocket(Socket);
}