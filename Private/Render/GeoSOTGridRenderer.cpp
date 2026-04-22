#include "Render/GeoSOTGridRenderer.h"
#include "Core/GeoSOTMath.h"
#include "Fission/GeoSOTRiskEnvironment.h" 
#include "Kismet/GameplayStatics.h"
#include "CesiumGeoreference.h" // 确保这行存在
#include "Engine/World.h"

// --- 纯整数转换辅助函数 (用于计算画框的边界) ---
static uint32 AngleToGridIndex(double Angle, int32 Level) {
    uint32 FullInt = static_cast<uint32>((Angle + 256.0) * 8388608.0);
    return FullInt >> (32 - Level);
}
static double GridIndexToBoundaryAngle(uint32 Index, int32 Level) {
    int32 Shift = 32 - Level;
    uint32 BoundaryInt = Index << Shift;
    return (static_cast<double>(BoundaryInt) / 8388608.0) - 256.0;
}

AGeoSOTGridRenderer::AGeoSOTGridRenderer()
{
    PrimaryActorTick.bCanEverTick = true;
    GridProcMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GridProcMesh"));
    RootComponent = GridProcMesh;
    GridProcMesh->bUseAsyncCooking = true;
    
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> AcademicMat(TEXT("你的材质路径"));
    if (AcademicMat.Succeeded()) GridProcMesh->SetMaterial(0, AcademicMat.Object);
}

void AGeoSOTGridRenderer::BeginPlay()
{
    Super::BeginPlay();
    InitializeTextPool();

    // 【核心修复】：解决运行游戏后不更新的 Bug！必须给大脑喂一次全新的游戏世界(PIE)指针。
    if (!RiskEnv) RiskEnv = NewObject<UGeoSOTRiskEnvironment>(this);
    if (!DataManager) DataManager = NewObject<UGeoSOTDataManager>(this);

    if (GetWorld() && CesiumGeoreference)
    {
        RiskEnv->InitializeMicroEnvironment(MinLat, MaxLat, MinLon, MaxLon); 
        DataManager->InitializeManager(GetWorld(), CesiumGeoreference, RiskEnv, MinLat, MaxLat, MinLon, MaxLon, MinAlt, MaxAlt, TargetLevel);
    }
}

bool AGeoSOTGridRenderer::ShouldTickIfViewportsOnly() const { return true; }

void AGeoSOTGridRenderer::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    InitializeTextPool();

    if (!RiskEnv) RiskEnv = NewObject<UGeoSOTRiskEnvironment>(this);
    if (!DataManager) DataManager = NewObject<UGeoSOTDataManager>(this);
    
    DataManager->bEnableAdaptiveFission = this->bEnableAdaptiveFission;
    DataManager->MaxFissionLevel = this->MaxFissionLevel;

    if (GetWorld() && CesiumGeoreference)
    {
        RiskEnv->InitializeMicroEnvironment(MinLat, MaxLat, MinLon, MaxLon); 
        
        // 【修改】把 MinAlt 和 MaxAlt 也一并传给大脑！
        DataManager->InitializeManager(GetWorld(), CesiumGeoreference, RiskEnv, 
                                       MinLat, MaxLat, MinLon, MaxLon, 
                                       MinAlt, MaxAlt, TargetLevel);
    }
}

bool AGeoSOTGridRenderer::GetCurrentCameraLocation(FVector& OutLocation) const
{
    if (GetWorld()->HasBegunPlay()) {
        APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
        if (CameraManager) { OutLocation = CameraManager->GetCameraLocation(); return true; }
    }
    if (GetWorld()->ViewLocationsRenderedLastFrame.Num() > 0) {
        OutLocation = GetWorld()->ViewLocationsRenderedLastFrame[0]; return true;
    }
    return false;
}

// =========================================================
// 极简主循环：全权委托给大脑
// =========================================================
void AGeoSOTGridRenderer::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    FVector CameraPos;
    if (!GetCurrentCameraLocation(CameraPos) || !CesiumGeoreference || !DataManager || !bIsPoolInitialized) return;

    // 1. 投喂相机位置，让大脑在后台去加载/切分 Chunk
    DataManager->UpdateObserverPosition(CameraPos);

    // 2. 触发大脑的心跳，刷新环境风险 (风和通信)
    DataManager->RollingDynamicScan(GetWorld()->GetTimeSeconds());
    

    if (!bIsAnchorInitialized || FVector::DistSquaredXY(CameraPos, LastQueryAnchor) > (AnchorUpdateThreshold * AnchorUpdateThreshold))
    {
        // 更新锚点
        LastQueryAnchor = CameraPos;
        bIsAnchorInitialized = true;
    }

    // 3. 索要数据：【注意】这里用的是 LastQueryAnchor（静止的锚点），而不是乱动的 CameraPos！
    TArray<FGridNodeData> ActiveNodes;
    DataManager->GetReadyNodesInRadius(LastQueryAnchor, SensingRadius, ActiveNodes);

    if (ActiveNodes.Num() == 0) return;

    // 4. 智能渲染判断：
    // 因为锚点在 500 米内是静止的，所以 ActiveNodes.Num() 绝对不会变，不会触发清空重建！
    if (ActiveNodes.Num() != LastRenderedNodeCount)
    {
        RebuildMeshGeometry(ActiveNodes);
        LastRenderedNodeCount = ActiveNodes.Num();
    }
    else
    {
        // 5. 颜色更新：【注意】这里传的依然是真实的 CameraPos！
        // 这样即使几何体没更新，脚下的绿色安全区依然会平滑跟随你的真实视角。
        UpdateMeshColors(ActiveNodes, CameraPos);
    }

    // 5. 更新文字
    UpdateTextLOD(ActiveNodes, CameraPos);
}

// =========================================================
// 几何体瞬时重构 (仅在加载区块时执行)
// =========================================================
void AGeoSOTGridRenderer::RebuildMeshGeometry(const TArray<FGridNodeData>& ActiveNodes)
{
    CachedVertices.Empty(ActiveNodes.Num() * 8);
    CachedTriangles.Empty(ActiveNodes.Num() * 36);
    CachedUVs.Empty(ActiveNodes.Num() * 24);
    CachedColors.Empty(ActiveNodes.Num() * 24);

    int32 VertexIndexOffset = 0;

    for (const FGridNodeData& Node : ActiveNodes)
    {
        // =========================================================
        // 【核心修复】：必须使用网格自身的基因 (Node.Level)！
        // =========================================================
        int32 CurrentNodeLevel = Node.Level; 
        
        // 动态获取当前网格的真实大小 (L20=61m, L21=30m, L22=15m...)
        double SpanMeters = UGeoSOTMath::GetGridSpanMeters(CurrentNodeLevel);

        // 使用当前网格的真实 Level 反向推导物理边界
        uint32 LatIdx = AngleToGridIndex(Node.CenterLat, CurrentNodeLevel);
        uint32 LonIdx = AngleToGridIndex(Node.CenterLon, CurrentNodeLevel);

        double MinLatDeg = GridIndexToBoundaryAngle(LatIdx, CurrentNodeLevel);
        double MaxLatDeg = GridIndexToBoundaryAngle(LatIdx + 1, CurrentNodeLevel);
        double MinLonDeg = GridIndexToBoundaryAngle(LonIdx, CurrentNodeLevel);
        double MaxLonDeg = GridIndexToBoundaryAngle(LonIdx + 1, CurrentNodeLevel);
        
        double MinAltVal = Node.CenterAlt - (SpanMeters * 0.5);
        double MaxAltVal = Node.CenterAlt + (SpanMeters * 0.5);

        // --- 下面的代码保持完全不变 ---
        FVector3d LLH[8] = {
            FVector3d(MinLonDeg, MinLatDeg, MinAltVal), FVector3d(MaxLonDeg, MinLatDeg, MinAltVal), 
            FVector3d(MinLonDeg, MaxLatDeg, MinAltVal), FVector3d(MaxLonDeg, MaxLatDeg, MinAltVal), 
            FVector3d(MinLonDeg, MinLatDeg, MaxAltVal), FVector3d(MaxLonDeg, MinLatDeg, MaxAltVal), 
            FVector3d(MinLonDeg, MaxLatDeg, MaxAltVal), FVector3d(MaxLonDeg, MaxLatDeg, MaxAltVal)  
        };

        FTransform ActorInvTransform = GetActorTransform().Inverse();
        FVector P[8];
        for (int k = 0; k < 8; ++k) {
            P[k] = ActorInvTransform.TransformPosition((FVector)CesiumGeoreference->TransformLongitudeLatitudeHeightPositionToUnreal(LLH[k]));
        }

        int32 FaceIdx[6][4] = { {0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 4, 5}, {2, 3, 6, 7}, {0, 2, 4, 6}, {1, 3, 5, 7} };
        FColor DefColor = FColor(0, 0, 0, 0);

        for (int face = 0; face < 6; ++face) {
            CachedVertices.Add(P[FaceIdx[face][0]]); CachedVertices.Add(P[FaceIdx[face][1]]); 
            CachedVertices.Add(P[FaceIdx[face][2]]); CachedVertices.Add(P[FaceIdx[face][3]]); 
            CachedUVs.Add(FVector2D(0, 0)); CachedUVs.Add(FVector2D(1, 0)); CachedUVs.Add(FVector2D(0, 1)); CachedUVs.Add(FVector2D(1, 1));
            CachedColors.Add(DefColor); CachedColors.Add(DefColor); CachedColors.Add(DefColor); CachedColors.Add(DefColor);

            CachedTriangles.Add(VertexIndexOffset + 0); CachedTriangles.Add(VertexIndexOffset + 2); CachedTriangles.Add(VertexIndexOffset + 1);
            CachedTriangles.Add(VertexIndexOffset + 1); CachedTriangles.Add(VertexIndexOffset + 2); CachedTriangles.Add(VertexIndexOffset + 3);
            VertexIndexOffset += 4;
        }
    }

    TArray<FVector> EmptyNormals; TArray<FProcMeshTangent> EmptyTangents;
    GridProcMesh->CreateMeshSection(0, CachedVertices, CachedTriangles, EmptyNormals, CachedUVs, CachedColors, EmptyTangents, false);
}

// =========================================================
// 极速颜色刷新 (每帧执行)
// =========================================================
void AGeoSOTGridRenderer::UpdateMeshColors(const TArray<FGridNodeData>& ActiveNodes, const FVector& CameraPos)
{
    bool bNeedsUpdate = false;
    double SafeRadiusSq = SafeVisibleRadius * SafeVisibleRadius;

    for (int32 i = 0; i < ActiveNodes.Num(); ++i)
    {
        const FGridNodeData& Node = ActiveNodes[i];
        
        FVector3d World3D = CesiumGeoreference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(Node.CenterLon, Node.CenterLat, Node.CenterAlt));
        double DistSq = FVector::DistSquaredXY((FVector)World3D, CameraPos);

        FColor TargetColor;

        // 1. 双层视窗逻辑：超出安全范围的网格，直接二元隐身
        if (DistSq > SafeRadiusSq) 
        {
            TargetColor = FColor(0, 0, 0, 0); 
        } 
        else 
        {

            if (Node.bIsObstacle) 
            {
                // 危险网格/禁飞区：醒目的红色，且几乎不透明 (Alpha = 230)
                TargetColor = FColor(255, 0, 0, 230);
            } 
            else 
            {
                // 安全网格，按照层级加深绿色实体感
                uint8 Alpha = 15 + (Node.Level - 20) * 45; 
                Alpha = FMath::Min((uint8)150, Alpha); // 防止超出最大透明度
                TargetColor = FColor(0, 255, 0, Alpha);
            }
        }

        // 3. 颜色覆写
        int32 ColorStartIdx = i * 24;
        if (CachedColors[ColorStartIdx] != TargetColor)
        {
            for (int v = 0; v < 24; ++v) CachedColors[ColorStartIdx + v] = TargetColor;
            bNeedsUpdate = true;
        }
    }

    if (bNeedsUpdate)
    {
        TArray<FVector> EmptyNormals; TArray<FProcMeshTangent> EmptyTangents;
        GridProcMesh->UpdateMeshSection(0, CachedVertices, EmptyNormals, CachedUVs, CachedColors, EmptyTangents);
    }
}

// =========================================================
// 文字 LOD (保持纯净)
// =========================================================
void AGeoSOTGridRenderer::InitializeTextPool()
{
    if (bIsPoolInitialized) return;
    for (int32 i = 0; i < MaxTextPoolSize; ++i) {
       FName CompName = MakeUniqueObjectName(this, UTextRenderComponent::StaticClass(), TEXT("PoolText"));
       UTextRenderComponent* TextComp = NewObject<UTextRenderComponent>(this, CompName);
       TextComp->RegisterComponent();
       TextComp->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
       TextComp->SetText(FText::GetEmpty()); TextComp->SetVisibility(false);
       TextComp->SetHorizontalAlignment(EHTA_Center); TextComp->SetVerticalAlignment(EVRTA_TextCenter); 
       TextPool.Add(TextComp);
    }
    bIsPoolInitialized = true;
}

struct FDistanceItem { double DistSq; const FGridNodeData* Info; bool operator<(const FDistanceItem& Other) const { return DistSq < Other.DistSq; } };

void AGeoSOTGridRenderer::UpdateTextLOD(const TArray<FGridNodeData>& ActiveNodes, const FVector& CameraPos)
{
    TArray<FDistanceItem> Candidates;
    double DistSqLimit = SafeVisibleRadius * SafeVisibleRadius;

    for (const FGridNodeData& Node : ActiveNodes) {
       FVector3d RealTimeWorld3D = CesiumGeoreference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(Node.CenterLon, Node.CenterLat, Node.CenterAlt));
       double DistSq = FVector::DistSquared((FVector)RealTimeWorld3D, CameraPos);
       
       // 只有内圈 (非透明) 的节点才配显示文字，节省性能
       if (DistSq < DistSqLimit) Candidates.Add({DistSq, &Node}); 
    }

    Candidates.Sort();
    int32 NumToDisplay = FMath::Min(Candidates.Num(), TextPool.Num());
    APlayerCameraManager* CameraManager = GetWorld()->HasBegunPlay() ? UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0) : nullptr;
    FRotator BillboardRot = CameraManager ? CameraManager->GetCameraRotation() : FRotator::ZeroRotator;
    BillboardRot.Yaw += 180.f; 

    for (int32 i = 0; i < TextPool.Num(); ++i) {
       UTextRenderComponent* TextComp = TextPool[i];
       if (i < NumToDisplay) {
          const FGridNodeData* TargetNode = Candidates[i].Info;
          FVector TextWorldLoc = (FVector)CesiumGeoreference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(TargetNode->CenterLon, TargetNode->CenterLat, TargetNode->CenterAlt));
          FString FinalDisplayText = FString::Printf(TEXT("%s\nS: %.2f"), *TargetNode->GeoSOTCode, TargetNode->Score);
          
          TextComp->SetText(FText::FromString(FinalDisplayText));
          TextComp->SetWorldLocation(TextWorldLoc); TextComp->SetWorldRotation(BillboardRot); 
          TextComp->SetWorldSize(TextWorldSize); 
          TextComp->SetTextRenderColor(TargetNode->Score < DataManager->Threshold_Theta ? FColor::Yellow : FColor::Green);
          TextComp->SetVisibility(true);
       } else { TextComp->SetVisibility(false); }
    }
}