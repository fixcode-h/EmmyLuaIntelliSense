// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "EditorSubsystem.h"
#include "LuaExportManager.generated.h"

class IDirectoryWatcher;

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

    /** 关闭导出管理器 */
    void Shutdown();

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

private:

    /** 是否已初始化 */
    bool bInitialized;

    /** 输出目录 */
    FString OutputDir;

    /** 待导出的蓝图资源 */
    TSet<FString> PendingBlueprints;

    /** 待导出的原生类型 */
    TSet<TWeakObjectPtr<const UField>> PendingNativeTypes;

    /** 文件监听器 */
    IDirectoryWatcher* DirectoryWatcher;

    /** 监听的目录和对应的句柄 */
    TMap<FString, FDelegateHandle> WatchedDirectories;

    /** 上次检查原生类型的时间 */
    double LastNativeTypesCheckTime;

    /** 导出状态缓存文件路径 */
    FString ExportCacheFilePath;

    /** 已导出文件的时间戳缓存 */
    TMap<FString, FDateTime> ExportedFilesCache;
    
    /** 已导出文件的哈希值缓存 */
    TMap<FString, FString> ExportedFilesHashCache;

    bool AddToPendingBlueprints(const FAssetData& AssetData);
    
    /** 导出单个蓝图 */
    void ExportBlueprint(const UBlueprint* Blueprint);

    /** 导出单个原生类型 */
    void ExportNativeType(const UField* Field);

    /** 导出UE核心类型 */
    void ExportUETypes(const TArray<const UField*>& Types);

    /** 收集所有原生类型 */
    void CollectNativeTypes(TArray<const UField*>& Types);

    /** 判断是否为蓝图资源 */
    static bool IsBlueprint(const FAssetData& AssetData);

    /** 判断蓝图是否应该导出 */
    bool ShouldExportBlueprint(const FAssetData& AssetData, bool bLoad = false) const;

    /** 保存文件 */
    void SaveFile(const FString& ModuleName, const FString& FileName, const FString& Content);

    /** 删除文件 */
    void DeleteFile(const FString& ModuleName, const FString& FileName);

    /** 获取输出目录 */
    FString GetOutputDirectory() const;

    /** 初始化文件监听器 */
    void InitializeFileWatcher();

    /** 关闭文件监听器 */
    void ShutdownFileWatcher();

    /** 文件变化回调 */
    void OnDirectoryChanged(const TArray<struct FFileChangeData>& FileChanges);

    /** 检查原生类型变化 */
    void CheckNativeTypesChanges();

    /** 添加监听目录 */
    void AddWatchDirectory(const FString& Directory);
    
    /** 生成UnLua特定的定义 */
    FString GenerateUnLuaDefinitions() const;

    /** 加载导出缓存 */
    void LoadExportCache();

    /** 保存导出缓存 */
    void SaveExportCache();

    /** 检查文件是否需要重新导出 */
    bool ShouldReexport(const FString& AssetPath, const FDateTime& AssetModifyTime) const;
    
    /** 检查文件是否需要重新导出（直接传入文件路径） */
    bool ShouldReexport(const FString& AssetPath, const FString& AssetFilePath) const;

    /** 更新文件导出缓存 */
    void UpdateExportCache(const FString& AssetPath, const FDateTime& ExportTime);

    /** 获取资源的修改时间 */
    FDateTime GetAssetModifyTime(const FString& AssetPath) const;
    
    /** 计算文件的哈希值 */
    FString CalculateFileHash(const FString& FilePath) const;
    
    /** 获取资源的哈希值 */
    FString GetAssetHash(const FString& AssetPath) const;
    
    /** 检查文件是否需要重新导出（基于哈希值） */
    bool ShouldReexportByHash(const FString& AssetPath, const FString& AssetHash) const;
    
    /** 更新文件导出缓存（基于哈希值） */
    void UpdateExportCacheByHash(const FString& AssetPath, const FString& AssetHash);

    /** 验证UField是否有效且名称合法 */
    bool IsValidFieldForExport(const UField* Field, FString& OutFieldName) const;

    /** 检查路径是否应该被排除在导出之外 */
    bool ShouldExcludeFromExport(const FString& AssetPath) const;

    /** 从JSON文件加载排除路径列表 */
    void LoadExcludedPathsFromFile(TArray<FString>& OutExcludedPaths) const;
};