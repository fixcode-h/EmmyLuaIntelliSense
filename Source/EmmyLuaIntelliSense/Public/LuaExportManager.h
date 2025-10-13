// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "EditorSubsystem.h"
#include "LuaExportManager.generated.h"

/**
 * 增量导出管理器
 * 负责监听UE反射代码变化并管理Lua文件的增量导出
 */
UCLASS()
class EMMYLUAINTELLISENSE_API ULuaExportManager : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    ULuaExportManager();

    // USubsystem interface
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /** 获取实例 */
    static ULuaExportManager* Get();

    /** 执行全量导出 */
    void ExportAll();

    /** 执行增量导出 */
    void ExportIncremental();

    /** 检查是否有待导出的变更 */
    bool HasPendingChanges() const;

    /** 获取待处理文件数量 */
    int32 GetPendingFilesCount() const;
    
    /** 获取待处理蓝图数量 */
    int32 GetPendingBlueprintsCount() const;
    
    /** 获取待处理原生类型数量 */
    int32 GetPendingNativeTypesCount() const;
    
    /** 获取待处理核心文件数量（UE.lua, UE4.lua, UnLua.lua） */
    int32 GetPendingCoreFilesCount() const;

    /** 清除待导出的变更记录 */
    void ClearPendingChanges();
    
    /** 扫描现有资源并添加到待导出列表 */
    void ScanExistingAssets();
    
    /** 异步扫描现有资源并添加到待导出列表 */
    void ScanExistingAssetsAsync();
    
    /** 取消异步扫描 */
    void CancelAsyncScan();
    
    /** 启动分帧处理 */
    void StartFramedProcessing();
    
    /** 分帧处理单步执行 */
    bool ProcessFramedStep();
    
    /** Timer包装函数 */
    void ProcessFramedStepWrapper();
    
    /** 完成分帧处理 */
    void CompleteFramedProcessing();

private:
    // ========== 成员变量 ==========
    
    /** 是否已初始化 */
    bool bInitialized;

    /** 输出目录 */
    FString OutputDir;

    /** 待导出的蓝图资源 */
    TSet<FString> PendingBlueprints;

    /** 待导出的原生类型 */
    TSet<TWeakObjectPtr<const UField>> PendingNativeTypes;

    /** 导出状态缓存文件路径 */
    FString ExportCacheFilePath;

    /** 已导出文件的哈希值缓存 */
    TMap<FString, FString> ExportedFilesHashCache;
    
    /** UField的Hash缓存 */
    mutable TMap<const UField*, FString> FieldHashCache;
    
    /** UField Hash缓存的时间戳 */
    mutable TMap<const UField*, double> FieldHashCacheTimestamp;
    
    /** 异步扫描相关 */
    bool bIsAsyncScanningInProgress;
    
    /** 扫描进度通知 */
    TSharedPtr<class SNotificationItem> ScanProgressNotification;
    
    /** 扫描是否被用户取消 */
    bool bScanCancelled;
    
    /** 分帧处理相关 */
    TArray<FAssetData> ScannedBlueprintAssets;
    TArray<const UField*> ScannedNativeTypes;
    bool bIsFramedProcessingInProgress;
    int32 CurrentBlueprintIndex;
    int32 CurrentNativeTypeIndex;
    FTimerHandle FramedProcessingTimerHandle;

    // ========== 核心导出功能 ==========
    
    /** 添加蓝图到待导出列表 */
    bool AddToPendingBlueprints(const FAssetData& AssetData);
    
    /** 导出单个蓝图 */
    void ExportBlueprint(const UBlueprint* Blueprint);

    /** 导出单个原生类型 */
    void ExportNativeType(const UField* Field);

    /** 导出UE核心类型 */
    void ExportUETypes(const TArray<const UField*>& Types);

    /** 收集所有原生类型 */
    void CollectNativeTypes(TArray<const UField*>& Types);

    // ========== 资源判断和验证 ==========
    
    /** 判断是否为蓝图资源 */
    static bool IsBlueprint(const FAssetData& AssetData);

    /** 判断蓝图是否应该导出 */
    bool ShouldExportBlueprint(const FAssetData& AssetData, bool bLoad = false) const;

    /** 验证UField是否有效且名称合法 */
    bool IsValidFieldForExport(const UField* Field, FString& OutFieldName) const;

    /** 检查路径是否应该被排除在导出之外 */
    bool ShouldExcludeFromExport(const FString& AssetPath) const;

    // ========== 文件操作 ==========
    
    /** 保存文件 */
    void SaveFile(const FString& ModuleName, const FString& FileName, const FString& Content);

    /** 删除文件 */
    void DeleteFile(const FString& ModuleName, const FString& FileName);

    /** 获取输出目录 */
    FString GetOutputDirectory() const;

    /** 拷贝UELib文件夹到输出目录 */
    void CopyUELibFolder() const;
    
    // ========== 缓存管理 ==========
    
    /** 加载导出缓存 */
    void LoadExportCache();

    /** 保存导出缓存 */
    void SaveExportCache();

    /** 检查文件是否需要重新导出（基于哈希值） */
    bool ShouldReexport(const FString& AssetPath, const FString& AssetFilePath) const;
    
    /** 检查文件是否需要重新导出（基于哈希值） */
    bool ShouldReexportByHash(const FString& AssetPath, const FString& AssetHash) const;
    
    /** 更新导出缓存中的Hash值 */
    void UpdateExportCacheByHash(const FString& AssetPath, const FString& AssetHash);
    
    /** 获取UField的缓存哈希值（带缓存优化） */
    FString GetCachedFieldHash(const UField* Field) const;
    
    /** 清理过期的Hash缓存 */
    void CleanupExpiredHashCache() const;
    
    /** 缓存失效时间（秒） */
    static constexpr double HASH_CACHE_EXPIRE_TIME = 300.0; // 5分钟

    // ========== 哈希计算 ==========
    
    /** 计算文件的哈希值 */
    FString CalculateFileHash(const FString& FilePath) const;
    
    /** 计算UE类结构签名哈希值 */
    FString CalculateClassStructureHash(const UClass* Class) const;
    
    /** 获取资源的哈希值 */
    FString GetAssetHash(const FString& AssetPath) const;
    
    /** 获取UField的哈希值（用于原生类型） */
    FString GetAssetHash(const UField* Field) const;
    


    // ========== 辅助功能 ==========
    
    /** 生成UnLua特定的定义 */
    FString GenerateUnLuaDefinitions() const;

    /** 从JSON文件加载排除路径列表 */
    void LoadExcludedPathsFromFile(TArray<FString>& OutExcludedPaths) const;
    
    // ========== 异步扫描辅助功能 ==========
    
    /** 异步扫描完成回调 */
    void OnAsyncScanCompleted(const TArray<FAssetData>& BlueprintAssets, const TArray<const UField*>& NativeTypes);
    
    /** 分批处理蓝图资源 */
    void ProcessBlueprintsInBatches(const TArray<FAssetData>& BlueprintAssets, int32 BatchSize = 50);
    
    /** 分批处理原生类型 */
    void ProcessNativeTypesInBatches(const TArray<const UField*>& NativeTypes, int32 BatchSize = 100);
    
    /** 处理单批蓝图资源 */
    void ProcessBlueprintBatch(const TArray<FAssetData>& BatchAssets, int32 BatchIndex, int32 TotalBatches);
    
    /** 处理单批原生类型 */
    void ProcessNativeTypeBatch(const TArray<const UField*>& BatchTypes, int32 BatchIndex, int32 TotalBatches);
};