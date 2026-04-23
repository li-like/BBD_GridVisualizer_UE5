#include "Core/GeoSOTDataManager.h"
#include "Core/GeoSOTMath.h"
#include "Fission/GeoSOTRiskEnvironment.h"
#include "CesiumGeoreference.h"
#include "Engine/World.h"

static uint32 AngleToGridIndex(double Angle, int32 Level) {
    uint32 FullInt = static_cast<uint32>((Angle + 256.0) * 8388608.0);
    return FullInt >> (32 - Level);
}
static double GridIndexToCenterAngle(uint32 Index, int32 Level) {
    int32 Shift = 32 - Level;
    uint32 CenterOffset = (Shift > 0) ? (1 << (Shift - 1)) : 0;
    uint32 FullInt = (Index << Shift) | CenterOffset;
    return (static_cast<double>(FullInt) / 8388608.0) - 256.0;
}

UGeoSOTDataManager::UGeoSOTDataManager() {}

void UGeoSOTDataManager::InitializeManager(UWorld* InWorld, ACesiumGeoreference* InGeoRef, UGeoSOTRiskEnvironment* InRiskEnv,
                                           double InMinLat, double InMaxLat, double InMinLon, double InMaxLon, 
                                           double InMinAlt, double InMaxAlt, int32 InTargetLevel)
{
    WorldPtr = InWorld; GeoRefPtr = InGeoRef; RiskEnvPtr = InRiskEnv;
    TargetLevel = InTargetLevel;
    
    // 【新增】接收渲染器传来的高度参数
    MinAlt = InMinAlt;
    MaxAlt = InMaxAlt;
    
    GlobalChunkMap.Empty();

    // 算出目标层级下，用户指定范围的绝对整数索引边界
    GlobalMinLatIdx = AngleToGridIndex(InMinLat, TargetLevel);
    GlobalMaxLatIdx = AngleToGridIndex(InMaxLat, TargetLevel);
    GlobalMinLonIdx = AngleToGridIndex(InMinLon, TargetLevel);
    GlobalMaxLonIdx = AngleToGridIndex(InMaxLon, TargetLevel);
}

void UGeoSOTDataManager::UpdateObserverPosition(const FVector& CameraPos)
{
    if (!GeoRefPtr || !WorldPtr) return;
    ProcessLoadQueue();

    FVector3d CamLLH = GeoRefPtr->TransformUnrealPositionToLongitudeLatitudeHeight((FVector3d)CameraPos);
    
    // 1. 算出相机在 Level 20 下的绝对索引
    uint32 CamLatIdx = AngleToGridIndex(CamLLH.Y, TargetLevel);
    uint32 CamLonIdx = AngleToGridIndex(CamLLH.X, TargetLevel);

    // 2. 算出相机所在的 Chunk 坐标 (极其简单的整数除法！)
    int32 CamChunkX = CamLonIdx / ChunkGridSize;
    int32 CamChunkY = CamLatIdx / ChunkGridSize;

    // 3. 圈定周围 3x3 个 Chunk 进行加载
    for (int32 i = -1; i <= 1; ++i)
    {
        for (int32 j = -1; j <= 1; ++j)
        {
            int32 TargetChunkX = CamChunkX + i;
            int32 TargetChunkY = CamChunkY + j;

            FString ChunkKey = FString::Printf(TEXT("C_%d_%d"), TargetChunkX, TargetChunkY);

            if (!GlobalChunkMap.Contains(ChunkKey))
            {
                FGridChunk NewChunk;
                NewChunk.ChunkKey = ChunkKey;
                NewChunk.State = EChunkState::Unloaded;
                NewChunk.ChunkX = TargetChunkX;
                NewChunk.ChunkY = TargetChunkY;
                
                GlobalChunkMap.Add(ChunkKey, NewChunk);
                ChunkLoadQueue.Enqueue(ChunkKey);
            }
        }
    }
}

void UGeoSOTDataManager::ProcessLoadQueue()
{
    FString KeyToLoad;
    if (ChunkLoadQueue.Dequeue(KeyToLoad)) {
        if (FGridChunk* ChunkPtr = GlobalChunkMap.Find(KeyToLoad)) {
            ChunkPtr->State = EChunkState::Loading;
            BuildChunkNodes(*ChunkPtr);
            ChunkPtr->State = EChunkState::Ready;
            
            // 【新增】记录就绪的区块，准备进行雷达式的轮询扫描
            ReadyChunkKeys.Add(KeyToLoad); 
        }
    }
}

void UGeoSOTDataManager::BuildChunkNodes(FGridChunk& OutChunk)
{
    if (!WorldPtr || !GeoRefPtr) return;

    double BaseSpanMeters = UGeoSOTMath::GetGridSpanMeters(TargetLevel); // TargetLevel 是 20
    int32 AltLayerMin = FMath::FloorToInt(MinAlt / BaseSpanMeters);
    int32 AltLayerMax = FMath::FloorToInt(MaxAlt / BaseSpanMeters);

    uint32 StartLonIdx = OutChunk.ChunkX * ChunkGridSize;
    uint32 StartLatIdx = OutChunk.ChunkY * ChunkGridSize;

    // 初始遍历生成所有 L20 基准节点，交给递归器去处理
    for (int32 h = AltLayerMin; h <= AltLayerMax; ++h)
    {
        for (int32 i = 0; i < ChunkGridSize; ++i)
        {
            for (int32 j = 0; j < ChunkGridSize; ++j)
            {
                uint32 CurrentLonIdx = StartLonIdx + i;
                uint32 CurrentLatIdx = StartLatIdx + j;

                // 边界裁剪
                if (CurrentLatIdx < GlobalMinLatIdx || CurrentLatIdx > GlobalMaxLatIdx ||
                    CurrentLonIdx < GlobalMinLonIdx || CurrentLonIdx > GlobalMaxLonIdx) continue;

                // 【核心引爆点】：将初始节点扔进递归工厂
                EvaluateAndFission(OutChunk, CurrentLatIdx, CurrentLonIdx, h, TargetLevel);
            }
        }
    }
}

// =========================================================
// 自适应八叉树递归裂变核心工厂
// =========================================================
void UGeoSOTDataManager::EvaluateAndFission(FGridChunk& OutChunk, uint32 LatIdx, uint32 LonIdx, int32 AltIdx, int32 CurrentLevel)
{
    double SpanMeters = UGeoSOTMath::GetGridSpanMeters(CurrentLevel);
    double CenterLat = GridIndexToCenterAngle(LatIdx, CurrentLevel);
    double CenterLon = GridIndexToCenterAngle(LonIdx, CurrentLevel);
    double CenterAlt = (AltIdx * SpanMeters) + (SpanMeters * 0.5);

    // ==========================================================
    // 【核心策略分流与性能优化】
    // ==========================================================
    // 判断当前节点是否属于“强制向下裂变”的中间节点
    bool bIsIntermediateUniformNode = (DivisionMode == EGridDivisionMode::FullUniform) && (CurrentLevel < TargetUniformLevel);

    double Rho_Risk = 0.0;
    double Score = 1.0;
    bool bIsDangerous = false;

    // 优化引爆点：如果是完全划分的中间节点，直接跳过昂贵的 UE 物理扫掠和风险计算！
    if (!bIsIntermediateUniformNode)
    {
        // --- 1. 执行物理环境扫掠探测 (保持你原有的逻辑) ---
        FVector3d Center3D = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(CenterLon, CenterLat, CenterAlt));
        FVector WorldCenter = (FVector)Center3D;
        FVector3d UpLLH = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(CenterLon, CenterLat, CenterAlt + 100.0));
        FVector UpDir = (FVector)(UpLLH - Center3D).GetSafeNormal();
        FVector3d NorthLLH = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(CenterLon, CenterLat + 0.01, CenterAlt));
        FVector TempNorthDir = (FVector)(NorthLLH - Center3D).GetSafeNormal();
        FQuat WorldQuat = FRotationMatrix::MakeFromXZ(FVector::CrossProduct(TempNorthDir, UpDir).GetSafeNormal(), UpDir).ToQuat();

        FVector BoxHalfExtent(SpanMeters * 48.0, SpanMeters * 48.0, 1.0);
        FCollisionShape SweepShape = FCollisionShape::MakeBox(BoxHalfExtent);
        FVector TopPos = WorldCenter + UpDir * (SpanMeters * 50.0);
        FVector BottomPos = WorldCenter - UpDir * (SpanMeters * 50.0);
        
        FHitResult HitResult;
        FCollisionQueryParams QueryParams;
        QueryParams.bTraceComplex = true;
        bool bHit = WorldPtr->SweepSingleByChannel(HitResult, TopPos, BottomPos, WorldQuat, ECC_Visibility, SweepShape, QueryParams);

        // --- 2. 气象和通信风险计算 ---
        Rho_Risk = bHit ? 1.0 : 0.0;
        double W_Risk = 0.0;
        double Q_Risk = 0.0;
        if (RiskEnvPtr) 
        {
            float CurrentTime = WorldPtr->GetTimeSeconds();
            W_Risk = RiskEnvPtr->GetWeatherRisk(CenterLat, CenterLon, CenterAlt, CurrentTime);
            Q_Risk = RiskEnvPtr->GetCommunicationRisk(CenterLat, CenterLon, CenterAlt);
        }

        Score = 1.0 - (Weight_Rho * Rho_Risk + Weight_Weather * W_Risk + Weight_Comm * Q_Risk); 
        Score = FMath::Clamp(Score, 0.0, 1.0);
        bIsDangerous = (Score < Threshold_Theta);
    }

    // ==========================================================
    // 命运分流判定：到底存为网格，还是继续裂变？
    // ==========================================================
    bool bShouldFission = false;

    if (DivisionMode == EGridDivisionMode::FullUniform)
    {
        // 完全划分模式：只要没到目标层级，无条件裂变
        bShouldFission = (CurrentLevel < TargetUniformLevel);
    }
    else
    {
        // 自适应模式：危险 + 开关开启 + 没到底限层级
        bShouldFission = (bIsDangerous && bEnableAdaptiveFission && CurrentLevel < MaxFissionLevel);
    }

    if (!bShouldFission)
    {
        // 终止裂变，记录当前节点
        FGridNodeData NewNode;
        NewNode.Level = CurrentLevel;
        NewNode.CenterLat = CenterLat;
        NewNode.CenterLon = CenterLon;
        NewNode.CenterAlt = CenterAlt;
        NewNode.GeoSOTCode = UGeoSOTMath::EncodeToGeoSOTString(CenterLat, CenterLon, CenterAlt, CurrentLevel, true);
        NewNode.Rho_Risk = Rho_Risk;
        NewNode.Score = Score;
        
        // 危险判定：只有在非中间节点计算过得分后，且得分低于阈值才为危险
        NewNode.bIsObstacle = bIsDangerous; 
        
        OutChunk.Nodes.Add(NewNode);
    }
    else
    {
        // 触发八叉树裂变：炸成 8 个子节点
        uint32 ChildLatStart = LatIdx << 1;
        uint32 ChildLonStart = LonIdx << 1;
        int32 ChildAltStart = AltIdx << 1; 

        for (int32 h = 0; h < 2; ++h) {
            for (int32 i = 0; i < 2; ++i) {
                for (int32 j = 0; j < 2; ++j) {
                    EvaluateAndFission(OutChunk, ChildLatStart + i, ChildLonStart + j, ChildAltStart + h, CurrentLevel + 1);
                }
            }
        }
    }
}

// 辅助函数：将一个父节点的所有空间填满子节点
void UGeoSOTDataManager::SubdivideTestNode(FGridChunk& Chunk, uint32 PLatIdx, uint32 PLonIdx, int32 PAltIdx, int32 PLevel, int32 TLevel)
{
    // 计算倍率。例如 20 到 23 级，倍率是 2^(23-20) = 8
    int32 Ratio = 1 << (TLevel - PLevel);
    
    // 计算子网格在目标层级的起始索引
    uint32 StartLat = PLatIdx << (TLevel - PLevel);
    uint32 StartLon = PLonIdx << (TLevel - PLevel);
    int32 StartAlt = PAltIdx * Ratio;

    double Span = UGeoSOTMath::GetGridSpanMeters(TLevel);

    for (int32 ah = 0; ah < Ratio; ++ah) {
        for (int32 ai = 0; ai < Ratio; ++ai) {
            for (int32 aj = 0; aj < Ratio; ++aj) {
                FGridNodeData SubNode;
                SubNode.Level = TLevel;
                
                uint32 CLatIdx = StartLat + ai;
                uint32 CLonIdx = StartLon + aj;
                int32 CAltIdx = StartAlt + ah;

                SubNode.CenterLat = GridIndexToCenterAngle(CLatIdx, TLevel);
                SubNode.CenterLon = GridIndexToCenterAngle(CLonIdx, TLevel);
                SubNode.CenterAlt = (CAltIdx * Span) + (Span * 0.5);
                
                // 这里暂时假设子网格都标记为危险，以便观察红色
                SubNode.bIsObstacle = true;
                SubNode.Score = 0.0; 
                
                Chunk.Nodes.Add(SubNode);
            }
        }
    }
}

// =========================================================
// 核心 3：滚动动态扫描 (解决 Cesium 异步加载导致物理漏判的神技)
// =========================================================
void UGeoSOTDataManager::RollingDynamicScan(float CurrentTimeSeconds)
{
    if (ReadyChunkKeys.Num() == 0 || !WorldPtr || !GeoRefPtr) return;

    // 轮询：每帧只拿出一个 Chunk 来处理，分摊性能压力
    CurrentScanChunkIndex = (CurrentScanChunkIndex + 1) % ReadyChunkKeys.Num();
    FString KeyToScan = ReadyChunkKeys[CurrentScanChunkIndex];

    if (FGridChunk* ChunkPtr = GlobalChunkMap.Find(KeyToScan))
    {
        // =========================================================
        // 分支 A：重量级物理扫描 (带有 2.0 秒的冷却时间)
        // =========================================================
        // 只有当当前时间距离上次扫描超过 2 秒时，才执行昂贵的物理扫掠
        if (CurrentTimeSeconds - ChunkPtr->LastPhysicsScanTime > 5.0f)
        {
            // 重置秒表
            ChunkPtr->LastPhysicsScanTime = CurrentTimeSeconds;
            
            ChunkPtr->Nodes.Empty(); 

            double BaseSpanMeters = UGeoSOTMath::GetGridSpanMeters(TargetLevel);
            int32 AltLayerMin = FMath::FloorToInt(MinAlt / BaseSpanMeters);
            int32 AltLayerMax = FMath::FloorToInt(MaxAlt / BaseSpanMeters);

            uint32 StartLonIdx = ChunkPtr->ChunkX * ChunkGridSize;
            uint32 StartLatIdx = ChunkPtr->ChunkY * ChunkGridSize;

            for (int32 h = AltLayerMin; h <= AltLayerMax; ++h) {
                for (int32 i = 0; i < ChunkGridSize; ++i) {
                    for (int32 j = 0; j < ChunkGridSize; ++j) {
                        uint32 CLonIdx = StartLonIdx + i;
                        uint32 CLatIdx = StartLatIdx + j;
                        if (CLatIdx < GlobalMinLatIdx || CLatIdx > GlobalMaxLatIdx ||
                            CLonIdx < GlobalMinLonIdx || CLonIdx > GlobalMaxLonIdx) continue;
                        
                        EvaluateAndFission(*ChunkPtr, CLatIdx, CLonIdx, h, TargetLevel);
                    }
                }
            }
        }
        // =========================================================
        // 分支 B：轻量级动态风险刷新 (无冷却，极速运算)
        // =========================================================
        else 
        {
            // 在物理冷却的间隙，我们只做简单的数学算分，这几乎不消耗性能
            if (RiskEnvPtr) 
            {
                for (FGridNodeData& Node : ChunkPtr->Nodes) 
                {
                    Node.W_Risk = RiskEnvPtr->GetWeatherRisk(Node.CenterLat, Node.CenterLon, Node.CenterAlt, CurrentTimeSeconds);
                    Node.Q_Risk = RiskEnvPtr->GetCommunicationRisk(Node.CenterLat, Node.CenterLon, Node.CenterAlt);
                    
                    Node.Score = 1.0 - (Weight_Rho * Node.Rho_Risk + Weight_Weather * Node.W_Risk + Weight_Comm * Node.Q_Risk);
                    Node.Score = FMath::Clamp(Node.Score, 0.0, 1.0);
                    
                    Node.bIsObstacle = (Node.Score < Threshold_Theta);
                }
            }
        }
    }
}

void UGeoSOTDataManager::GetReadyNodesInRadius(const FVector& CameraPos, double Radius, TArray<FGridNodeData>& OutNodes) const
{
    OutNodes.Empty();
    double RadiusSq = Radius * Radius;
    for (const auto& Pair : GlobalChunkMap) {
        if (Pair.Value.State == EChunkState::Ready) {
            for (const FGridNodeData& Node : Pair.Value.Nodes) {
                FVector3d World3D = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(Node.CenterLon, Node.CenterLat, Node.CenterAlt));
                if (FVector::DistSquaredXY((FVector)World3D, CameraPos) <= RadiusSq) {
                    OutNodes.Add(Node);
                }
            }
        }
    }
}

// =========================================================
// 论文数据导出：CSV 写入逻辑
// =========================================================
void UGeoSOTDataManager::ExportMapSnapshotToCSV(FString AreaName)
{
    // 定义文件保存路径 
    FString DirectoryPath = FPaths::ProjectSavedDir() + TEXT("SimulationData/");
    // 文件名会自动带上区域名和当前系统时间，防止覆盖
    FString FileName = FString::Printf(TEXT("%s_GeoSOT_%s.csv"), *AreaName, *FDateTime::Now().ToString(TEXT("%H%M%S")));
    FString FilePath = DirectoryPath + FileName;

    // 确保文件夹存在
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*DirectoryPath)) {
        PlatformFile.CreateDirectory(*DirectoryPath);
    }

    // 写入 CSV 的表头 (严格对应论文的数据维度)
    FString CSVContent = TEXT("Level,CenterLat,CenterLon,CenterAlt,GeoSOTCode,Rho_Risk,W_Risk,Q_Risk,TotalScore,IsObstacle\n");

    int32 ExportedNodeCount = 0;

    // 遍历大脑里所有的区块和网格
    for (const auto& Pair : GlobalChunkMap) 
    {
        // 只有彻底就绪的网格才会被记录
        if (Pair.Value.State == EChunkState::Ready) 
        {
            for (const FGridNodeData& Node : Pair.Value.Nodes) 
            {
                // 将数据格式化为逗号分隔的字符串
                FString Line = FString::Printf(TEXT("%d,%.7f,%.7f,%.2f,%s,%.2f,%.2f,%.2f,%.4f,%d\n"),
                    Node.Level, 
                    Node.CenterLat, 
                    Node.CenterLon, 
                    Node.CenterAlt, 
                    *Node.GeoSOTCode,
                    Node.Rho_Risk, 
                    Node.W_Risk, 
                    Node.Q_Risk, 
                    Node.Score, 
                    Node.bIsObstacle ? 1 : 0);
                
                CSVContent += Line;
                ExportedNodeCount++;
            }
        }
    }

    // 写入本地硬盘
    if (FFileHelper::SaveStringToFile(CSVContent, *FilePath))
    {
        // 屏幕打印绿字，提示成功！
        if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::Printf(TEXT("成功导出 %d 个网格数据！"), ExportedNodeCount));
        UE_LOG(LogTemp, Warning, TEXT("导出路径: %s"), *FilePath);
    }
    else
    {
        if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("导出失败！请检查文件夹权限。"));
    }
}

void UGeoSOTDataManager::ClearMapMemory()
{
    GlobalChunkMap.Empty();
    ReadyChunkKeys.Empty();
    if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, TEXT("地图记忆已清空，可开始新的区域采集。"));
}

