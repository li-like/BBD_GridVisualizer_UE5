#include "Simulation/SwarmDrone.h"
#include "Core/GeoSOTUtils.h" // 引入核心工具库
#include "CesiumGeoreference.h" // 需要在 Build.cs 中加入 "CesiumRuntime"
#include "Kismet/GameplayStatics.h"

ASwarmDrone::ASwarmDrone()
{
    PrimaryActorTick.bCanEverTick = true;

    // 创建一个小球代表无人机，方便观察 (实际连接 AirSim 后可隐藏)
    UStaticMeshComponent* MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DroneMesh"));
    RootComponent = MeshComp;
    
    // 加载一个简单的模型 (引擎自带的球体)
    static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (SphereMesh.Succeeded())
    {
        MeshComp->SetStaticMesh(SphereMesh.Object);
        MeshComp->SetWorldScale3D(FVector(0.5f)); // 直径50cm
    }
}

void ASwarmDrone::BeginPlay()
{
    Super::BeginPlay();
}

void ASwarmDrone::ExecuteGeoSOTPath(const TArray<int64>& PathCodes, ACesiumGeoreference* InGeoReference)
{
    if (!InGeoReference)
    {
        UE_LOG(LogTemp, Error, TEXT("[SwarmDrone] Error: No GeoReference provided!"));
        return;
    }

    this->GeoReference = InGeoReference;
    
    UE_LOG(LogTemp, Warning, TEXT("[SwarmDrone] Received Path with %d nodes. Converting to World Coordinates..."), PathCodes.Num());

    // 1. 坐标转换 (GeoSOT -> UE World)
    ConvertPathToWaypoints(PathCodes);

    // 2. 启动飞行
    if (Waypoints.Num() > 0)
    {
        CurrentWaypointIndex = 0;
        bIsFlying = true;
        // 把无人机瞬间移动到起点 (或者你可以让它飞过去)
        SetActorLocation(Waypoints[0]);
        UE_LOG(LogTemp, Warning, TEXT("[SwarmDrone] Takeoff! Flying to Node 1..."));
    }
}

void ASwarmDrone::ConvertPathToWaypoints(const TArray<int64>& PathCodes)
{
    Waypoints.Empty();

    // 为了避免变量名混淆，我们将循环变量命名为 PackedCode
    for (int64 PackedCode : PathCodes)
    {
        // 1. 解码：将 int64 拆解为 (Row, Col, Level, Alt)
        // 注意：DecodeGeoSOTCode 的参数类型是 int32&，所以我们定义 int32 变量来接收
        int32 R = 0;
        int32 C = 0;
        int32 L = 0;
        int32 H = 0;
        UGeoSOTUtils::DecodeGeoSOTCode(PackedCode, R, C, L, H);

        // 2. 构造 Cell 并正确赋值
        FGeoSOTCell TempCell;
        
        // --- 核心修正：访问嵌套结构体并赋值 ---
        // 这里的 Code 是 FGeoSOTCode 类型的成员变量
        TempCell.Code.Level = L;
        TempCell.Code.Row   = R;  // 自动从 int32 提升为 int64
        TempCell.Code.Col   = C;  // 自动从 int32 提升为 int64
        TempCell.Code.Alt   = H;  // 自动从 int32 提升为 int64

        // 注意：MinLon/MaxLon 等边界信息在这里没有手动计算。
        // 我们假设 CalculateGridWorldTransform 会根据 TempCell.Code 自动计算中心点，
        // 或者只需要 Code 就能算出中心点。
        
        // 3. 调用核心转换函数
        FVector WorldPos;
        FQuat WorldRot;
        FVector WorldScale;

        bool bSuccess = UGeoSOTUtils::CalculateGridWorldTransform(
            GeoReference,
            TempCell,
            WorldPos,
            WorldRot,
            WorldScale
        );

        if (bSuccess)
        {
            Waypoints.Add(WorldPos);
        }
    }
}

void ASwarmDrone::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (bIsFlying)
    {
        ProcessFlightControl(DeltaTime);
    }
}

void ASwarmDrone::ProcessFlightControl(float DeltaTime)
{
    // === 这里是模拟 AirSim 飞行的部分 ===
    // 实际对接 AirSim 时，这里会替换为 AirSim 的 API 调用
    // 例如: AirSimClient->moveOnPathAsync(Waypoints, Velocity, ...)

    if (Waypoints.Num() == 0 || CurrentWaypointIndex >= Waypoints.Num())
    {
        bIsFlying = false;
        UE_LOG(LogTemp, Warning, TEXT("[SwarmDrone] Mission Complete!"));
        return;
    }

    FVector TargetPos = Waypoints[CurrentWaypointIndex];
    FVector CurrentPos = GetActorLocation();

    // 计算方向和距离
    FVector Direction = (TargetPos - CurrentPos);
    float Distance = Direction.Size();

    if (Distance < AcceptanceRadius)
    {
        // 到达当前路点，前往下一个
        CurrentWaypointIndex++;
        UE_LOG(LogTemp, Log, TEXT("[SwarmDrone] Reached Waypoint %d"), CurrentWaypointIndex);
    }
    else
    {
        // 简单的插值飞行 (P控制)
        Direction.Normalize();
        FVector NewPos = CurrentPos + (Direction * FlightSpeed * DeltaTime);
        SetActorLocation(NewPos);
    }
}