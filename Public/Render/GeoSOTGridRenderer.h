#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Core/GeoSOTDataManager.h"
#include "GeoSOTGridRenderer.generated.h"

// 前置声明
class ACesiumGeoreference;
class UGeoSOTRiskEnvironment;
class UGeoSOTDataManager;

UCLASS()
class GEOSOTSYSTEM_API AGeoSOTGridRenderer : public AActor
{
    GENERATED_BODY()

public:
    AGeoSOTGridRenderer();

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual bool ShouldTickIfViewportsOnly() const override;

public:
    // --- 核心配置 ---
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Reference")
    ACesiumGeoreference* CesiumGeoreference;

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Settings")
    int32 TargetLevel = 20;

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Settings")
    bool bEnableAdaptiveFission = true;

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Settings")
    int32 MaxFissionLevel = 23;

    // --- 边界设置 ---
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Bounds")
    double MinLat;
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Bounds")
    double MaxLat;
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Bounds")
    double MinLon;
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Bounds")
    double MaxLon;
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Bounds")
    double MinAlt = 0.0;
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Bounds")
    double MaxAlt = 500.0;

    // --- 渲染控制 ---
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Render")
    double SensingRadius = 2000.0;      // 索要数据的半径

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Render")
    double SafeVisibleRadius = 800.0;   // 颜色渲染可见的半径

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Render")
    float AnchorUpdateThreshold = 500.0; // 锚点更新阈值

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Text")
    float TextWorldSize = 50.0;

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Text")
    int32 MaxTextPoolSize = 100;
    
    UPROPERTY(BlueprintReadOnly, Category = "GeoSOT")
    UGeoSOTDataManager* DataManager;
    
    /** * 在细节面板生成一个按钮：点击即刻导出当前所有网格数据
     * Category 填 "GeoSOT|Export" 会在右侧面板生成专门的分组
     */
    UFUNCTION(BlueprintCallable, CallInEditor, Category = "GeoSOT|Export")
    void TriggerDataExport();

    /** * 在细节面板生成一个按钮：清空当前缓存的网格
     */
    UFUNCTION(BlueprintCallable, CallInEditor, Category = "GeoSOT|Export")
    void TriggerClearMemory();
    /** 给导出的区域起个名字，可以直接在面板里改 */
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Export")
    FString ExportAreaName = TEXT("Area_001");
    
    // 1. 在面板上选择模式的下拉菜单
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Control")
    EGridDivisionMode DivisionMode = EGridDivisionMode::Adaptive;

    // 2. 在面板上输入目标层级的数字（只有选了完全划分才有用）
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Control")
    int32 TargetUniformLevel = 22;

    // 3. 核心魔法：这个宏会在引擎的细节面板生成一个叫 "Apply Division Mode" 的按钮！
    UFUNCTION(CallInEditor, Category = "GeoSOT|Control")
    void ApplyDivisionMode();


private:
    /** 重新构建几何体网格 */
    void RebuildMeshGeometry(const TArray<FGridNodeData>& ActiveNodes);

    /** 刷新网格颜色（每帧） */
    void UpdateMeshColors(const TArray<FGridNodeData>& ActiveNodes, const FVector& CameraPos);

    /** 初始化文字组件池 */
    void InitializeTextPool();

    /** 刷新文字 LOD */
    void UpdateTextLOD(const TArray<FGridNodeData>& ActiveNodes, const FVector& CameraPos);

    /** 获取当前活动相机位置 */
    bool GetCurrentCameraLocation(FVector& OutLocation) const;
    
    
private:
    UPROPERTY(VisibleAnywhere)
    UProceduralMeshComponent* GridProcMesh;


    

    UPROPERTY()
    UGeoSOTRiskEnvironment* RiskEnv;

    // --- 渲染缓存数据 ---
    TArray<FVector> CachedVertices;
    TArray<int32> CachedTriangles;
    TArray<FVector2D> CachedUVs;
    TArray<FColor> CachedColors;

    UPROPERTY()
    TArray<UTextRenderComponent*> TextPool;

    FVector LastQueryAnchor = FVector::ZeroVector;
    bool bIsAnchorInitialized = false;
    bool bIsPoolInitialized = false;
    int32 LastRenderedNodeCount = -1;
};