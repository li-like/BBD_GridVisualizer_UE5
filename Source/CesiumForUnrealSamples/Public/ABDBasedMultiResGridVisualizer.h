// Copyright 2020-2021 CesiumGS, Inc. and Contributors

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CesiumGeoreference.h"
#include "ABDBasedMultiResGridVisualizer.generated.h"


USTRUCT(BlueprintType)
struct FGridResolutionRule
{
	GENERATED_BODY()
    /** 起始高度 (米) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid Rules")
    double MinAltitude;

    /** 结束高度 (米) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid Rules")
    double MaxAltitude;

    /** 稀疏区域(无建筑)采用的层级 (例如 L6) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid Rules")
    int32 SparseLevel;

    /** 密集区域(有建筑)采用的层级 (例如 L9) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid Rules")
    int32 DenseLevel;

    // 默认构造函数
    FGridResolutionRule()
        : MinAltitude(10.0), MaxAltitude(120.0), SparseLevel(6), DenseLevel(7) {}

    // 带参构造函数
    FGridResolutionRule(double InMin, double InMax, int32 InSparse, int32 InDense)
        : MinAltitude(InMin), MaxAltitude(InMax), SparseLevel(InSparse), DenseLevel(InDense) {}
};


UCLASS()
class CESIUMFORUNREALSAMPLES_API AABDBasedMultiResGridVisualizer : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AABDBasedMultiResGridVisualizer();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

    // --- 核心组件依赖 ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Beidou Grid | Core")
    ACesiumGeoreference* CesiumGeoreference;

    // --- 多分辨率配置 ---

    /** * @brief 存储所有的多分辨率规则列表。
     * 在编辑器中，你可以看到这个数组并添加/修改规则。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Beidou Grid | Rules")
    TArray<FGridResolutionRule> ResolutionRules;

    // --- 区域控制 (仅用于演示/测试范围) ---
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Debug")
    double MinLongitude = 144.945;
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Debug")
    double MaxLongitude = 144.960;
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Debug")
    double MinLatitude = -37.815;
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Debug")
    double MaxLatitude = -37.800;

    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Display")
    float GlobalLineThickness = 0.2f;

    // --- 【新增】材质接口 ---
    // 编译后，请在编辑器中将 MI_Grid_Low 等材质拖入这些槽位

    /** L1-L4 使用的材质 (通常设为绿色) */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Materials")
    UMaterialInterface* MaterialLow;

    /** L6 使用的材质 (通常设为黄色) */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Materials")
    UMaterialInterface* MaterialMid;

    /** L7-L10 使用的材质 (通常设为红色) */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Materials")
    UMaterialInterface* MaterialHigh;

    /** 空闲区域材质 (绿色) */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Materials")
    UMaterialInterface* MaterialFree;

    /** L7 障碍区域材质 (红色 - 用于调试碰撞) */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Materials")
    UMaterialInterface* MaterialOccupied;

private:
    /**
     * @brief 初始化默认的划分规则 (如果用户没有在编辑器里设置的话)
     */
    void InitDefaultRules();

    // 上次更新网格时的位置
    FVector LastUpdateLocation;

    // 是否是第一次运行
    bool bInitialized = false;

    // 重新生成的距离阈值 (例如移动 50米才刷新一次网格)
    const float UpdateDistanceThreshold = 5000.0f; // 5000cm = 50m

    /**
     * @brief 核心决策函数：根据高度和密度获取目标层级
     * @param Altitude 当前高度 (米)
     * @param bIsDense 当前区域是否密集 (true=有建筑)
     * @return int32 推荐的北斗网格层级
     */
    int32 GetTargetLevel(double Altitude, bool bIsDense) const;

    /**
     * @brief (预留) 扫描并绘制网格的主入口
     */
    void DrawMultiResGrid();

    // 【新增】专门用于 L7 分类显示的组件
    UPROPERTY()
    UInstancedStaticMeshComponent* ISM_L7_Free;

    UPROPERTY()
    UInstancedStaticMeshComponent* ISM_L7_Occupied;


    /**
     * @brief [核心标准] 获取北斗网格从 CurrentLevel 到下一级的拆分比例 (3D)
     * @param CurrentLevel 当前层级
     * @param OutLonDiv 经度方向切几份
     * @param OutLatDiv 纬度方向切几份
     * @param OutAltDiv [新增] 高度方向切几份
     * @return 是否成功获取
     */
    bool GetBeidouSplitRatio(int32 CurrentLevel, int32& OutLonDiv, int32& OutLatDiv, int32& OutAltDiv) const;

    /**
     * @brief [环境感知] 检测指定区域是否包含建筑物 (密集区)
     * @param Center 网格中心 (Unreal坐标)
     * @param Extent 网格半长 (Unreal尺寸)
     * @return true=密集/有障碍物, false=稀疏/空旷
     */
    bool IsRegionDense(const FVector& Center, const FVector& Extent) const;

    /**
     * @brief [递归核心] 处理网格单元：决定是绘制它，还是继续切分它
     * * @param LonRange X, Y: 当前网格的起始经度、结束经度
     * @param LatRange X, Y: 当前网格的起始纬度、结束纬度
     * @param AltRange X, Y: 当前高度层的底部高度、顶部高度 (由 HeightRules 决定)
     * @param CurrentLevel 当前递归到的层级
     * @param TargetLevel  当前状态下的目标层级 (递归的终点)
     */
    void ProcessGridRecursive(
        const FVector2D& LonRange,
        const FVector2D& LatRange,
        const FVector2D& AltRange,
        int32 CurrentLevel,
        int32 TargetLevel,
        const FVector& ViewerPos
    );

    // 辅助：用于临时转换坐标
    FVector GetUnrealLocation(double Longitude, double Latitude, double Height) const;

    // 【新增】存储 10 个层级对应的 ISM 组件
    UPROPERTY()
    TArray<UInstancedStaticMeshComponent*> LevelISMs;

    // 【新增】初始化 ISM 组件的函数
    void SetupISMComponents();


    /**
     * @brief [核心升级] 计算区域内的建筑密度
     * 使用蒙特卡洛方法，在包围盒内随机采样，估算被占据的体积比例。
     * @param Center 中心点
     * @param Extent 半长宽高
     * @param SampleCount 采样点数量 (建议 5-10，要在性能和精度间取舍)
     * @return 密度值 (0.0 ~ 1.0)
     */
    float CalculateRegionDensity(const FVector& Center, const FVector& Extent, int32 SampleCount = 8) const;

};
