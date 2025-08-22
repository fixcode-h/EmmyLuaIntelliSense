// Copyright Epic Games, Inc. All Rights Reserved.

#include "LuaExportManager.h"
#include "LuaCodeGenerator.h"
#include "LuaExportDialog.h"
#include "EmmyLuaIntelliSenseSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/UObjectIterator.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/ScopedSlowTask.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

ULuaExportManager::ULuaExportManager()
    : bInitialized(false)
    , DirectoryWatcher(nullptr)
    , LastNativeTypesCheckTime(0.0)
{
    // 设置导出缓存文件路径到插件的Intermediate目录
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("EmmyLuaIntelliSense"));
    if (Plugin.IsValid())
    {
        FString PluginDir = Plugin->GetBaseDir();
        ExportCacheFilePath = FPaths::Combine(PluginDir, TEXT("Intermediate"), TEXT("ExportCache.json"));
    }
    else
    {
        // 如果找不到插件，回退到项目的Intermediate目录
        ExportCacheFilePath = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("EmmyLuaIntelliSense"), TEXT("ExportCache.json"));
    }
}

ULuaExportManager* ULuaExportManager::Get()
{
    return GEditor ? GEditor->GetEditorSubsystem<ULuaExportManager>() : nullptr;
}

void ULuaExportManager::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    
    UE_LOG(LogTemp, Warning, TEXT("=== ULuaExportManager::Initialize() called ==="));
    
    if (bInitialized)
    {
        UE_LOG(LogTemp, Warning, TEXT("Already initialized, returning"));
        return;
    }

    // 设置输出目录
    OutputDir = GetOutputDirectory();

    // 注册资源注册表事件
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    AssetRegistryModule.Get().OnAssetAdded().AddUObject(this, &ULuaExportManager::OnAssetAdded);
    AssetRegistryModule.Get().OnAssetRemoved().AddUObject(this, &ULuaExportManager::OnAssetRemoved);
    AssetRegistryModule.Get().OnAssetRenamed().AddUObject(this, &ULuaExportManager::OnAssetRenamed);
    AssetRegistryModule.Get().OnAssetUpdated().AddUObject(this, &ULuaExportManager::OnAssetUpdated);

    // 初始化文件监听器
    InitializeFileWatcher();

    // 记录初始化时间
    LastNativeTypesCheckTime = FPlatformTime::Seconds();

    // 加载导出缓存
    LoadExportCache();

    // 扫描现有的蓝图和类型，添加到待导出列表
    ScanExistingAssets();

    bInitialized = true;

    UE_LOG(LogTemp, Warning, TEXT("=== LuaExportManager initialized successfully. Output directory: %s ==="), *OutputDir);
}

void ULuaExportManager::Deinitialize()
{
    Super::Deinitialize();
    
    if (!bInitialized)
    {
        return;
    }

    // 取消注册资源注册表事件
    if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
    {
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        AssetRegistryModule.Get().OnAssetAdded().RemoveAll(this);
        AssetRegistryModule.Get().OnAssetRemoved().RemoveAll(this);
        AssetRegistryModule.Get().OnAssetRenamed().RemoveAll(this);
        AssetRegistryModule.Get().OnAssetUpdated().RemoveAll(this);
    }

    // 停止文件监听器
    ShutdownFileWatcher();

    // 保存导出缓存
    SaveExportCache();

    bInitialized = false;
    PendingBlueprints.Empty();
    PendingNativeTypes.Empty();
    WatchedDirectories.Empty();
    ExportedFilesCache.Empty();

    UE_LOG(LogTemp, Log, TEXT("LuaExportManager shutdown."));
}

void ULuaExportManager::ExportAll()
{
    if (!bInitialized)
    {
        UE_LOG(LogTemp, Warning, TEXT("LuaExportManager not initialized."));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("Starting full Lua export..."));

    // 收集所有蓝图
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    
    FARFilter Filter;
    Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
    
    TArray<FAssetData> BlueprintAssets;
    AssetRegistryModule.Get().GetAssets(Filter, BlueprintAssets);

    // 收集所有原生类型
    TArray<const UField*> NativeTypes;
    CollectNativeTypes(NativeTypes);

    int32 TotalCount = BlueprintAssets.Num() + NativeTypes.Num() + 1; // +1 for UE types
    
    // 使用FScopedSlowTask显示进度条，参考UnLua实现
    FScopedSlowTask SlowTask(TotalCount, FText::FromString(TEXT("正在导出Lua IntelliSense文件...")));
    SlowTask.MakeDialog();

    try
    {
        // 导出蓝图
        for (const FAssetData& AssetData : BlueprintAssets)
        {
            if (SlowTask.ShouldCancel())
            {
                UE_LOG(LogTemp, Warning, TEXT("Lua export cancelled by user."));
                return;
            }
            
            SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("正在导出蓝图: %s"), *AssetData.AssetName.ToString())));
            
            if (ShouldExport(AssetData, true))
            {
                if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetData.ObjectPath.ToString()))
                {
                    ExportBlueprint(Blueprint);
                }
            }
        }

        // 导出原生类型
        for (const UField* Field : NativeTypes)
        {
            if (SlowTask.ShouldCancel())
            {
                UE_LOG(LogTemp, Warning, TEXT("Lua export cancelled by user."));
                return;
            }
            
            SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("正在导出原生类型: %s"), *Field->GetName())));
            ExportNativeType(Field);
        }

        // 导出UE核心类型
        SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("正在导出UE核心类型...")));
        ExportUETypes(NativeTypes);
        
        // 显示成功通知
        FLuaExportNotificationManager::ShowExportSuccess(TEXT("Lua IntelliSense文件导出完成！"));

        UE_LOG(LogTemp, Log, TEXT("Full Lua export completed. Exported %d items."), TotalCount);
    }
    catch (const std::exception& e)
    {
        FString ErrorMsg = FString::Printf(TEXT("导出失败: %s"), UTF8_TO_TCHAR(e.what()));
        FLuaExportNotificationManager::ShowExportFailure(ErrorMsg);
        UE_LOG(LogTemp, Error, TEXT("Full Lua export failed: %s"), UTF8_TO_TCHAR(e.what()));
    }
}

void ULuaExportManager::ExportIncremental()
{
    if (!bInitialized)
    {
        UE_LOG(LogTemp, Warning, TEXT("LuaExportManager not initialized."));
        return;
    }

    if (!HasPendingChanges())
    {
        UE_LOG(LogTemp, Log, TEXT("No pending changes for incremental export."));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("Starting incremental Lua export..."));

    int32 ExportedCount = 0;
    
    // 计算总任务数
    int32 TotalTasks = PendingBlueprints.Num() + PendingNativeTypes.Num();
    if (PendingNativeTypes.Num() > 0)
    {
        TotalTasks++; // +1 for UE types export
    }
    
    // 使用FScopedSlowTask显示进度条
    FScopedSlowTask SlowTask(TotalTasks, FText::FromString(TEXT("正在进行增量导出...")));
    SlowTask.MakeDialog();
    
    // 记录导出时间
    FDateTime ExportTime = FDateTime::Now();

    // 导出待处理的蓝图
    for (const FString& BlueprintPath : PendingBlueprints)
    {
        if (SlowTask.ShouldCancel())
        {
            UE_LOG(LogTemp, Warning, TEXT("Incremental export cancelled by user."));
            return;
        }
        
        FString BlueprintName = FPaths::GetBaseFilename(BlueprintPath);
        SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("正在导出蓝图: %s"), *BlueprintName)));
        
        if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath))
        {
            ExportBlueprint(Blueprint);
            // 更新导出缓存
            UpdateExportCache(BlueprintPath, ExportTime);
            ExportedCount++;
        }
    }

    // 导出待处理的原生类型
    for (const TWeakObjectPtr<const UField>& WeakField : PendingNativeTypes)
    {
        if (SlowTask.ShouldCancel())
        {
            UE_LOG(LogTemp, Warning, TEXT("Incremental export cancelled by user."));
            return;
        }
        
        if (const UField* Field = WeakField.Get())
        {
            FString FieldName;
            if (IsValidFieldForExport(Field, FieldName))
            {
                SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("正在导出原生类型: %s"), *Field->GetName())));
                
                ExportNativeType(Field);
                // 更新导出缓存
                FString FieldPathName = Field->GetPathName();
                UpdateExportCache(FieldPathName, ExportTime);
                ExportedCount++;
            }
        }
        else
        {
            // 如果WeakObjectPtr已失效，仍需要推进进度条
            SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("跳过已失效的原生类型")));
        }
    }

    // 如果有原生类型变化，重新导出UE核心类型
    if (PendingNativeTypes.Num() > 0)
    {
        if (SlowTask.ShouldCancel())
        {
            UE_LOG(LogTemp, Warning, TEXT("Incremental export cancelled by user."));
            return;
        }
        
        SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("正在导出UE核心类型...")));
        
        TArray<const UField*> AllNativeTypes;
        CollectNativeTypes(AllNativeTypes);
        ExportUETypes(AllNativeTypes);
        // 更新UE核心类型的导出缓存
        UpdateExportCache(TEXT("UE"), ExportTime);
        UpdateExportCache(TEXT("UE4"), ExportTime);
        UpdateExportCache(TEXT("UnLua"), ExportTime);
        ExportedCount++;
    }

    // 保存缓存到文件
    SaveExportCache();

    ClearPendingChanges();

    FString Message = FString::Printf(TEXT("增量导出完成，共导出 %d 项"), ExportedCount);
    FLuaExportNotificationManager::ShowExportSuccess(Message);

    UE_LOG(LogTemp, Log, TEXT("Incremental Lua export completed. Exported %d items."), ExportedCount);
}

bool ULuaExportManager::HasPendingChanges() const
{
    return PendingBlueprints.Num() > 0 || PendingNativeTypes.Num() > 0;
}

void ULuaExportManager::ClearPendingChanges()
{
    PendingBlueprints.Empty();
    PendingNativeTypes.Empty();
}

void ULuaExportManager::OnAssetAdded(const FAssetData& AssetData)
{
    if (ShouldExport(AssetData))
    {
        PendingBlueprints.Add(AssetData.ObjectPath.ToString());
    }
}

void ULuaExportManager::OnAssetRemoved(const FAssetData& AssetData)
{
    if (IsBlueprint(AssetData))
    {
        // 删除对应的Lua文件
        FString FileName = AssetData.AssetName.ToString();
        DeleteFile(TEXT("/Game"), FileName);
    }
}

void ULuaExportManager::OnAssetRenamed(const FAssetData& AssetData, const FString& OldPath)
{
    if (IsBlueprint(AssetData))
    {
        // 删除旧文件
        FString OldFileName = FPackageName::GetShortName(OldPath);
        DeleteFile(TEXT("/Game"), OldFileName);
        
        // 添加新文件到待导出列表
        if (ShouldExport(AssetData))
        {
            PendingBlueprints.Add(AssetData.ObjectPath.ToString());
        }
    }
}

void ULuaExportManager::OnAssetUpdated(const FAssetData& AssetData)
{
    if (ShouldExport(AssetData))
    {
        PendingBlueprints.Add(AssetData.ObjectPath.ToString());
    }
}

void ULuaExportManager::ExportBlueprint(const UBlueprint* Blueprint)
{
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return;
	}

	// 获取蓝图路径用于日志输出
	FString BlueprintPath = Blueprint->GetPathName();
	UE_LOG(LogTemp, Log, TEXT("[EXPORT] Exporting Blueprint: %s"), *BlueprintPath);

	FString LuaCode = FEmmyLuaCodeGenerator::GenerateBlueprint(Blueprint);
	if (!LuaCode.IsEmpty())
	{
		FString FileName = FEmmyLuaCodeGenerator::GetTypeName(Blueprint->GeneratedClass);
		if (FileName.EndsWith(TEXT("_C")))
		{
			FileName.LeftChopInline(2);
		}
		SaveFile(TEXT("/Game"), FileName, LuaCode);
		UE_LOG(LogTemp, Log, TEXT("[EXPORT] Blueprint exported successfully: %s -> %s.lua"), *BlueprintPath, *FileName);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[EXPORT] Failed to generate Lua code for Blueprint: %s"), *BlueprintPath);
	}
}

void ULuaExportManager::ExportNativeType(const UField* Field)
{
    FString FieldName;
    if (!IsValidFieldForExport(Field, FieldName))
    {
        return;
    }

    // 生成原生类型路径用于日志输出
    FString NativeTypePath = FString::Printf(TEXT("/Native/%s"), *FieldName);
    UE_LOG(LogTemp, Log, TEXT("[EXPORT] Exporting Native Type: %s (%s)"), *NativeTypePath, *Field->GetName());

    FString LuaCode;
    if (const UClass* Class = Cast<UClass>(Field))
    {
        if (IsValid(Class))
        {
            LuaCode = FEmmyLuaCodeGenerator::GenerateClass(Class);
        }
    }
    else if (const UScriptStruct* Struct = Cast<UScriptStruct>(Field))
    {
        if (IsValid(Struct))
        {
            LuaCode = FEmmyLuaCodeGenerator::GenerateStruct(Struct);
        }
    }
    else if (const UEnum* Enum = Cast<UEnum>(Field))
    {
        if (IsValid(Enum))
        {
            LuaCode = FEmmyLuaCodeGenerator::GenerateEnum(Enum);
        }
    }

    if (!LuaCode.IsEmpty())
    {
        const UPackage* Package = Field->GetPackage();
        FString ModuleName = Package ? Package->GetName() : TEXT("");
        FString FileName = FEmmyLuaCodeGenerator::GetTypeName(Field);
        
        if (!FileName.IsEmpty() && FileName != TEXT("Error") && FileName != TEXT("Invalid"))
        {
            SaveFile(ModuleName, FileName, LuaCode);
            UE_LOG(LogTemp, Log, TEXT("[EXPORT] Native Type exported successfully: %s -> %s/%s.lua"), *NativeTypePath, *ModuleName, *FileName);
            
            // 更新原生类型的导出缓存
            UpdateExportCache(NativeTypePath, FDateTime::Now());
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[EXPORT] Invalid filename for Native Type: %s (%s)"), *NativeTypePath, *Field->GetName());
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[EXPORT] Failed to generate Lua code for Native Type: %s (%s)"), *NativeTypePath, *Field->GetName());
    }
}

void ULuaExportManager::ExportUETypes(const TArray<const UField*>& Types)
{
    // 生成UE.lua文件
    FString UELuaCode = FEmmyLuaCodeGenerator::GenerateUETable(Types);
    if (!UELuaCode.IsEmpty())
    {
        SaveFile(TEXT(""), TEXT("UE"), UELuaCode);
    }
    
    // 生成UE4.lua文件（UE4 = UE的别名）
    FString UE4LuaCode = TEXT("\r\nUE4 = UE\r\n");
    SaveFile(TEXT(""), TEXT("UE4"), UE4LuaCode);
    
    // 生成UnLua.lua文件（UnLua特定的类和函数定义）
    FString UnLuaCode = GenerateUnLuaDefinitions();
    if (!UnLuaCode.IsEmpty())
    {
        SaveFile(TEXT(""), TEXT("UnLua"), UnLuaCode);
    }
}

void ULuaExportManager::CollectNativeTypes(TArray<const UField*>& Types)
{
    Types.Empty();

    // 收集所有类
    for (TObjectIterator<UClass> It; It; ++It)
    {
        const UClass* Class = *It;
        FString ClassName;
        if (!IsValidFieldForExport(Class, ClassName))
        {
            continue;
        }
        
        if (FEmmyLuaCodeGenerator::ShouldSkipType(Class))
        {
            continue;
        }
        Types.Add(Class);
    }

    // 收集所有结构体
    for (TObjectIterator<UScriptStruct> It; It; ++It)
    {
        const UScriptStruct* Struct = *It;
        FString StructName;
        if (!IsValidFieldForExport(Struct, StructName))
        {
            continue;
        }
        
        if (FEmmyLuaCodeGenerator::ShouldSkipType(Struct))
        {
            continue;
        }
        Types.Add(Struct);
    }

    // 收集所有枚举
    for (TObjectIterator<UEnum> It; It; ++It)
    {
        const UEnum* Enum = *It;
        FString EnumName;
        if (!IsValidFieldForExport(Enum, EnumName))
        {
            continue;
        }
        
        if (FEmmyLuaCodeGenerator::ShouldSkipType(Enum))
        {
            continue;
        }
        Types.Add(Enum);
    }
    
    UE_LOG(LogTemp, Log, TEXT("CollectNativeTypes: Collected %d valid types"), Types.Num());
}

bool ULuaExportManager::IsBlueprint(const FAssetData& AssetData) const
{
    return AssetData.AssetClass == UBlueprint::StaticClass()->GetFName();
}

bool ULuaExportManager::ShouldExport(const FAssetData& AssetData, bool bLoad) const
{
    if (!IsBlueprint(AssetData))
    {
        return false;
    }
    
    // 检查插件设置中的蓝图导出开关
    const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
    if (Settings && !Settings->bExportBlueprintFiles)
    {
        // 如果禁用了蓝图导出，跳过所有蓝图
        FString AssetPath = AssetData.ObjectPath.ToString();
        UE_LOG(LogTemp, Log, TEXT("[EXPORT] Blueprint export disabled, skipping: %s"), *AssetPath);
        return false;
    }

    if (bLoad)
    {
        if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetData.ObjectPath.ToString()))
        {
            return Blueprint->GeneratedClass != nullptr;
        }
        return false;
    }

    return true;
}

void ULuaExportManager::SaveFile(const FString& ModuleName, const FString& FileName, const FString& Content)
{
    FString Directory = OutputDir;
    if (!ModuleName.IsEmpty())
    {
        Directory = FPaths::Combine(Directory, ModuleName);
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*Directory))
    {
        PlatformFile.CreateDirectoryTree(*Directory);
    }

    FString FilePath = FPaths::Combine(Directory, FileName + TEXT(".lua"));
    
    // 检查文件内容是否有变化
    FString ExistingContent;
    if (FFileHelper::LoadFileToString(ExistingContent, *FilePath))
    {
        if (ExistingContent == Content)
        {
            return; // 内容没有变化，不需要重写
        }
    }

    if (!FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to save Lua file: %s"), *FilePath);
    }
    else
    {
        UE_LOG(LogTemp, Verbose, TEXT("Saved Lua file: %s"), *FilePath);
    }
}

void ULuaExportManager::DeleteFile(const FString& ModuleName, const FString& FileName)
{
    FString Directory = OutputDir;
    if (!ModuleName.IsEmpty())
    {
        Directory = FPaths::Combine(Directory, ModuleName);
    }

    FString FilePath = FPaths::Combine(Directory, FileName + TEXT(".lua"));
    
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (PlatformFile.FileExists(*FilePath))
    {
        if (PlatformFile.DeleteFile(*FilePath))
        {
            UE_LOG(LogTemp, Log, TEXT("Deleted Lua file: %s"), *FilePath);
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to delete Lua file: %s"), *FilePath);
        }
    }
}

FString ULuaExportManager::GetOutputDirectory() const
{
    // 使用EmmyLuaIntelliSense插件的Intermediate目录
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("EmmyLuaIntelliSense"));
    if (Plugin.IsValid())
    {
        FString PluginDir = Plugin->GetBaseDir();
        return FPaths::Combine(PluginDir, TEXT("Intermediate"), TEXT("LuaIntelliSense"));
    }
    
    // 如果找不到插件，回退到项目的Intermediate目录
    return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("LuaIntelliSense"));
}

void ULuaExportManager::InitializeFileWatcher()
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
    {
        return;
    }

    FDirectoryWatcherModule& DirectoryWatcherModule = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
    DirectoryWatcher = DirectoryWatcherModule.Get();

    if (DirectoryWatcher)
    {
        // 监听引擎源码目录
        FString EngineSourceDir = FPaths::Combine(FPaths::EngineDir(), TEXT("Source"));
        AddWatchDirectory(EngineSourceDir);

        // 监听项目源码目录
        FString ProjectSourceDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"));
        if (FPaths::DirectoryExists(ProjectSourceDir))
        {
            AddWatchDirectory(ProjectSourceDir);
        }

        // 监听插件目录
        FString PluginsDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins"));
        if (FPaths::DirectoryExists(PluginsDir))
        {
            AddWatchDirectory(PluginsDir);
        }

        UE_LOG(LogTemp, Log, TEXT("File watcher initialized with %d directories"), WatchedDirectories.Num());
    }
}

void ULuaExportManager::ShutdownFileWatcher()
{
    if (DirectoryWatcher)
    {
        for (const auto& Pair : WatchedDirectories)
        {
            DirectoryWatcher->UnregisterDirectoryChangedCallback_Handle(Pair.Key, Pair.Value);
        }
        DirectoryWatcher = nullptr;
    }
}

void ULuaExportManager::AddWatchDirectory(const FString& Directory)
{
    if (DirectoryWatcher && FPaths::DirectoryExists(Directory))
    {
        FDelegateHandle Handle;
        bool bSuccess = DirectoryWatcher->RegisterDirectoryChangedCallback_Handle(
            Directory,
            IDirectoryWatcher::FDirectoryChanged::CreateUObject(this, &ULuaExportManager::OnDirectoryChanged),
            Handle,
            true // bWatchSubtree
        );

        if (bSuccess)
        {
            WatchedDirectories.Add(Directory, Handle);
            UE_LOG(LogTemp, Log, TEXT("Added directory watcher: %s"), *Directory);
        }
    }
}

void ULuaExportManager::OnDirectoryChanged(const TArray<FFileChangeData>& FileChanges)
{
    bool bHasRelevantChanges = false;

    for (const FFileChangeData& Change : FileChanges)
    {
        FString FileName = FPaths::GetCleanFilename(Change.Filename);
        FString Extension = FPaths::GetExtension(FileName);

        // 只关心头文件和源文件的变化
        if (Extension == TEXT("h") || Extension == TEXT("cpp") || Extension == TEXT("hpp"))
        {
            // 跳过临时文件和备份文件
            if (FileName.Contains(TEXT("~")) || FileName.Contains(TEXT(".tmp")))
            {
                continue;
            }

            UE_LOG(LogTemp, Log, TEXT("Detected file change: %s (Action: %d)"), *Change.Filename, (int32)Change.Action);
            bHasRelevantChanges = true;
        }
    }

    if (bHasRelevantChanges)
    {
        // 延迟检查原生类型变化，避免频繁触发
        if (GEngine && GEngine->GetWorld())
        {
            FTimerHandle TimerHandle;
            FTimerManager& TimerManager = GEngine->GetWorld()->GetTimerManager();
            TimerManager.SetTimer(TimerHandle, [this](){ CheckNativeTypesChanges(); }, 2.0f, false);
        }
        else
        {
            // 如果没有世界对象，直接调用检查函数
            CheckNativeTypesChanges();
        }
    }
}

void ULuaExportManager::CheckNativeTypesChanges()
{
    double CurrentTime = FPlatformTime::Seconds();
    
    // 避免频繁检查
    if (CurrentTime - LastNativeTypesCheckTime < 5.0)
    {
        return;
    }

    LastNativeTypesCheckTime = CurrentTime;

    // 收集当前的原生类型
    TArray<const UField*> CurrentNativeTypes;
    CollectNativeTypes(CurrentNativeTypes);

    // 简单的变化检测：如果数量发生变化，则认为有变化
    static int32 LastNativeTypesCount = 0;
    if (CurrentNativeTypes.Num() != LastNativeTypesCount)
    {
        LastNativeTypesCount = CurrentNativeTypes.Num();
        
        // 将有效的原生类型标记为待导出
         int32 ValidTypesCount = 0;
         for (const UField* Field : CurrentNativeTypes)
         {
             FString FieldName;
             if (IsValidFieldForExport(Field, FieldName))
             {
                 PendingNativeTypes.Add(TWeakObjectPtr<const UField>(Field));
                 ValidTypesCount++;
             }
         }

        UE_LOG(LogTemp, Log, TEXT("Native types changed detected. %d valid types marked for export (out of %d total)."), ValidTypesCount, CurrentNativeTypes.Num());
    }
}

void ULuaExportManager::ScanExistingAssets()
{
    // 扫描现有的蓝图资源
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> BlueprintAssets;
    
    // 获取所有蓝图资源
    FARFilter Filter;
    Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
    AssetRegistryModule.Get().GetAssets(Filter, BlueprintAssets);
    
    // 添加蓝图到待导出列表（只添加需要重新导出的）
    UE_LOG(LogTemp, Log, TEXT("[SCAN] Found %d blueprint assets, checking for export..."), BlueprintAssets.Num());
    for (const FAssetData& AssetData : BlueprintAssets)
    {
        if (ShouldExport(AssetData))
        {
            FString AssetPath = AssetData.ObjectPath.ToString();
            
            // 检查是否应该排除此路径
            if (ShouldExcludeFromExport(AssetPath))
            {
                UE_LOG(LogTemp, Verbose, TEXT("[SCAN] Excluded Blueprint: %s"), *AssetPath);
                continue;
            }
            
            // 直接获取完整的文件路径，使用重载版本的ShouldReexport避免重复转换
              FString AssetFilePath;
              if (FPackageName::TryConvertLongPackageNameToFilename(AssetPath, AssetFilePath, TEXT(".uasset")))
              {
                  UE_LOG(LogTemp, VeryVerbose, TEXT("[SCAN] Direct file access: %s -> %s"), *AssetPath, *AssetFilePath);
                  
                  // 使用重载版本的ShouldReexport，直接传入文件路径
                  if (ShouldReexport(AssetPath, AssetFilePath))
                  {
                      PendingBlueprints.Add(AssetPath);
                      UE_LOG(LogTemp, Log, TEXT("[SCAN] Added Blueprint to pending list: %s"), *AssetPath);
                  }
                  else
                  {
                      UE_LOG(LogTemp, Verbose, TEXT("[SCAN] Blueprint up-to-date, skipping: %s"), *AssetPath);
                  }
              }
              else
              {
                  UE_LOG(LogTemp, Warning, TEXT("[SCAN] Failed to convert package name to file path: %s"), *AssetPath);
              }
        }
    }
    
    // 扫描原生类型
    TArray<const UField*> NativeTypes;
    CollectNativeTypes(NativeTypes);
    
    // 添加有效的原生类型到待导出列表（只添加需要重新导出的）
    UE_LOG(LogTemp, Log, TEXT("[SCAN] Found %d native types, checking for export..."), NativeTypes.Num());
    int32 ValidNativeTypesCount = 0;
    int32 PendingNativeTypesCount = 0;
    for (const UField* Field : NativeTypes)
    {
        FString FieldName;
        if (IsValidFieldForExport(Field, FieldName))
        {
            ValidNativeTypesCount++;
            
            // 为原生类型生成唯一的路径标识
            FString NativeTypePath = FString::Printf(TEXT("/Native/%s"), *FieldName);
            
            // 检查是否应该排除此路径
            if (ShouldExcludeFromExport(NativeTypePath))
            {
                UE_LOG(LogTemp, Verbose, TEXT("[SCAN] Excluded Native Type: %s"), *NativeTypePath);
                continue;
            }
            
            // 检查是否需要重新导出（原生类型使用编译时间作为修改时间）
            FDateTime CompileTime = FDateTime::FromUnixTimestamp(FPlatformFileManager::Get().GetPlatformFile().GetStatData(*FModuleManager::Get().GetModuleFilename("EmmyLuaIntelliSense")).ModificationTime.ToUnixTimestamp());
            
            if (ShouldReexport(NativeTypePath, CompileTime))
            {
                PendingNativeTypes.Add(TWeakObjectPtr<const UField>(Field));
                PendingNativeTypesCount++;
                UE_LOG(LogTemp, Log, TEXT("[SCAN] Added Native Type to pending list: %s"), *NativeTypePath);
            }
            else
            {
                UE_LOG(LogTemp, Verbose, TEXT("[SCAN] Native Type up-to-date, skipping: %s"), *NativeTypePath);
            }
        }
    }
    
    UE_LOG(LogTemp, Log, TEXT("Scanned existing assets: %d blueprints, %d pending native types (out of %d valid, %d total)"), PendingBlueprints.Num(), PendingNativeTypesCount, ValidNativeTypesCount, NativeTypes.Num());
}

FString ULuaExportManager::GenerateUnLuaDefinitions() const
{
    // 直接从EmmyLuaIntelliSense插件的Resources/UnLua.lua文件读取内容
    FString UnLuaFilePath = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("EmmyLuaIntelliSense/Resources/UnLua.lua"));
    FString Content;
    
    if (FFileHelper::LoadFileToString(Content, *UnLuaFilePath))
    {
        return Content;
    }
    // 如果文件不存在，使用默认内容
    FString UnLuaCode = TEXT("---@class UnLua\n");

    return UnLuaCode;
}

void ULuaExportManager::LoadExportCache()
{
    // 开始计时
    double StartTime = FPlatformTime::Seconds();
    
    ExportedFilesCache.Empty();
    
    UE_LOG(LogTemp, Log, TEXT("Loading export cache from: %s"), *ExportCacheFilePath);
    
    if (!FPaths::FileExists(ExportCacheFilePath))
    {
        UE_LOG(LogTemp, Log, TEXT("Export cache file not found, starting fresh export. Path: %s"), *ExportCacheFilePath);
        return;
    }
    
    // 记录文件大小
    int64 FileSize = IFileManager::Get().FileSize(*ExportCacheFilePath);
    UE_LOG(LogTemp, Log, TEXT("Export cache file size: %lld bytes"), FileSize);
    
    FString JsonString;
    double LoadStartTime = FPlatformTime::Seconds();
    if (!FFileHelper::LoadFileToString(JsonString, *ExportCacheFilePath))
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to load export cache file: %s"), *ExportCacheFilePath);
        return;
    }
    double LoadEndTime = FPlatformTime::Seconds();
    UE_LOG(LogTemp, Log, TEXT("File loading took: %.3f ms"), (LoadEndTime - LoadStartTime) * 1000.0);
    
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    
    double ParseStartTime = FPlatformTime::Seconds();
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to parse export cache JSON"));
        return;
    }
    double ParseEndTime = FPlatformTime::Seconds();
    UE_LOG(LogTemp, Log, TEXT("JSON parsing took: %.3f ms"), (ParseEndTime - ParseStartTime) * 1000.0);
    
    double ProcessStartTime = FPlatformTime::Seconds();
    int32 FilteredCount = 0;
    for (const auto& Pair : JsonObject->Values)
    {
        // 检查是否应该排除此路径
        if (ShouldExcludeFromExport(Pair.Key))
        {
            FilteredCount++;
            continue;
        }
        
        FString TimeString = Pair.Value->AsString();
        FDateTime ExportTime;
        if (FDateTime::Parse(TimeString, ExportTime))
        {
            ExportedFilesCache.Add(Pair.Key, ExportTime);
        }
    }
    double ProcessEndTime = FPlatformTime::Seconds();
    UE_LOG(LogTemp, Log, TEXT("Cache processing took: %.3f ms, filtered %d excluded paths"), (ProcessEndTime - ProcessStartTime) * 1000.0, FilteredCount);
    
    // 总耗时
    double TotalTime = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogTemp, Log, TEXT("LoadExportCache completed: %d entries loaded in %.3f ms"), ExportedFilesCache.Num(), TotalTime * 1000.0);
}

void ULuaExportManager::SaveExportCache()
{
    // 开始计时
    double StartTime = FPlatformTime::Seconds();
    
    UE_LOG(LogTemp, Log, TEXT("Saving export cache with %d entries to: %s"), ExportedFilesCache.Num(), *ExportCacheFilePath);
    
    // 确保目录存在
    FString CacheDir = FPaths::GetPath(ExportCacheFilePath);
    if (!FPaths::DirectoryExists(CacheDir))
    {
        UE_LOG(LogTemp, Log, TEXT("Creating cache directory: %s"), *CacheDir);
        IFileManager::Get().MakeDirectory(*CacheDir, true);
    }
    
    double SerializeStartTime = FPlatformTime::Seconds();
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
    
    for (const auto& Pair : ExportedFilesCache)
    {
        JsonObject->SetStringField(Pair.Key, Pair.Value.ToString());
    }
    
    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    
    if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer))
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to serialize export cache JSON"));
        return;
    }
    double SerializeEndTime = FPlatformTime::Seconds();
    UE_LOG(LogTemp, Log, TEXT("JSON serialization took: %.3f ms, size: %d characters"), (SerializeEndTime - SerializeStartTime) * 1000.0, JsonString.Len());
    
    double SaveStartTime = FPlatformTime::Seconds();
    if (FFileHelper::SaveStringToFile(JsonString, *ExportCacheFilePath))
    {
        double SaveEndTime = FPlatformTime::Seconds();
        UE_LOG(LogTemp, Log, TEXT("File saving took: %.3f ms"), (SaveEndTime - SaveStartTime) * 1000.0);
        
        // 验证文件大小
        int64 FileSize = IFileManager::Get().FileSize(*ExportCacheFilePath);
        UE_LOG(LogTemp, Log, TEXT("Saved file size: %lld bytes"), FileSize);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to save export cache to: %s"), *ExportCacheFilePath);
        return;
    }
    
    // 总耗时
    double TotalTime = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogTemp, Log, TEXT("SaveExportCache completed in %.3f ms"), TotalTime * 1000.0);
}

bool ULuaExportManager::ShouldReexport(const FString& AssetPath, const FDateTime& AssetModifyTime) const
{
    // 获取插件设置
    const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
    
    if (Settings && Settings->FileComparisonMethod == EFileComparisonMethod::Hash)
    {
        // 使用哈希比较
        FString AssetHash = GetAssetHash(AssetPath);
        if (AssetHash.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[REEXPORT] Failed to get hash for %s, will export"), *AssetPath);
            return true;
        }
        
        return ShouldReexportByHash(AssetPath, AssetHash);
    }
    else
    {
        // 使用时间戳比较（默认方式）
        const FDateTime* CachedTime = ExportedFilesCache.Find(AssetPath);
        
        if (!CachedTime)
        {
            // 没有缓存记录，需要导出
            UE_LOG(LogTemp, Log, TEXT("[REEXPORT] No cache found for %s, will export"), *AssetPath);
            return true;
        }
        
        // 如果资源修改时间晚于上次导出时间，需要重新导出
        bool bShouldReexport = AssetModifyTime > *CachedTime;
        UE_LOG(LogTemp, Log, TEXT("[REEXPORT] %s: Asset time=%s, Cache time=%s, Should reexport=%s"), 
            *AssetPath, 
            *AssetModifyTime.ToString(), 
            *CachedTime->ToString(), 
            bShouldReexport ? TEXT("YES") : TEXT("NO"));
        
        return bShouldReexport;
    }
}

bool ULuaExportManager::ShouldReexport(const FString& AssetPath, const FString& AssetFilePath) const
{
    // 获取插件设置
    const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
    
    if (Settings && Settings->FileComparisonMethod == EFileComparisonMethod::Hash)
    {
        // 使用哈希比较 - 直接使用文件路径计算哈希
        FString AssetHash = CalculateFileHash(AssetFilePath);
        if (AssetHash.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[REEXPORT] Failed to get hash for file %s (asset: %s), will export"), *AssetFilePath, *AssetPath);
            return true;
        }
        
        return ShouldReexportByHash(AssetPath, AssetHash);
    }
    else
    {
        // 使用时间戳比较 - 直接使用文件路径获取修改时间
        FDateTime AssetModifyTime = IFileManager::Get().GetTimeStamp(*AssetFilePath);
        if (AssetModifyTime == FDateTime::MinValue())
        {
            UE_LOG(LogTemp, Warning, TEXT("[REEXPORT] Failed to get modify time for file %s (asset: %s), will export"), *AssetFilePath, *AssetPath);
            return true;
        }
        
        const FDateTime* CachedTime = ExportedFilesCache.Find(AssetPath);
        
        if (!CachedTime)
        {
            // 没有缓存记录，需要导出
            UE_LOG(LogTemp, Log, TEXT("[REEXPORT] No cache found for %s, will export"), *AssetPath);
            return true;
        }
        
        // 比较修改时间
        bool bShouldReexport = AssetModifyTime > *CachedTime;
        UE_LOG(LogTemp, Log, TEXT("[REEXPORT] %s: Asset time=%s, Cache time=%s, Should reexport=%s"), 
            *AssetPath, 
            *AssetModifyTime.ToString(), 
            *CachedTime->ToString(), 
            bShouldReexport ? TEXT("YES") : TEXT("NO"));
        
        return bShouldReexport;
    }
}

void ULuaExportManager::UpdateExportCache(const FString& AssetPath, const FDateTime& ExportTime)
{
    // 检查是否应该排除此路径
    if (ShouldExcludeFromExport(AssetPath))
    {
        return;
    }
    
    // 获取插件设置
    const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
    
    if (Settings && Settings->FileComparisonMethod == EFileComparisonMethod::Hash)
    {
        // 使用哈希比较时，更新哈希缓存
        FString AssetHash = GetAssetHash(AssetPath);
        if (!AssetHash.IsEmpty())
        {
            UpdateExportCacheByHash(AssetPath, AssetHash);
        }
    }
    else
    {
        // 使用时间戳比较时，更新时间戳缓存（默认方式）
        ExportedFilesCache.Add(AssetPath, ExportTime);
    }
}

bool ULuaExportManager::IsValidFieldForExport(const UField* Field, FString& OutFieldName) const
{
    if (!Field)
    {
        UE_LOG(LogTemp, Warning, TEXT("IsValidFieldForExport: Field is null"));
        return false;
    }

    // 检查Field是否有效
    if (!IsValid(Field))
    {
        UE_LOG(LogTemp, Warning, TEXT("IsValidFieldForExport: Field is not valid: %s"), Field ? *Field->GetName() : TEXT("null"));
        return false;
    }
    
    // 检查Field名称是否有效
    try
    {
        OutFieldName = Field->GetName();
    }
    catch (...)
    {
        UE_LOG(LogTemp, Warning, TEXT("IsValidFieldForExport: Exception getting field name"));
        return false;
    }
    
    if (OutFieldName.IsEmpty() || OutFieldName == TEXT("None") || OutFieldName == TEXT("NULL"))
    {
        UE_LOG(LogTemp, Warning, TEXT("IsValidFieldForExport: Skipping field with invalid name: %s"), *OutFieldName);
        return false;
    }

    // 检查名称是否以点或空格开头/结尾
    if (OutFieldName.StartsWith(TEXT(".")) || OutFieldName.EndsWith(TEXT(".")) ||
        OutFieldName.StartsWith(TEXT(" ")) || OutFieldName.EndsWith(TEXT(" ")))
    {
        UE_LOG(LogTemp, Warning, TEXT("IsValidFieldForExport: Skipping field with invalid name format: %s"), *OutFieldName);
        return false;
    }
    
    // 检查名称长度是否合理（避免过长的非法名称）
    if (OutFieldName.Len() > 256)
    {
        UE_LOG(LogTemp, Warning, TEXT("IsValidFieldForExport: Skipping field with excessively long name (length: %d)"), OutFieldName.Len());
        return false;
    }

    return true;
}

FDateTime ULuaExportManager::GetAssetModifyTime(const FString& AssetPath) const
{
    // 对于蓝图资源，获取.uasset文件的修改时间
    // 处理 /Game/ 路径下的资源
    if (AssetPath.StartsWith(TEXT("/Game/")))
    {
        FString PackageName = AssetPath.RightChop(6); // 移除 "/Game/"
        FString AssetFilePath = FPaths::Combine(FPaths::ProjectContentDir(), PackageName + TEXT(".uasset"));
        
        UE_LOG(LogTemp, Log, TEXT("[TIMESTAMP] Checking Game Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
        
        if (FPaths::FileExists(AssetFilePath))
        {
            FDateTime ModifyTime = IFileManager::Get().GetTimeStamp(*AssetFilePath);
            UE_LOG(LogTemp, Log, TEXT("[TIMESTAMP] Blueprint modify time: %s"), *ModifyTime.ToString());
            return ModifyTime;
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[TIMESTAMP] Game Blueprint file not found: %s"), *AssetFilePath);
        }
    }
    // 处理插件路径下的资源（如 /PluginName/...）
    else if (AssetPath.StartsWith(TEXT("/")) && AssetPath.Contains(TEXT("/")))
    {
        // 尝试通过包管理器找到实际的文件路径
        FString PackageName = AssetPath;
        FString AssetFilePath;
        
        // 尝试通过 FPackageName 转换为文件路径
        if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, AssetFilePath, TEXT(".uasset")))
        {
            UE_LOG(LogTemp, Log, TEXT("[TIMESTAMP] Checking Plugin Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
            
            if (FPaths::FileExists(AssetFilePath))
            {
                FDateTime ModifyTime = IFileManager::Get().GetTimeStamp(*AssetFilePath);
                UE_LOG(LogTemp, Log, TEXT("[TIMESTAMP] Plugin Blueprint modify time: %s"), *ModifyTime.ToString());
                return ModifyTime;
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("[TIMESTAMP] Plugin Blueprint file not found: %s"), *AssetFilePath);
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[TIMESTAMP] Failed to convert package name to file path: %s"), *AssetPath);
        }
    }
    // 对于原生类型，使用插件模块的编译时间
    else if (AssetPath.StartsWith(TEXT("/Native/")))
    {
        FString ModuleFilename = FModuleManager::Get().GetModuleFilename("EmmyLuaIntelliSense");
        if (FPaths::FileExists(ModuleFilename))
        {
            return FDateTime::FromUnixTimestamp(FPlatformFileManager::Get().GetPlatformFile().GetStatData(*ModuleFilename).ModificationTime.ToUnixTimestamp());
        }
    }
    
    // 对于其他情况，返回当前时间
    return FDateTime::Now();
}

void ULuaExportManager::LoadExcludedPathsFromFile(TArray<FString>& OutExcludedPaths) const
{
    // 从插件的Resources目录读取排除路径配置文件
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("EmmyLuaIntelliSense"));
    FString ConfigFilePath;
    
    if (Plugin.IsValid())
    {
        FString PluginDir = Plugin->GetBaseDir();
        ConfigFilePath = FPaths::Combine(PluginDir, TEXT("Resources"), TEXT("ExcludedPaths.json"));
    }
    else
    {
        // 如果找不到插件，回退到项目的Config目录
        ConfigFilePath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("EmmyLuaIntelliSense"), TEXT("ExcludedPaths.json"));
    }
    
    UE_LOG(LogTemp, Log, TEXT("[CONFIG] Loading excluded paths from: %s"), *ConfigFilePath);
    UE_LOG(LogTemp, Log, TEXT("[CONFIG] File exists check: %s"), FPaths::FileExists(ConfigFilePath) ? TEXT("YES") : TEXT("NO"));
    
    if (!FPaths::FileExists(ConfigFilePath))
    {
        UE_LOG(LogTemp, Warning, TEXT("[CONFIG] Excluded paths config file not found, using default exclusions: %s"), *ConfigFilePath);
        LoadDefaultExcludedPaths(OutExcludedPaths);
        return;
    }
    
    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *ConfigFilePath))
    {
        UE_LOG(LogTemp, Warning, TEXT("[CONFIG] Failed to load excluded paths config file: %s"), *ConfigFilePath);
        LoadDefaultExcludedPaths(OutExcludedPaths);
        return;
    }
    
    UE_LOG(LogTemp, Log, TEXT("[CONFIG] Successfully loaded JSON file, size: %d characters"), JsonString.Len());
    
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("[CONFIG] Failed to parse excluded paths JSON config"));
        LoadDefaultExcludedPaths(OutExcludedPaths);
        return;
    }
    
    UE_LOG(LogTemp, Log, TEXT("[CONFIG] Successfully parsed JSON object"));
    
    const TArray<TSharedPtr<FJsonValue>>* PathsArray;
    if (JsonObject->TryGetArrayField(TEXT("excludedPaths"), PathsArray))
    {
        OutExcludedPaths.Empty();
        for (const TSharedPtr<FJsonValue>& PathValue : *PathsArray)
        {
            FString Path = PathValue->AsString();
            if (!Path.IsEmpty())
            {
                OutExcludedPaths.Add(Path);
            }
        }
        
        UE_LOG(LogTemp, Log, TEXT("[CONFIG] Loaded %d excluded paths from config file"), OutExcludedPaths.Num());
        
        // 打印前几个路径用于调试
        for (int32 i = 0; i < FMath::Min(3, OutExcludedPaths.Num()); i++)
        {
            UE_LOG(LogTemp, Log, TEXT("[CONFIG] Sample path [%d]: %s"), i, *OutExcludedPaths[i]);
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[CONFIG] No 'excludedPaths' array found in config file"));
        LoadDefaultExcludedPaths(OutExcludedPaths);
    }
}

void ULuaExportManager::LoadDefaultExcludedPaths(TArray<FString>& OutExcludedPaths) const
{
    // 默认的排除路径列表 - 只排除在业务代码中绝对不会用到的路径
    OutExcludedPaths = {
        // 测试相关
        TEXT("/RuntimeTests"),
        TEXT("/UnLuaTestSuite"),
        TEXT("/Script/RuntimeTests"),
        
        // 编辑器专用工具和界面
        TEXT("/Script/EditorStyle"),
        TEXT("/Script/EditorWidgets"),
        TEXT("/Script/UnrealEd"),
        TEXT("/Script/ToolMenus"),
        TEXT("/Script/PropertyEditor"),
        TEXT("/Script/ContentBrowser"),
        TEXT("/Script/AssetTools"),
        TEXT("/Script/LevelEditor"),
        TEXT("/Script/Sequencer"),
        TEXT("/Script/MovieSceneTools"),
        TEXT("/Script/MovieSceneCapture"),
        TEXT("/Script/BlueprintGraph"),
        TEXT("/Script/KismetCompiler"),
        TEXT("/Script/MaterialEditor"),
        TEXT("/Script/MaterialBaking"),
        TEXT("/Script/AdvancedPreviewScene"),
        TEXT("/Script/StatsViewer"),
        TEXT("/Script/CurveEditor"),
        TEXT("/Script/ContentBrowserData"),
        TEXT("/Script/ClassViewer"),
        
        // 特定插件（通常不在业务代码中使用）
        TEXT("/MagicLeapPassableWorld"),
        TEXT("/Script/MagicLeapPassableWorld"),
        TEXT("/DatasmithContent"),
        TEXT("/Script/DatasmithContent"),
        
        // 交互工具框架（编辑器专用）
        TEXT("/Script/InteractiveToolsFramework"),
        TEXT("/Script/EditorInteractiveToolsFramework"),
        
        // VR编辑器（编辑器专用）
        TEXT("/Script/VREditor"),
        TEXT("/Script/ViewportInteraction"),
        
        // 服装系统编辑器
        TEXT("/Script/ClothingSystemEditor"),
        
        // 序列录制工具
        TEXT("/Script/SequenceRecorder"),
        
        // 本地化工具
        TEXT("/Script/Localization"),
        
        // 硬件目标和项目生成（编辑器专用）
        TEXT("/Script/HardwareTargeting"),
        TEXT("/Script/GameProjectGeneration"),
        
        // 源码控制（编辑器专用）
        TEXT("/Script/SourceControl"),
        
        // PIE预览设备（编辑器专用）
        TEXT("/Script/PIEPreviewDeviceSpecification"),
        TEXT("/Script/PIEPreviewDeviceProfileSelector"),
        
        // 一些不常用的引擎系统
        TEXT("/Script/PacketHandler"),
        TEXT("/Script/MeshDescription"),
        TEXT("/Script/StaticMeshDescription"),
        TEXT("/Script/PropertyAccess"),
        TEXT("/Script/MaterialShaderQualitySettings"),
        
        // 临时文件和编辑器原生类型
        TEXT("/Temp/"),
        TEXT("/Native/EditorStyle"),
        TEXT("/Native/ToolMenus"),
        TEXT("/Native/UnrealEd"),
    };
    
    UE_LOG(LogTemp, Log, TEXT("[CONFIG] Using default excluded paths: %d entries"), OutExcludedPaths.Num());
    
    // 打印前几个默认路径用于调试
    for (int32 i = 0; i < FMath::Min(3, OutExcludedPaths.Num()); i++)
    {
        UE_LOG(LogTemp, Log, TEXT("[CONFIG] Default path [%d]: %s"), i, *OutExcludedPaths[i]);
    }
}

FString ULuaExportManager::CalculateFileHash(const FString& FilePath) const
{
    if (!FPaths::FileExists(FilePath))
    {
        return FString();
    }
    
    TArray<uint8> FileData;
    if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
    {
        UE_LOG(LogTemp, Warning, TEXT("[HASH] Failed to load file for hashing: %s"), *FilePath);
        return FString();
    }
    
    // 使用SHA1计算哈希值
    FSHA1 Sha1;
    Sha1.Update(FileData.GetData(), FileData.Num());
    Sha1.Final();
    
    // 转换为十六进制字符串
    FString HashString;
    for (int32 i = 0; i < 20; ++i)
    {
        HashString += FString::Printf(TEXT("%02x"), Sha1.m_digest[i]);
    }
    
    return HashString;
}

FString ULuaExportManager::GetAssetHash(const FString& AssetPath) const
{
    // 对于蓝图资源，计算.uasset文件的哈希值
    // 处理 /Game/ 路径下的资源
    if (AssetPath.StartsWith(TEXT("/Game/")))
    {
        FString PackageName = AssetPath.RightChop(6); // 移除 "/Game/"
        FString AssetFilePath = FPaths::Combine(FPaths::ProjectContentDir(), PackageName + TEXT(".uasset"));
        
        UE_LOG(LogTemp, Log, TEXT("[HASH] Calculating hash for Game Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
        
        FString Hash = CalculateFileHash(AssetFilePath);
        if (!Hash.IsEmpty())
        {
            UE_LOG(LogTemp, Log, TEXT("[HASH] Game Blueprint hash: %s"), *Hash);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[HASH] Failed to calculate hash for Game Blueprint: %s"), *AssetFilePath);
        }
        
        return Hash;
    }
    // 处理插件路径下的资源（如 /PluginName/...）
    else if (AssetPath.StartsWith(TEXT("/")) && AssetPath.Contains(TEXT("/")))
    {
        // 尝试通过包管理器找到实际的文件路径
        FString PackageName = AssetPath;
        FString AssetFilePath;
        
        // 尝试通过 FPackageName 转换为文件路径
        if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, AssetFilePath, TEXT(".uasset")))
        {
            UE_LOG(LogTemp, Log, TEXT("[HASH] Calculating hash for Plugin Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
            
            FString Hash = CalculateFileHash(AssetFilePath);
            if (!Hash.IsEmpty())
            {
                UE_LOG(LogTemp, Log, TEXT("[HASH] Plugin Blueprint hash: %s"), *Hash);
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("[HASH] Failed to calculate hash for Plugin Blueprint: %s"), *AssetFilePath);
            }
            
            return Hash;
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[HASH] Failed to convert package name to file path: %s"), *AssetPath);
        }
    }
    
    // 对于原生类型，使用类型名称和编译时间生成哈希
    FString TypeInfo = AssetPath + TEXT("_") + FDateTime::Now().ToString();
    return FMD5::HashAnsiString(*TypeInfo);
}

bool ULuaExportManager::ShouldReexportByHash(const FString& AssetPath, const FString& AssetHash) const
{
    const FString* CachedHash = ExportedFilesHashCache.Find(AssetPath);
    
    if (!CachedHash)
    {
        // 没有缓存记录，需要导出
        UE_LOG(LogTemp, Log, TEXT("[REEXPORT_HASH] No hash cache found for %s, will export"), *AssetPath);
        return true;
    }
    
    // 如果哈希值不同，需要重新导出
    bool bShouldReexport = AssetHash != *CachedHash;
    UE_LOG(LogTemp, Log, TEXT("[REEXPORT_HASH] %s: Asset hash=%s, Cache hash=%s, Should reexport=%s"), 
        *AssetPath, 
        *AssetHash, 
        **CachedHash, 
        bShouldReexport ? TEXT("YES") : TEXT("NO"));
    
    return bShouldReexport;
}

void ULuaExportManager::UpdateExportCacheByHash(const FString& AssetPath, const FString& AssetHash)
{
    ExportedFilesHashCache.Add(AssetPath, AssetHash);
    UE_LOG(LogTemp, Log, TEXT("[CACHE_HASH] Updated hash cache for %s: %s"), *AssetPath, *AssetHash);
}

bool ULuaExportManager::ShouldExcludeFromExport(const FString& AssetPath) const
{
    // 从JSON文件加载排除路径列表
    static TArray<FString> ExcludedPaths;
    static bool bPathsLoaded = false;
    
    if (!bPathsLoaded)
    {
        LoadExcludedPathsFromFile(ExcludedPaths);
        bPathsLoaded = true;
        UE_LOG(LogTemp, Log, TEXT("[EXCLUDE] Loaded %d excluded paths for filtering"), ExcludedPaths.Num());
        
        // 打印前几个排除路径用于调试
        for (int32 i = 0; i < FMath::Min(5, ExcludedPaths.Num()); i++)
        {
            UE_LOG(LogTemp, Log, TEXT("[EXCLUDE] Sample excluded path [%d]: %s"), i, *ExcludedPaths[i]);
        }
    }
    
    // 检查是否匹配任何排除路径
    for (const FString& ExcludedPath : ExcludedPaths)
    {
        if (AssetPath.StartsWith(ExcludedPath))
        {
            UE_LOG(LogTemp, Verbose, TEXT("[EXCLUDE] Path excluded: %s (matched: %s)"), *AssetPath, *ExcludedPath);
            return true;
        }
    }
    
    UE_LOG(LogTemp, VeryVerbose, TEXT("[EXCLUDE] Path allowed: %s"), *AssetPath);
    return false;
}