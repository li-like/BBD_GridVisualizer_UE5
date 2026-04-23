#include "Core/GeoSOTMath.h"
#include "Math/UnrealMathUtility.h"

// ---------------------------------------------------------
// 路线 A 工具：二维位交叉 (生成四进制字符串)
// ---------------------------------------------------------
static FString InterleaveToBase4String(uint32 LatVal, uint32 LonVal, int32 Bits)
{
	FString Result = TEXT("");
	// 从高位到低位逐位提取并交叉
	for (int i = Bits - 1; i >= 0; --i)
	{
		uint32 bLat = (LatVal >> i) & 1;
		uint32 bLon = (LonVal >> i) & 1;
		// 纬度在前，经度在后，合并为 0, 1, 2, 3
		uint32 QuadVal = (bLat << 1) | bLon; 
		Result += FString::FromInt(QuadVal);
	}
	return Result;
}

// ---------------------------------------------------------
// 路线 B 工具：三维位交叉与解码
// ---------------------------------------------------------
static uint64 SpreadBits3D_Safe(uint32 Value, int32 Level)
{
	uint64 Result = 0;
	for (int i = 0; i < Level; ++i)
	{
		if (Value & (1 << i)) Result |= (1ULL << (i * 3));
	}
	return Result;
}

static uint32 CompactBits3D_Safe(uint64 Code, int32 Level, int32 Offset)
{
	uint32 Result = 0;
	for (int i = 0; i < Level; ++i)
	{
		if (Code & (1ULL << (i * 3 + Offset))) Result |= (1 << i);
	}
	return Result;
}

// =========================================================
// 路线 A 实现：生成国标 32 位四段式字符串 
// 格式: Gddddddddd-mmmmmm-ssssss.uuuuuuuuuuu
// =========================================================
FString UGeoSOTMath::EncodeToGeoSOTString(double Lat, double Lon, double Alt, int32 Level, bool bIncludeAlt)
{
    // 限制最大层级为国标上限 32 层
    Level = FMath::Clamp(Level, 0, 32);

    double AbsLat = FMath::Abs(Lat);
    double AbsLon = FMath::Abs(Lon);

    // 提前算出纯净的整数度数（用于后续算余数，防止混入符号位）
    uint32 PureLatD = FMath::FloorToInt(AbsLat);
    uint32 PureLonD = FMath::FloorToInt(AbsLon);

    // =========================================================
    // 1. 度 (Degree) - [第 0 ~ 9 层]，最多 9 位
    // =========================================================
    uint32 LatD = PureLatD;
    uint32 LonD = PureLonD;
    // 核心魔法：将半球符号存入第 9 位 (1 << 8)
    if (Lat < 0) LatD |= (1 << 8);
    if (Lon < 0) LonD |= (1 << 8);
    
    int32 DegreeBits = FMath::Clamp(Level, 0, 9);
    FString FinalStr = FString::Printf(TEXT("G%s"), *InterleaveToBase4String(LatD, LonD, DegreeBits));

    // 如果目标层级在度级内，直接返回
    if (Level <= 9) return bIncludeAlt ? FinalStr + FString::Printf(TEXT("+H%010d"), FMath::FloorToInt(Alt)) : FinalStr;

    // =========================================================
    // 2. 分 (Minute) - [第 10 ~ 15 层]，最多 6 位
    // =========================================================
    // 国标划分：1度 = 64分
    double LatRemM_val = (AbsLat - PureLatD) * 64.0;
    double LonRemM_val = (AbsLon - PureLonD) * 64.0;
    uint32 LatM = FMath::FloorToInt(LatRemM_val);
    uint32 LonM = FMath::FloorToInt(LonRemM_val);
    
    int32 MinuteBits = FMath::Clamp(Level - 9, 0, 6);
    FinalStr += FString::Printf(TEXT("-%s"), *InterleaveToBase4String(LatM, LonM, MinuteBits));

    if (Level <= 15) return bIncludeAlt ? FinalStr + FString::Printf(TEXT("+H%010d"), FMath::FloorToInt(Alt)) : FinalStr;

    // =========================================================
    // 3. 秒 (Second) - [第 16 ~ 21 层]，最多 6 位
    // =========================================================
    // 国标划分：1分 = 64秒
    double LatRemS_val = (LatRemM_val - LatM) * 64.0;
    double LonRemS_val = (LonRemM_val - LonM) * 64.0;
    uint32 LatS = FMath::FloorToInt(LatRemS_val);
    uint32 LonS = FMath::FloorToInt(LonRemS_val);
    
    int32 SecondBits = FMath::Clamp(Level - 15, 0, 6);
    FinalStr += FString::Printf(TEXT("-%s"), *InterleaveToBase4String(LatS, LonS, SecondBits));

    if (Level <= 21) return bIncludeAlt ? FinalStr + FString::Printf(TEXT("+H%010d"), FMath::FloorToInt(Alt)) : FinalStr;

    // =========================================================
    // 4. 秒以下 (Sub-second) - [第 22 ~ 32 层]，最多 11 位
    // =========================================================
    // 国标划分：秒以下采用 2 的 n 次方细分
    int32 ExtBits = FMath::Clamp(Level - 21, 0, 11);
    double Multiplier = FMath::Pow(2.0, ExtBits); 
    
    double LatRemU_val = (LatRemS_val - LatS) * Multiplier;
    double LonRemU_val = (LonRemS_val - LonS) * Multiplier;
    uint32 LatU = FMath::FloorToInt(LatRemU_val);
    uint32 LonU = FMath::FloorToInt(LonRemU_val);
    
    // 秒以下编码以 "." 隔开
    FinalStr += FString::Printf(TEXT(".%s"), *InterleaveToBase4String(LatU, LonU, ExtBits));

    // =========================================================
    // 5. 附加高度
    // =========================================================
    if (bIncludeAlt)
    {
       FinalStr += FString::Printf(TEXT("+H%010d"), FMath::FloorToInt(Alt)); 
    }

    return FinalStr;
}

// =========================================================
// 路线 B 实现：底层 3D Morton 码 (A*算法与数据库使用)
// =========================================================
int64 UGeoSOTMath::EncodeToMorton3D(double Lat, double Lon, double Alt, int32 Level)
{
	if (Level > 21) return 0;

	// 映射到 [0, 512] 空间并转为 32 位整数
	uint32 LonInt = static_cast<uint32>((Lon + 256.0) * 8388608.0);
	uint32 LatInt = static_cast<uint32>((Lat + 256.0) * 8388608.0);
	double AltDegree = Alt / 111319.490793;
	uint32 AltInt = static_cast<uint32>((AltDegree + 256.0) * 8388608.0);

	// 截取前 Level 位并移动到底部
	int32 Shift = 32 - Level;
	uint32 LonEff = LonInt >> Shift;
	uint32 LatEff = LatInt >> Shift;
	uint32 AltEff = AltInt >> Shift;

	// 3D 交叉: 高(H) 纬(B) 经(L)
	uint64 Code = (SpreadBits3D_Safe(AltEff, Level) << 2) |
	              (SpreadBits3D_Safe(LatEff, Level) << 1) |
	               SpreadBits3D_Safe(LonEff, Level);

	return Code;
}

void UGeoSOTMath::DecodeFromMorton3D(int64 Code, int32 Level, double& OutLat, double& OutLon, double& OutAlt)
{
	if (Level > 21) return;

	uint32 LonEff = CompactBits3D_Safe(Code, Level, 0);
	uint32 LatEff = CompactBits3D_Safe(Code, Level, 1);
	uint32 AltEff = CompactBits3D_Safe(Code, Level, 2);

	int32 Shift = 32 - Level;
	uint32 CenterOffset = (Shift > 0) ? (1 << (Shift - 1)) : 0;
	
	uint32 LonInt = (LonEff << Shift) | CenterOffset;
	uint32 LatInt = (LatEff << Shift) | CenterOffset;
	uint32 AltInt = (AltEff << Shift) | CenterOffset;

	OutLon = (static_cast<double>(LonInt) / 8388608.0) - 256.0;
	OutLat = (static_cast<double>(LatInt) / 8388608.0) - 256.0;
	double AltDegree = (static_cast<double>(AltInt) / 8388608.0) - 256.0;
	OutAlt = AltDegree * 111319.490793;
}

void UGeoSOTMath::GetFissionChildren(int64 ParentCode, TArray<int64>& OutChildren)
{
	OutChildren.Empty(8);
	for (int64 k = 0; k < 8; ++k) OutChildren.Add((ParentCode << 3) | k);
}

double UGeoSOTMath::GetGridSpanMeters(int32 Level)
{
	if (Level < 0 || Level > 32) return 0.0;
	switch (Level) {
		case 15: return 1800.0; case 16: return 989.5; case 17: return 494.7;
		case 18: return 247.4;  case 19: return 123.7; case 20: return 61.8;
		case 21: return 30.9;   case 22: return 15.5;  case 23: return 7.7;
		default:
			if (Level > 23) return 30.9 / FMath::Pow(2.0, Level - 21);
			return 111319.49 / FMath::Pow(2.0, Level - 9); 
	}
}