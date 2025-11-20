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

private:
    bool                                    bInitialized;                                    // 是否已初始化
    FString                                 OutputDir;                                       // 输出目录
    TSet<FString>                           PendingBlueprints;                              // 待导出的蓝图资源
    TSet<TWeakObjectPtr<const UField>>      PendingNativeTypes;                              // 待导出的原生类型
    FString                                 ExportCacheFilePath;                             // 导出状态缓存文件路径
    TMap<FString, FString>                  ExportedFilesHashCache;                         // 已导出文件的哈希值缓存
    mutable TMap<const UField*, FString>    FieldHashCache;                                  // UField的Hash缓存
    mutable TMap<const UField*, double>    FieldHashCacheTimestamp;                         // UField Hash缓存的时间戳
    bool                                    bIsAsyncScanningInProgress;                      // 异步扫描相关
    TSharedPtr<class SNotificationItem>     ScanProgressNotification;                        // 扫描进度通知
    bool                                    bScanCancelled;                                  // 扫描是否被用户取消
    TArray<FAssetData>                      ScannedBlueprintAssets;                         // 分帧处理相关
    TArray<const UField*>                   ScannedNativeTypes;                             // 分帧处理相关
    bool                                    bIsFramedProcessingInProgress;                   // 分帧处理相关
    int32                                   CurrentBlueprintIndex;                           // 分帧处理相关
    int32                                   CurrentNativeTypeIndex;                          // 分帧处理相关
    FTimerHandle                            FramedProcessingTimerHandle;                     // 分帧处理相关

    // 缓存失效时间（秒）
    static constexpr double HASH_CACHE_EXPIRE_TIME = 300.0; // 5分钟

public:
    ULuaExportManager();

    // Begin USubsystem
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    // End USubsystem

    // 获取实例
    static ULuaExportManager* Get();

    // ---------------------------------------------------------
    // 导出相关
    // ---------------------------------------------------------
    void            ExportAll();                                                 // 执行全量导出
    void            ExportIncremental();                                         // 执行增量导出
    bool            HasPendingChanges() const;                                  // 检查是否有待导出的变更
    void            ClearPendingChanges();                                      // 清除待导出的变更记录

    // ---------------------------------------------------------
    // 待处理文件查询
    // ---------------------------------------------------------
    int32           GetPendingFilesCount() const;                                // 获取待处理文件数量
    int32           GetPendingBlueprintsCount() const;                           // 获取待处理蓝图数量
    int32           GetPendingNativeTypesCount() const;                          // 获取待处理原生类型数量
    int32           GetPendingCoreFilesCount() const;                            // 获取待处理核心文件数量（UE.lua, UE4.lua, UnLua.lua）

    // ---------------------------------------------------------
    // 扫描相关
    // ---------------------------------------------------------
    void            ScanExistingAssets();                                       // 扫描现有资源并添加到待导出列表
    void            ScanExistingAssetsAsync();                                 // 异步扫描现有资源并添加到待导出列表
    void            CancelAsyncScan();                                          // 取消异步扫描

    // ---------------------------------------------------------
    // 分帧处理相关
    // ---------------------------------------------------------
    void            StartFramedProcessing();                                    // 启动分帧处理
    bool            ProcessFramedStep();                                         // 分帧处理单步执行
    void            ProcessFramedStepWrapper();                                  // Timer包装函数
    void            CompleteFramedProcessing();                                 // 完成分帧处理

private:
    // ---------------------------------------------------------
    // 核心导出功能
    // ---------------------------------------------------------
    bool            AddToPendingBlueprints(const FAssetData& AssetData);        // 添加蓝图到待导出列表
    void            ExportBlueprint(const UBlueprint* Blueprint);             // 导出单个蓝图
    void            ExportNativeType(const UField* Field);                      // 导出单个原生类型
    void            ExportUETypes(const TArray<const UField*>& Types);          // 导出UE核心类型
    void            CollectNativeTypes(TArray<const UField*>& Types);            // 收集所有原生类型

    // ---------------------------------------------------------
    // 资源判断和验证
    // ---------------------------------------------------------
    static bool     IsBlueprint(const FAssetData& AssetData);                    // 判断是否为蓝图资源
    bool            ShouldExportBlueprint(const FAssetData& AssetData, bool bLoad = false) const; // 判断蓝图是否应该导出
    bool            IsValidFieldForExport(const UField* Field, FString& OutFieldName) const; // 验证UField是否有效且名称合法
    bool            ShouldExcludeFromExport(const FString& AssetPath) const;    // 检查路径是否应该被排除在导出之外

    // ---------------------------------------------------------
    // 文件操作
    // ---------------------------------------------------------
    void            SaveFile(const FString& ModuleName, const FString& FileName, const FString& Content); // 保存文件
    void            DeleteFile(const FString& ModuleName, const FString& FileName); // 删除文件
    FString         GetOutputDirectory() const;                                 // 获取输出目录
    void            CopyUELibFolder() const;                                    // 拷贝UELib文件夹到输出目录

    // ---------------------------------------------------------
    // 缓存管理
    // ---------------------------------------------------------
    void            LoadExportCache();                                          // 加载导出缓存
    void            SaveExportCache();                                          // 保存导出缓存
    bool            ShouldReexport(const FString& AssetPath, const FString& AssetFilePath) const; // 检查文件是否需要重新导出（基于哈希值）
    bool            ShouldReexportByHash(const FString& AssetPath, const FString& AssetHash) const; // 检查文件是否需要重新导出（基于哈希值）
    void            UpdateExportCacheByHash(const FString& AssetPath, const FString& AssetHash); // 更新导出缓存中的Hash值
    FString         GetCachedFieldHash(const UField* Field) const;               // 获取UField的缓存哈希值（带缓存优化）
    void            CleanupExpiredHashCache() const;                             // 清理过期的Hash缓存

    // ---------------------------------------------------------
    // 哈希计算
    // ---------------------------------------------------------
    FString         CalculateFileHash(const FString& FilePath) const;           // 计算文件的哈希值
    FString         CalculateClassStructureHash(const UClass* Class) const;     // 计算UE类结构签名哈希值
    FString         GetAssetHash(const FString& AssetPath) const;                // 获取资源的哈希值
    FString         GetAssetHash(const UField* Field) const;                    // 获取UField的哈希值（用于原生类型）

    // ---------------------------------------------------------
    // 辅助功能
    // ---------------------------------------------------------
    FString         GenerateUnLuaDefinitions() const;                           // 生成UnLua特定的定义
    void            LoadExcludedPathsFromFile(TArray<FString>& OutExcludedPaths) const; // 从JSON文件加载排除路径列表

    // ---------------------------------------------------------
    // 异步扫描辅助功能
    // ---------------------------------------------------------
    void            OnAsyncScanCompleted(const TArray<FAssetData>& BlueprintAssets, const TArray<const UField*>& NativeTypes); // 异步扫描完成回调
};
