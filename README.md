# UE5 + Cesium + GeoSOT 网格系统开发技术复盘

## 1. 项目背景
在 Unreal Engine 5 环境下，结合 Cesium for Unreal 插件，实现基于 GeoSOT（全球剖分网格）的低空经济数字孪生系统。核心需求是实时渲染海量网格，并动态检测网格内是否存在建筑物障碍。

## 2. 遇到的核心问题与解决方案

### 2.1. 物理碰撞体加载延迟 (The "All Green" Issue)
* **现象**：游戏运行初期（`BeginPlay`），所有网格显示为绿色（空闲），但肉眼可见有建筑物。
* **原因**：Cesium 3D Tiles 采用流式异步加载。在第一帧时，视觉模型可能刚开始流送，物理碰撞网格（Physics Mesh）尚未生成，射线检测或重叠检测直接穿透。
* **解决方案**：
    * **配置层**：在 Cesium3DTileset 中必须勾选 `Create Physics Meshes`。
    * **逻辑层**：放弃在 `BeginPlay` 一次性检测。改为**实时监控机制**，或等待 Cesium 加载进度（`ComputeLoadProgress`）完成后再触发检测。

### 2.2. 渲染性能瓶颈 (Rendering Optimization)
* **现象**：最初构想使用 `ProceduralMesh` 或绘制 12 条 Debug Line 来表现一个网格，性能开销巨大。
* **原因**：Draw Call 过多，且几何体生成复杂。
* **解决方案**：
    * **ISM 技术**：使用 `InstancedStaticMeshComponent` (ISM)，将成千上万个网格合并为极少量的 Draw Call。
    * **Shader 线框**：放弃“用12根圆柱体拼凑边框”的方案。改用**1个标准 Cube 模型 + 半透明自发光边缘材质**。
    * **优势**：实例数量减少 12 倍，且视觉效果更具科技感（半透明辉光）。

### 2.3. 动态检测优先级 (Proximity-Frequency Detection)
* **现象**：快速飞行接近建筑物时，网格变红有明显延迟（穿模）；或者站立不动时，新加载的建筑物不会触发网格变色。
* **原因**：单一频率的 `Tick` 检测无法兼顾性能与响应速度。
* **解决方案**：**“近快远慢”分级策略**。
    * **近处（如 500m 内）**：每 0.1s 检测一次（高频），保证飞行安全。
    * **远处（如 500m 外）**：每 1.0s 检测一次（低频），节省 CPU。

### 2.4. 地理空间偏移与畸变 (The "Offset & Distortion" Issue)
* **现象**：网格与建筑物墙面不平行（歪斜），且网格之间有重叠或缝隙，看起来像是有偏移。
* **原因**：
    * **旋转丢失**：使用了 `FRotator::ZeroRotator`。UE 的坐标轴是直的，地球表面的经纬网格是随曲率倾斜的。
    * **AABB 虚胖**：使用 `(Max - Min).GetAbs()` 计算尺寸。将一个“斜”的地理网格强行塞进一个“正”的物理盒子，导致尺寸膨胀。
* **解决方案**：**构建局部切线坐标系 (Local Tangent Plane)**。
    * 利用 `LlhToUnreal` 计算出该网格中心的 **北(North)** 和 **东(East)** 向量。
    * 使用 `FRotationMatrix::MakeFromXY(North, East)` 计算出网格的真实物理旋转。
    * 直接测量向量长度作为物理尺寸，而非坐标差绝对值。

### 2.5. 物理检测误报 (False Positives)
* **现象**：明明网格在空地上，却显示为红色（障碍）。
* **原因**：使用了默认的 `FCollisionShape::MakeBox` 进行检测，这会生成**轴对齐包围盒 (AABB)**。对于旋转了的网格，AABB 会比实际体积大很多，导致误触周围物体。
* **解决方案**：**OBB (Oriented Bounding Box) 检测**。
    * 使用 `World->OverlapBlockingTestByChannel`。
    * **关键点**：必须传入计算好的 `FQuat Rotation`，确保物理检测框与渲染框一样也是“斜”着的，实现严丝合缝的检测。

### 2.6. 坐标系不一致 (World vs Local)
* **现象**：网格位置整体偏移，或者当 Cesium 移动世界原点（Rebase）时网格乱飞。
* **原因**：`LlhToUnreal` 计算出的是**世界绝对坐标**，但 ISM 组件期待的是**相对 Actor 的本地坐标**。
* **解决方案**：**坐标分离原则**。
    * **物理层**：使用 `World Transform` 进行碰撞检测（因为 Cesium Tiles 在世界空间）。
    * **渲染层**：使用 `Actor->InverseTransformPosition()` 将世界坐标转为 **Local Transform** 传给 ISM。

## 3. 关键代码逻辑摘要

```cpp
// 1. 构建局部切线系 (解决旋转与尺寸)
FVector DirNorth = (NorthPos_World - CenterPos_World);
FVector DirEast  = (EastPos_World - CenterPos_World);
FQuat GridQuat_World = FRotationMatrix::MakeFromXY(DirNorth, DirEast).ToQuat();

// 2. 坐标空间分离 (解决偏移)
// 物理检测用 World
bool bHit = World->OverlapBlockingTestByChannel(CenterPos_World, GridQuat_World, ...);
// 渲染用 Local
FVector CenterPos_Local = ActorTransform.InverseTransformPosition(CenterPos_World);
FQuat GridQuat_Local = ActorTransform.InverseTransformRotation(GridQuat_World);
ISM->AddInstance(FTransform(GridQuat_Local, CenterPos_Local, Scale));