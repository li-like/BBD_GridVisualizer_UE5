#include "Simulation/SwarmDrone.h"
#include "Core/GeoSOTUtils.h"
#include "CesiumGeoreference.h"
#include "DrawDebugHelpers.h"

// 构造函数
ASwarmDrone::ASwarmDrone()
{
    // 在这个模式下，我们不需要 Tick，因为是 Python 在控制飞行
    PrimaryActorTick.bCanEverTick = false; 
}

void ASwarmDrone::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);
}

void ASwarmDrone::ExecuteGeoSOTPath(const TArray<int64>& PathCodes, ACesiumGeoreference* InGeoReference)
{
    if (!InGeoReference)
    {
        UE_LOG(LogTemp, Error, TEXT("[SwarmDrone] Error: GeoReference is NULL!"));
        return;
    }
    this->GeoReference = InGeoReference;

    // 1. 转换坐标 (GeoSOT -> UE World)
    ConvertPathToWaypoints(PathCodes);

    // 2. 发送给 Python
    if (Waypoints.Num() > 0)
    {
        SendPathToPython();
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[SwarmDrone] Warning: Converted path is empty!"));
    }
}



void ASwarmDrone::ConvertPathToWaypoints(const TArray<int64>& PathCodes)
{
    Waypoints.Empty();
    if (!GeoReference) 
    {
        UE_LOG(LogTemp, Error, TEXT("[SwarmDrone] Error: GeoReference is NULL!"));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("[SwarmDrone] Converting %d path nodes using validated GeoSOTUtils..."), PathCodes.Num());

    for (int64 Code : PathCodes)
    {
        int32 R, C, L, H;
        
        // 1. 解码 (使用你现有的 DecodeGeoSOTCode)
        // 注意：你目前的 Decode 函数内部写死了 OutLevel = 21
        // 如果你的 PathCodes 是 L21 生成的，这没问题。
        // 如果需要动态层级，你需要确保 Decode 逻辑正确。
        UGeoSOTUtils::DecodeGeoSOTCode(Code, R, C, L, H);

        // 2. 计算经纬度中心 (使用你现有的 GetGeoSOTCenter)
        double Lat, Lon;
        UGeoSOTUtils::GetGeoSOTCenter(R, C, L, Lat, Lon);

        // 3. 计算高度
        // 使用你现有的 GetHeightSpan 公式：1度物理距离 * GridSpanDeg
        // 这里的逻辑是：H 是高度索引，索引 * 每层高度跨度 = 实际高度
        double HSpan = UGeoSOTUtils::GetHeightSpan(L);
        double AltMeters = (double)H * HSpan; 

        // 4. 坐标转换 (Cesium LLH -> Unreal World)
        // Cesium 函数期望的是 (Longitude, Latitude, Height)
        FVector3d TargetLLH(Lon, Lat, AltMeters);
        
        // 核心：利用 Cesium GeoReference 进行精确转换
        FVector WorldPos = GeoReference->TransformLongitudeLatitudeHeightPositionToUnreal(TargetLLH);

        Waypoints.Add(WorldPos);
    }
    
    // --- [调试校验] ---
    if (Waypoints.Num() >= 2)
    {
        FVector P0 = Waypoints[0];
        FVector P1 = Waypoints[1];
        float Dist = FVector::Dist(P0, P1);
        UE_LOG(LogTemp, Warning, TEXT("[SwarmDrone] Conversion Verify: P0=%s, P1=%s, Dist=%.2f cm"), 
            *P0.ToString(), *P1.ToString(), Dist);
            
        if (Dist < 0.1f) // 如果两点距离小于 1毫米，说明转换可能有问题（或者路径本身就在原地）
        {
             UE_LOG(LogTemp, Error, TEXT("[SwarmDrone] CRITICAL WARNING: Adjacent path points are identical! Check Level/Decoding logic."));
        }
    }
}

void ASwarmDrone::SendPathToPython()
{
    // --- 1. 创建 Socket (保持不变) ---
    ISocketSubsystem* SocketSubs = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    FSocket* Socket = SocketSubs->CreateSocket(NAME_Stream, TEXT("AirSimConnector"), false);
    
    if (!Socket) return;

    TSharedRef<FInternetAddr> Addr = SocketSubs->CreateInternetAddr();
    bool bIsValid;
    Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
    Addr->SetPort(8888);

    UE_LOG(LogTemp, Warning, TEXT("[SwarmDrone] Connecting to Python..."));
    
    if (!Socket->Connect(*Addr))
    {
        UE_LOG(LogTemp, Error, TEXT("[SwarmDrone] Connection Failed!"));
        SocketSubs->DestroySocket(Socket);
        return;
    }

    // --- 2. 构造 JSON 字符串 ---
    // 我们手动拼接字符串，简单高效，不需要引入 Json 模块
    FString JsonPayload = TEXT("{\n");
    
    // 添加起点信息 (UE 坐标)
    FVector StartUE = Waypoints[0];
    JsonPayload += FString::Printf(TEXT("  \"origin_ue\": {\"x\": %.2f, \"y\": %.2f, \"z\": %.2f},\n"), StartUE.X, StartUE.Y, StartUE.Z);
    
    // 开始点位数组
    JsonPayload += TEXT("  \"path\": [\n");

    for (int i = 0; i < Waypoints.Num(); i++)
    {
        FVector Pt = Waypoints[i];
        
        // 核心转换: UE(cm) -> AirSim(m)
        // 计算相对于起点的偏移量
        FVector Rel = Pt - StartUE;
        float X_m = Rel.X / 100.0f;
        float Y_m = Rel.Y / 100.0f;
        float Z_m = -Rel.Z / 100.0f; // Z反转

        // 拼接单个点
        FString PointStr = FString::Printf(TEXT("    {\"x\": %.3f, \"y\": %.3f, \"z\": %.3f}"), X_m, Y_m, Z_m);
        
        // 如果不是最后一个点，加逗号
        if (i < Waypoints.Num() - 1)
        {
            PointStr += TEXT(",");
        }
        JsonPayload += PointStr + TEXT("\n");
    }

    JsonPayload += TEXT("  ]\n}"); // 闭合 JSON

    // --- 3. 发送数据 ---
    TCHAR* SerializedChar = JsonPayload.GetCharArray().GetData();
    FTCHARToUTF8 Convert(SerializedChar); // 转为 UTF-8
    
    int32 Sent = 0;
    bool bSuccess = Socket->Send((uint8*)Convert.Get(), Convert.Length(), Sent);
    
    if (bSuccess)
    {
        UE_LOG(LogTemp, Warning, TEXT("[SwarmDrone] JSON Path sent! Size: %d bytes"), Sent);
    }

    // --- 4. 彻底断开 ---
    // 发送完毕后直接关闭，这样 Python 端的 recv() 会收到空数据从而知道传输结束
    Socket->Close();
    SocketSubs->DestroySocket(Socket);
}