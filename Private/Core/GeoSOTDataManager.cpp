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

    // 1. 执行物理环境扫掠探测
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

    // ==========================================================
    // 【核心修正 1】：在裂变时，引入气象和通信风险，计算真·综合得分！
    // ==========================================================
    double Rho_Risk = bHit ? 1.0 : 0.0;
    
    // 如果存在动态环境指针，则获取实时气象和通信风险
    double W_Risk = 0.0;
    double Q_Risk = 0.0;
    if (RiskEnvPtr) 
    {
        float CurrentTime = WorldPtr->GetTimeSeconds();
        W_Risk = RiskEnvPtr->GetWeatherRisk(CenterLat, CenterLon, CenterAlt, CurrentTime);
        Q_Risk = RiskEnvPtr->GetCommunicationRisk(CenterLat, CenterLon, CenterAlt);
    }

    double Score = 1.0 - (Weight_Rho * Rho_Risk + Weight_Weather * W_Risk + Weight_Comm * Q_Risk); 
    Score = FMath::Clamp(Score, 0.0, 1.0);
    bool bIsDangerous = (Score < Threshold_Theta);

    // 2. 裂变命运分流判定
    // 如果安全，或者开关关闭，或者已经切到了设定的极限层级 -> 终止裂变
    if (!bIsDangerous || !bEnableAdaptiveFission || CurrentLevel >= MaxFissionLevel)
    {
        FGridNodeData NewNode;
        NewNode.Level = CurrentLevel;
        NewNode.CenterLat = CenterLat;
        NewNode.CenterLon = CenterLon;
        NewNode.CenterAlt = CenterAlt;
        NewNode.GeoSOTCode = UGeoSOTMath::EncodeToGeoSOTString(CenterLat, CenterLon, CenterAlt, CurrentLevel, true);
        NewNode.Rho_Risk = Rho_Risk;
        NewNode.Score = Score;
        
        // 【最终生死判决】：只有它危险，并且由于条件限制(到达底层或关了开关)无法再裂变逃生时，它才被判为绝对障碍物！
        NewNode.bIsObstacle = bIsDangerous; 
        
        OutChunk.Nodes.Add(NewNode);
    }
    else
    {
        // 3. 触发八叉树裂变：当前节点不存入数组，直接炸成 8 个子节点！
        // 核心数学：子节点的起点索引，等于父节点索引左移一位 (乘以2)
        uint32 ChildLatStart = LatIdx << 1;
        uint32 ChildLonStart = LonIdx << 1;
        int32 ChildAltStart = AltIdx << 1; 

        // 生成 8 个子节点并递归探测
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

    CurrentScanChunkIndex = (CurrentScanChunkIndex + 1) % ReadyChunkKeys.Num();
    FString KeyToScan = ReadyChunkKeys[CurrentScanChunkIndex];

    if (FGridChunk* ChunkPtr = GlobalChunkMap.Find(KeyToScan))
    {
        // =========================================================
        // 分支 A：物理拓扑未锁死，执行昂贵的重建与扫掠
        // =========================================================
        if (!ChunkPtr->bIsTopologyFinalized)
        {
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

            // 增加扫描次数。如果一个区块被反复扫了 10 次（说明地形早就加载透了），将其永久锁死！
            ChunkPtr->ScanCount++;
            if (ChunkPtr->ScanCount > 10) 
            {
                ChunkPtr->bIsTopologyFinalized = true;
            }
        }
        // =========================================================
        // 分支 B：物理拓扑已锁死，只执行极速的动态气象/通信刷新
        // =========================================================
        else 
        {
            if (RiskEnvPtr) 
            {
                for (FGridNodeData& Node : ChunkPtr->Nodes) 
                {
                    // 仅更新动态权重
                    Node.W_Risk = RiskEnvPtr->GetWeatherRisk(Node.CenterLat, Node.CenterLon, Node.CenterAlt, CurrentTimeSeconds);
                    Node.Q_Risk = RiskEnvPtr->GetCommunicationRisk(Node.CenterLat, Node.CenterLon, Node.CenterAlt);
                    
                    // 重新算分
                    Node.Score = 1.0 - (Weight_Rho * Node.Rho_Risk + Weight_Weather * Node.W_Risk + Weight_Comm * Node.Q_Risk);
                    Node.Score = FMath::Clamp(Node.Score, 0.0, 1.0);
                    
                    // 动态风险可能会导致原本安全的网格分数不及格，但此时不再引发结构改变，仅作颜色警告
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