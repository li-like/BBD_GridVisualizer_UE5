#include "Simulation/SwarmManager.h"
#include "Simulation/SwarmDrone.h"

// --- 引入 Core 模块 ---
#include "Core/GeoSOTUtils.h"
#include "Core/GeoSOTPathFinder.h"
#include "Tests/GeoSOTPathTester.h"

// --- 引入 引擎/Cesium 工具 ---
#include "CesiumGeoreference.h"
#include "Kismet/GameplayStatics.h"

ASwarmManager::ASwarmManager()
{
    PrimaryActorTick.bCanEverTick = true;
}

void ASwarmManager::BeginPlay()
{
    Super::BeginPlay();
    
    // 1. 系统启动，先找到地理锚点
    FindGeoReference();

    // // 2. 延迟 1 秒执行测试，确保其他组件（如 Cesium 地形）已加载
    // FTimerHandle UnusedHandle;
    // GetWorldTimerManager().SetTimer(UnusedHandle, this, &ASwarmManager::RunSwarmTest, 1.0f, false);
}

void ASwarmManager::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
}

void ASwarmManager::FindGeoReference()
{
    // 自动查找场景里的 CesiumGeoreference
    TArray<AActor*> FoundActors;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ACesiumGeoreference::StaticClass(), FoundActors);

    if (FoundActors.Num() > 0)
    {
        GeoReference = Cast<ACesiumGeoreference>(FoundActors[0]);
        UE_LOG(LogTemp, Log, TEXT("[SwarmManager] Found CesiumGeoreference: %s"), *GeoReference->GetName());
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[SwarmManager] Error: No CesiumGeoreference found in the scene!"));
    }
}

void ASwarmManager::RunSwarmTest()
{
    UE_LOG(LogTemp, Warning, TEXT("========== SWARM TEST START =========="));

    // ==============================================================================
    // 模式一：联动模式 (优先)
    // 直接复用 GeoSOTPathTester (红线) 的路径数据，实现"所见即所得"
    // ==============================================================================
    if (bSyncWithTester)
    {
        // 1. 如果没有手动指定 TargetTester，尝试在场景中自动搜索一个
        if (!TargetTester)
        {
            TargetTester = Cast<AGeoSOTPathTester>(UGameplayStatics::GetActorOfClass(GetWorld(), AGeoSOTPathTester::StaticClass()));
        }

        if (TargetTester)
        {
            UE_LOG(LogTemp, Warning, TEXT("[SwarmManager] Found PathTester: %s. Syncing data..."), *TargetTester->GetName());

            // 2. 获取 PathTester 已经计算好的路径
            // 前提：PathTester 必须已经运行过 BeginPlay 并生成了路径
            const TArray<int64>& SyncedPath = TargetTester->GetCalculatedPath();

            if (SyncedPath.Num() > 0)
            {
                UE_LOG(LogTemp, Log, TEXT("[SwarmManager] Successfully copied %d nodes from PathTester."), SyncedPath.Num());
                
                // 3. 这里的 GeoReference 我们最好也跟 PathTester 保持一致，或者使用场景默认的
                if (!GeoReference) FindGeoReference();

                // 4. 直接执行任务，不再重复寻路
                SpawnAndCommandDrone(SyncedPath); // 直接把红线的路径传给无人机
                
                UE_LOG(LogTemp, Warning, TEXT("========== SWARM TEST END (SYNCED) =========="));
                return; // 联动成功，直接返回，不再执行下面的独立逻辑
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("[SwarmManager] PathTester found, but it has NO path data. Falling back to independent test."));
            }
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("[SwarmManager] bSyncWithTester is True, but NO AGeoSOTPathTester found in scene. Falling back..."));
        }
    }

    // ==============================================================================
    // 模式二：独立测试模式 (兜底)
    // 如果没有 PathTester，或者联动失败，自动生成一条从原点出发的测试路径
    // ==============================================================================
    UE_LOG(LogTemp, Log, TEXT("[SwarmManager] Running independent test mode..."));

    if (!GeoReference)
    {
        FindGeoReference();
        if (!GeoReference) return;
    }

    // --- 1. 定义起终点 (基于 GeoRef 原点偏移，确保在视野内) ---
    // 假设 Level 21
    int32 TargetLevel = 21;
    
    // 获取当前地理原点的经纬度
    double OriginLat = GeoReference->GetOriginLatitude();
    double OriginLon = GeoReference->GetOriginLongitude();
    
    // 起点：原点
    // 终点：向东偏移 0.001 度 (约100米)，向北偏移 0.0005 度
    double EndLat = OriginLat + 0.0005; 
    double EndLon = OriginLon + 0.0010;

    // --- 2. 编码 (LatLon -> GeoSOT Code) ---
    int32 StartRow, StartCol;
    int32 EndRow, EndCol;
    
    // 调用 Utils 进行转换
    UGeoSOTUtils::LatLonToGeoSOT(OriginLat, OriginLon, TargetLevel, StartRow, StartCol);
    UGeoSOTUtils::LatLonToGeoSOT(EndLat, EndLon, TargetLevel, EndRow, EndCol);

    // 生成编码 (假设高度层级为 10，保证在空中)
    // 注意：这里的高度层级可能需要根据实际地形调整，防止生成在地底下
    int64 StartCode = UGeoSOTUtils::GenerateGeoSOTCode(StartRow, StartCol, TargetLevel, 10);
    int64 EndCode   = UGeoSOTUtils::GenerateGeoSOTCode(EndRow, EndCol,   TargetLevel, 10);

    UE_LOG(LogTemp, Log, TEXT("[SwarmManager] Independent Plan: Start[%lld] -> End[%lld]"), StartCode, EndCode);

    // --- 3. 核心寻路 (Call Core) ---
    TArray<int64> PathCodes;
    bool bPathFound = UGeoSOTPathFinder::FindPath(this, StartCode, EndCode, PathCodes);

    if (bPathFound && PathCodes.Num() > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[SwarmManager] Independent Path Found! Length: %d nodes. Spawning Drone..."), PathCodes.Num());
        
        // --- 4. 执行任务 ---
        SpawnAndCommandDrone(PathCodes);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[SwarmManager] Failed to find path. Check if start/end are valid or blocked."));
    }

    UE_LOG(LogTemp, Warning, TEXT("========== SWARM TEST END =========="));
}

void ASwarmManager::SpawnAndCommandDrone(const TArray<int64>& PathCodes)
{
    if (!GeoReference) return;

    // 1. 如果没有无人机，生成一架
    if (!TestDrone)
    {
        // 初始位置设为 (0,0,0)，它会马上被 ExecuteGeoSOTPath 修正到路径起点
        FVector SpawnLoc = FVector::ZeroVector; 
        FRotator SpawnRot = FRotator::ZeroRotator;
        
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        TestDrone = GetWorld()->SpawnActor<ASwarmDrone>(ASwarmDrone::StaticClass(), SpawnLoc, SpawnRot, SpawnParams);
    }

    // 2. 发送指令
    if (TestDrone)
    {
        // 这里的 ExecuteGeoSOTPath 是我们在 SwarmDrone.h 中定义的接口
        TestDrone->ExecuteGeoSOTPath(PathCodes, GeoReference);
    }
}