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
#include "EmmyLuaIntelliSense.h"
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
    
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("EmmyLuaIntelliSense"));
    if (Plugin.IsValid())
    {
        FString PluginDir = Plugin->GetBaseDir();
        ExportCacheFilePath = FPaths::Combine(PluginDir, TEXT("Intermediate"), TEXT("ExportCache.json"));
    }
    else
    {
        
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
    
    UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("=== ULuaExportManager::Initialize() called ==="));
    
    if (bInitialized)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Already initialized, returning"));
        return;
    }

    
    OutputDir = GetOutputDirectory();

    
    InitializeFileWatcher();

    
    LastNativeTypesCheckTime = FPlatformTime::Seconds();

    
    LoadExportCache();

    bInitialized = true;

    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("=== LuaExportManager initialized successfully. Output directory: %s ==="), *OutputDir);
}

void ULuaExportManager::Deinitialize()
{
    Super::Deinitialize();
    
    if (!bInitialized)
    {
        return;
    }
    
    ShutdownFileWatcher();

    
    SaveExportCache();

    bInitialized = false;
    PendingBlueprints.Empty();
    PendingNativeTypes.Empty();
    WatchedDirectories.Empty();
    ExportedFilesCache.Empty();

    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("LuaExportManager shutdown."));
}

void ULuaExportManager::ExportAll()
{
    if (!bInitialized)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("LuaExportManager not initialized."));
        return;
    }

    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Starting full Lua export..."));

    
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    
    FARFilter Filter;
    Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
    
    TArray<FAssetData> BlueprintAssets;
    AssetRegistryModule.Get().GetAssets(Filter, BlueprintAssets);

    
    TArray<const UField*> NativeTypes;
    CollectNativeTypes(NativeTypes);

    int32 TotalCount = BlueprintAssets.Num() + NativeTypes.Num() + 1; 
    
    
    FScopedSlowTask SlowTask(TotalCount, FText::FromString(TEXT("正在导出Lua IntelliSense文件...")));
    SlowTask.MakeDialog();

    try
    {
        
        for (const FAssetData& AssetData : BlueprintAssets)
        {
            if (SlowTask.ShouldCancel())
            {
                UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Lua export cancelled by user."));
                return;
            }
            
            SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("正在导出蓝图: %s"), *AssetData.AssetName.ToString())));
            
            if (ShouldExportBlueprint(AssetData, false))
            {
                if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetData.ObjectPath.ToString()))
                {
                    ExportBlueprint(Blueprint);
                }
            }
        }

        
        for (const UField* Field : NativeTypes)
        {
            if (SlowTask.ShouldCancel())
            {
                UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Lua export cancelled by user."));
                return;
            }
            
            SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("正在导出原生类型: %s"), *Field->GetName())));
            ExportNativeType(Field);
        }

        
        SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("正在导出UE核心类型...")));
        ExportUETypes(NativeTypes);
        
        
        FLuaExportNotificationManager::ShowExportSuccess(TEXT("Lua IntelliSense文件导出完成！"));

        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Full Lua export completed. Exported %d items."), TotalCount);
    }
    catch (const std::exception& e)
    {
        FString ErrorMsg = FString::Printf(TEXT("导出失败: %s"), UTF8_TO_TCHAR(e.what()));
        FLuaExportNotificationManager::ShowExportFailure(ErrorMsg);
        UE_LOG(LogEmmyLuaIntelliSense, Error, TEXT("Full Lua export failed: %s"), UTF8_TO_TCHAR(e.what()));
    }
}

void ULuaExportManager::ExportIncremental()
{
    if (!bInitialized)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("LuaExportManager not initialized."));
        return;
    }

    if (!HasPendingChanges())
    {
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("No pending changes for incremental export."));
        return;
    }

    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Starting incremental Lua export..."));

    int32 ExportedCount = 0;
    
    
    int32 TotalTasks = PendingBlueprints.Num() + PendingNativeTypes.Num();
    if (PendingNativeTypes.Num() > 0)
    {
        TotalTasks++; 
    }
    
    
    FScopedSlowTask SlowTask(TotalTasks, FText::FromString(TEXT("正在进行增量导出...")));
    SlowTask.MakeDialog();
    
    
    for (const FString& BlueprintPath : PendingBlueprints)
    {
        if (SlowTask.ShouldCancel())
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Incremental export cancelled by user."));
            return;
        }
        
        FString BlueprintName = FPaths::GetBaseFilename(BlueprintPath);
        SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("正在导出蓝图: %s"), *BlueprintName)));
        
        if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath))
        {
            ExportBlueprint(Blueprint);
            
            FDateTime ExportTime = FDateTime::Now();
            UpdateExportCache(BlueprintPath, ExportTime);
            ExportedCount++;
        }
    }

    
    for (const TWeakObjectPtr<const UField>& WeakField : PendingNativeTypes)
    {
        if (SlowTask.ShouldCancel())
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Incremental export cancelled by user."));
            return;
        }
        
        if (const UField* Field = WeakField.Get())
        {
            FString FieldName;
            if (IsValidFieldForExport(Field, FieldName))
            {
                SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("正在导出原生类型: %s"), *Field->GetName())));
                
                ExportNativeType(Field);
                
                FDateTime ExportTime = FDateTime::Now();
                FString FieldPathName = Field->GetPathName();
                UpdateExportCache(FieldPathName, ExportTime);
                ExportedCount++;
            }
        }
        else
        {
            
            SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("跳过已失效的原生类型")));
        }
    }

    
    if (PendingNativeTypes.Num() > 0)
    {
        if (SlowTask.ShouldCancel())
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Incremental export cancelled by user."));
            return;
        }
        
        SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("正在导出UE核心类型...")));
        
        TArray<const UField*> AllNativeTypes;
        CollectNativeTypes(AllNativeTypes);
        ExportUETypes(AllNativeTypes);
        
        FDateTime ExportTime = FDateTime::Now();
        UpdateExportCache(TEXT("UE"), ExportTime);
        UpdateExportCache(TEXT("UE4"), ExportTime);
        UpdateExportCache(TEXT("UnLua"), ExportTime);
        ExportedCount++;
    }

    
    SaveExportCache();

    ClearPendingChanges();

    FString Message = FString::Printf(TEXT("增量导出完成，共导出 %d 项"), ExportedCount);
    FLuaExportNotificationManager::ShowExportSuccess(Message);

    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Incremental Lua export completed. Exported %d items."), ExportedCount);
}

bool ULuaExportManager::HasPendingChanges() const
{
    return PendingBlueprints.Num() > 0 || PendingNativeTypes.Num() > 0;
}

int32 ULuaExportManager::GetPendingFilesCount() const
{
    return GetPendingBlueprintsCount() + GetPendingNativeTypesCount() + GetPendingCoreFilesCount();
}

int32 ULuaExportManager::GetPendingBlueprintsCount() const
{
    return PendingBlueprints.Num();
}

int32 ULuaExportManager::GetPendingNativeTypesCount() const
{
    return PendingNativeTypes.Num();
}

int32 ULuaExportManager::GetPendingCoreFilesCount() const
{
    
    if (PendingNativeTypes.Num() > 0)
    {
        return 3; 
    }
    return 0;
}

void ULuaExportManager::ClearPendingChanges()
{
    PendingBlueprints.Empty();
    PendingNativeTypes.Empty();
}

bool ULuaExportManager::AddToPendingBlueprints(const FAssetData& AssetData)
{
    if (!ShouldExportBlueprint(AssetData))
    {
        return false;
    }
    
    FString AssetPath = AssetData.ObjectPath.ToString();
    
    if (PendingBlueprints.Contains(AssetPath))
    {
        UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[PENDING] Blueprint already in pending list, skipping: %s"), *AssetPath);
        return false;
    }
    
    
    FString PackageName = AssetPath;
    int32 LastDotIndex;
    if (PackageName.FindLastChar('.', LastDotIndex))
    {
        FString ClassName = PackageName.Mid(LastDotIndex + 1);
        FString PathWithoutClass = PackageName.Left(LastDotIndex);
        
        
        if (PathWithoutClass.EndsWith("/" + ClassName))
        {
            PackageName = PathWithoutClass;
            UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[PATH] Normalized blueprint path: %s -> %s"), *AssetPath, *PackageName);
        }
    }
    
    FString AssetFilePath;
    if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, AssetFilePath, TEXT(".uasset")))
    {
        UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[PATH] Failed to convert package name to file path: %s"), *PackageName);
        return false;
    }
   
    
    if (!FPaths::FileExists(AssetFilePath))
    {
        return false;
    }
    
    if (!ShouldReexport(AssetPath, AssetFilePath))
    {
        return false;
    }
    PendingBlueprints.Add(AssetPath);
    UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("[PENDING] Added Blueprint to pending list: %s (Total: %d)"), *AssetPath, PendingBlueprints.Num());

    return true;
}

void ULuaExportManager::ExportBlueprint(const UBlueprint* Blueprint)
{
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return;
	}

	
	FString BlueprintPath = Blueprint->GetPathName();

	FString LuaCode = FEmmyLuaCodeGenerator::GenerateBlueprint(Blueprint);
	if (!LuaCode.IsEmpty())
	{
		FString FileName = FEmmyLuaCodeGenerator::GetTypeName(Blueprint->GeneratedClass);
		if (FileName.EndsWith(TEXT("_C")))
		{
			FileName.LeftChopInline(2);
		}
		SaveFile(TEXT("/Game"), FileName, LuaCode);
		UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[EXPORT] Blueprint exported successfully: %s -> %s.lua"), *BlueprintPath, *FileName);
	}
	else
	{
		UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[EXPORT] Failed to generate Lua code for Blueprint: %s"), *BlueprintPath);
	}
}

void ULuaExportManager::ExportNativeType(const UField* Field)
{
    FString FieldName;
    if (!IsValidFieldForExport(Field, FieldName))
    {
        return;
    }

    
    FString NativeTypePath = FString::Printf(TEXT("/Native/%s"), *FieldName);
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[EXPORT] Exporting Native Type: %s (%s)"), *NativeTypePath, *Field->GetName());

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
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[EXPORT] Native Type exported successfully: %s -> %s/%s.lua"), *NativeTypePath, *ModuleName, *FileName);
            
            
            UpdateExportCache(NativeTypePath, FDateTime::Now());
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[EXPORT] Invalid filename for Native Type: %s (%s)"), *NativeTypePath, *Field->GetName());
        }
    }
    else
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[EXPORT] Failed to generate Lua code for Native Type: %s (%s)"), *NativeTypePath, *Field->GetName());
    }
}

void ULuaExportManager::ExportUETypes(const TArray<const UField*>& Types)
{
    
    FString UELuaCode = FEmmyLuaCodeGenerator::GenerateUETable(Types);
    if (!UELuaCode.IsEmpty())
    {
        SaveFile(TEXT(""), TEXT("UE"), UELuaCode);
    }
    
    
    FString UE4LuaCode = TEXT("---@type UE\r\nUE4 = UE\r\n");
    SaveFile(TEXT(""), TEXT("UE4"), UE4LuaCode);
    
    
    FString UnLuaCode = GenerateUnLuaDefinitions();
    if (!UnLuaCode.IsEmpty())
    {
        SaveFile(TEXT(""), TEXT("UnLua"), UnLuaCode);
    }
    
    
    CopyUELibFolder();
}

void ULuaExportManager::CollectNativeTypes(TArray<const UField*>& Types)
{
    Types.Empty();

    
    for (TObjectIterator<UClass> It; It; ++It)
    {
        const UClass* Class = *It;
        FString ClassName;
        if (!IsValidFieldForExport(Class, ClassName))
        {
            continue;
        }
        
        
        if (!Class->HasAnyClassFlags(CLASS_Native))
        {
            continue;
        }
        
        
        if (ClassName.StartsWith(TEXT("SKEL_")) || 
            ClassName.StartsWith(TEXT("REINST_")) ||
            ClassName.StartsWith(TEXT("TRASHCLASS_")) ||
            ClassName.StartsWith(TEXT("HOTRELOADED_")) ||
            ClassName.StartsWith(TEXT("PLACEHOLDER_")))
        {
            continue;
        }
        
        if (FEmmyLuaCodeGenerator::ShouldSkipType(Class))
        {
            continue;
        }
        Types.Add(Class);
    }

    
    for (TObjectIterator<UScriptStruct> It; It; ++It)
    {
        const UScriptStruct* Struct = *It;
        FString StructName;
        if (!IsValidFieldForExport(Struct, StructName))
        {
            continue;
        }
        
        
        if (!Struct->IsNative())
        {
            continue;
        }
        
        
        if (StructName.StartsWith(TEXT("SKEL_")) || 
            StructName.StartsWith(TEXT("REINST_")) ||
            StructName.StartsWith(TEXT("TRASHCLASS_")) ||
            StructName.StartsWith(TEXT("HOTRELOADED_")) ||
            StructName.StartsWith(TEXT("PLACEHOLDER_")))
        {
            continue;
        }
        
        if (FEmmyLuaCodeGenerator::ShouldSkipType(Struct))
        {
            continue;
        }
        Types.Add(Struct);
    }

    
    for (TObjectIterator<UEnum> It; It; ++It)
    {
        const UEnum* Enum = *It;
        FString EnumName;
        if (!IsValidFieldForExport(Enum, EnumName))
        {
            continue;
        }
        
        
        if (!Enum->IsNative())
        {
            continue;
        }
        
        
        if (EnumName.StartsWith(TEXT("SKEL_")) || 
            EnumName.StartsWith(TEXT("REINST_")) ||
            EnumName.StartsWith(TEXT("TRASHCLASS_")) ||
            EnumName.StartsWith(TEXT("HOTRELOADED_")) ||
            EnumName.StartsWith(TEXT("PLACEHOLDER_")))
        {
            continue;
        }
        
        if (FEmmyLuaCodeGenerator::ShouldSkipType(Enum))
        {
            continue;
        }
        Types.Add(Enum);
    }
    
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("CollectNativeTypes: Collected %d valid types"), Types.Num());
}

bool ULuaExportManager::IsBlueprint(const FAssetData& AssetData)
{
    return AssetData.AssetClass == UBlueprint::StaticClass()->GetFName();
}

bool ULuaExportManager::ShouldExportBlueprint(const FAssetData& AssetData, bool bLoad) const
{
    if (!IsBlueprint(AssetData))
    {
        return false;
    }
    
    
    FString AssetPath = AssetData.ObjectPath.ToString();
    if (ShouldExcludeFromExport(AssetPath))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("[EXPORT] Blueprint excluded from export: %s"), *AssetPath);
        return false;
    }
    
    
    const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
    if (Settings && !Settings->bExportBlueprintFiles)
    {
        
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[EXPORT] Blueprint export disabled, skipping: %s"), *AssetPath);
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
    
    
    FString ExistingContent;
    if (FFileHelper::LoadFileToString(ExistingContent, *FilePath))
    {
        if (ExistingContent == Content)
        {
            return; 
        }
    }

    if (!FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Error, TEXT("Failed to save Lua file: %s"), *FilePath);
    }
    else
    {
        UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("Saved Lua file: %s"), *FilePath);
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
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Deleted Lua file: %s"), *FilePath);
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Error, TEXT("Failed to delete Lua file: %s"), *FilePath);
        }
    }
}

FString ULuaExportManager::GetOutputDirectory() const
{
    
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("EmmyLuaIntelliSense"));
    if (Plugin.IsValid())
    {
        FString PluginDir = Plugin->GetBaseDir();
        return FPaths::Combine(PluginDir, TEXT("Intermediate"), TEXT("LuaIntelliSense"));
    }
    
    
    return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("LuaIntelliSense"));
}

void ULuaExportManager::InitializeFileWatcher()
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
    {
        return;
    }

    FDirectoryWatcherModule& DirectoryWatcherModule = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
    IDirectoryWatcher* RawDirectoryWatcher = DirectoryWatcherModule.Get();
    if (RawDirectoryWatcher)
    {
        DirectoryWatcher = MakeShareable(RawDirectoryWatcher);
    }

    if (DirectoryWatcher.IsValid())
    {
        
        FString EngineSourceDir = FPaths::Combine(FPaths::EngineDir(), TEXT("Source"));
        AddWatchDirectory(EngineSourceDir);

        
        FString ProjectSourceDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"));
        if (FPaths::DirectoryExists(ProjectSourceDir))
        {
            AddWatchDirectory(ProjectSourceDir);
        }

        
        FString PluginsDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins"));
        if (FPaths::DirectoryExists(PluginsDir))
        {
            AddWatchDirectory(PluginsDir);
        }

        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("File watcher initialized with %d directories"), WatchedDirectories.Num());
    }
}

void ULuaExportManager::ShutdownFileWatcher()
{
    if (DirectoryWatcher.IsValid())
    {
        for (const auto& Pair : WatchedDirectories)
        {
            DirectoryWatcher->UnregisterDirectoryChangedCallback_Handle(Pair.Key, Pair.Value);
        }
        DirectoryWatcher.Reset();
    }
}

void ULuaExportManager::AddWatchDirectory(const FString& Directory)
{
    if (DirectoryWatcher.IsValid() && FPaths::DirectoryExists(Directory))
    {
        FDelegateHandle Handle;
        bool bSuccess = DirectoryWatcher->RegisterDirectoryChangedCallback_Handle(
            Directory,
            IDirectoryWatcher::FDirectoryChanged::CreateUObject(this, &ULuaExportManager::OnDirectoryChanged),
            Handle,
            true 
        );

        if (bSuccess)
        {
            WatchedDirectories.Add(Directory, Handle);
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Added directory watcher: %s"), *Directory);
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

        
        if (Extension == TEXT("h") || Extension == TEXT("cpp") || Extension == TEXT("hpp"))
        {
            
            if (FileName.Contains(TEXT("~")) || FileName.Contains(TEXT(".tmp")))
            {
                continue;
            }

            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Detected file change: %s (Action: %d)"), *Change.Filename, (int32)Change.Action);
            bHasRelevantChanges = true;
        }
    }

    if (bHasRelevantChanges)
    {
        
        if (GEngine && GEngine->GetWorld())
        {
            FTimerHandle TimerHandle;
            FTimerManager& TimerManager = GEngine->GetWorld()->GetTimerManager();
            TimerManager.SetTimer(TimerHandle, [this](){ CheckNativeTypesChanges(); }, 2.0f, false);
        }
        else
        {
            
            CheckNativeTypesChanges();
        }
    }
}

void ULuaExportManager::CheckNativeTypesChanges()
{
    double CurrentTime = FPlatformTime::Seconds();
    
    
    if (CurrentTime - LastNativeTypesCheckTime < 5.0)
    {
        return;
    }

    LastNativeTypesCheckTime = CurrentTime;

    
    TArray<const UField*> CurrentNativeTypes;
    CollectNativeTypes(CurrentNativeTypes);

    
    static int32 LastNativeTypesCount = -1; 
    static bool bFirstCheck = true;
    
    if (bFirstCheck)
    {
        
        
        LastNativeTypesCount = CurrentNativeTypes.Num();
        bFirstCheck = false;
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("First native types check: %d types found, skipping auto-export"), CurrentNativeTypes.Num());
        return;
    }
    
    if (CurrentNativeTypes.Num() != LastNativeTypesCount)
    {
        int32 OldCount = LastNativeTypesCount;
        LastNativeTypesCount = CurrentNativeTypes.Num();
        
        
        
        if (CurrentNativeTypes.Num() > OldCount)
        {
            
            int32 ValidTypesCount = 0;
            for (const UField* Field : CurrentNativeTypes)
            {
                FString FieldName;
                if (IsValidFieldForExport(Field, FieldName))
                {
                    
                    TWeakObjectPtr<const UField> WeakField(Field);
                    if (!PendingNativeTypes.Contains(WeakField))
                    {
                        PendingNativeTypes.Add(WeakField);
                        ValidTypesCount++;
                    }
                }
            }
            
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Native types increased from %d to %d. %d new valid types marked for export."), OldCount, CurrentNativeTypes.Num(), ValidTypesCount);
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Native types count changed from %d to %d (decreased or no change), no action needed."), OldCount, CurrentNativeTypes.Num());
        }
    }
}

void ULuaExportManager::ScanExistingAssets()
{
    
    const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
    if (Settings && Settings->bExportBlueprintFiles)
    {
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> BlueprintAssets;
        
        
        FARFilter Filter;
        Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
        AssetRegistryModule.Get().GetAssets(Filter, BlueprintAssets);
        
        
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[SCAN] Found %d blueprint assets, checking for export..."), BlueprintAssets.Num());
        for (const FAssetData& AssetData : BlueprintAssets)
        {
            AddToPendingBlueprints(AssetData);
        }
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[SCAN] Wait To Export Count %d "), PendingBlueprints.Num());
    }
    
    
    TArray<const UField*> NativeTypes;
    CollectNativeTypes(NativeTypes);
    
    
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[SCAN] Found %d native types, checking for export..."), NativeTypes.Num());
    int32 ValidNativeTypesCount = 0;
    int32 PendingNativeTypesCount = 0;
    for (const UField* Field : NativeTypes)
    {
        FString FieldName;
        if (IsValidFieldForExport(Field, FieldName))
        {
            ValidNativeTypesCount++;
            
            
            FString NativeTypePath = FString::Printf(TEXT("/Native/%s"), *FieldName);
            
            
            if (ShouldExcludeFromExport(NativeTypePath))
            {
                UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("[SCAN] Excluded Native Type: %s"), *NativeTypePath);
                continue;
            }
            
            
            FDateTime CompileTime = FDateTime::FromUnixTimestamp(FPlatformFileManager::Get().GetPlatformFile().GetStatData(*FModuleManager::Get().GetModuleFilename("EmmyLuaIntelliSense")).ModificationTime.ToUnixTimestamp());
            
            if (ShouldReexport(NativeTypePath, CompileTime))
            {
                PendingNativeTypes.Add(TWeakObjectPtr<const UField>(Field));
                PendingNativeTypesCount++;
                UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[SCAN] Added Native Type to pending list: %s"), *NativeTypePath);
            }
            else
            {
                UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("[SCAN] Native Type up-to-date, skipping: %s"), *NativeTypePath);
            }
        }
    }
    
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Scanned existing assets: %d blueprints, %d pending native types (out of %d valid, %d total)"), PendingBlueprints.Num(), PendingNativeTypesCount, ValidNativeTypesCount, NativeTypes.Num());
}

FString ULuaExportManager::GenerateUnLuaDefinitions() const
{
    
    FString UnLuaFilePath = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("EmmyLuaIntelliSense/Resources/UnLua.lua"));
    FString Content;
    
    if (FFileHelper::LoadFileToString(Content, *UnLuaFilePath))
    {
        return Content;
    }
    
    FString UnLuaCode = TEXT("---@class UnLua\n");

    return UnLuaCode;
}

void ULuaExportManager::LoadExportCache()
{
    
    double StartTime = FPlatformTime::Seconds();
    
    ExportedFilesCache.Empty();
    
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Loading export cache from: %s"), *ExportCacheFilePath);
    
    if (!FPaths::FileExists(ExportCacheFilePath))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Export cache file not found, starting fresh export. Path: %s"), *ExportCacheFilePath);
        return;
    }
    
    
    int64 FileSize = IFileManager::Get().FileSize(*ExportCacheFilePath);
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Export cache file size: %lld bytes"), FileSize);
    
    FString JsonString;
    double LoadStartTime = FPlatformTime::Seconds();
    if (!FFileHelper::LoadFileToString(JsonString, *ExportCacheFilePath))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Failed to load export cache file: %s"), *ExportCacheFilePath);
        return;
    }
    double LoadEndTime = FPlatformTime::Seconds();
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("File loading took: %.3f ms"), (LoadEndTime - LoadStartTime) * 1000.0);
    
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    
    double ParseStartTime = FPlatformTime::Seconds();
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Failed to parse export cache JSON"));
        return;
    }
    double ParseEndTime = FPlatformTime::Seconds();
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("JSON parsing took: %.3f ms"), (ParseEndTime - ParseStartTime) * 1000.0);
    
    double ProcessStartTime = FPlatformTime::Seconds();
    int32 FilteredCount = 0;
    for (const auto& Pair : JsonObject->Values)
    {
        
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
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Cache processing took: %.3f ms, filtered %d excluded paths"), (ProcessEndTime - ProcessStartTime) * 1000.0, FilteredCount);
    
    
    double TotalTime = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("LoadExportCache completed: %d entries loaded in %.3f ms"), ExportedFilesCache.Num(), TotalTime * 1000.0);
}

void ULuaExportManager::SaveExportCache()
{
    
    double StartTime = FPlatformTime::Seconds();
    
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Saving export cache with %d entries to: %s"), ExportedFilesCache.Num(), *ExportCacheFilePath);
    
    
    FString CacheDir = FPaths::GetPath(ExportCacheFilePath);
    if (!FPaths::DirectoryExists(CacheDir))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Creating cache directory: %s"), *CacheDir);
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
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Failed to serialize export cache JSON"));
        return;
    }
    double SerializeEndTime = FPlatformTime::Seconds();
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("JSON serialization took: %.3f ms, size: %d characters"), (SerializeEndTime - SerializeStartTime) * 1000.0, JsonString.Len());
    
    double SaveStartTime = FPlatformTime::Seconds();
    if (FFileHelper::SaveStringToFile(JsonString, *ExportCacheFilePath))
    {
        double SaveEndTime = FPlatformTime::Seconds();
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("File saving took: %.3f ms"), (SaveEndTime - SaveStartTime) * 1000.0);
        
        
        int64 FileSize = IFileManager::Get().FileSize(*ExportCacheFilePath);
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Saved file size: %lld bytes"), FileSize);
    }
    else
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Failed to save export cache to: %s"), *ExportCacheFilePath);
        return;
    }
    
    
    double TotalTime = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("SaveExportCache completed in %.3f ms"), TotalTime * 1000.0);
}

bool ULuaExportManager::ShouldReexport(const FString& AssetPath, const FDateTime& AssetModifyTime) const
{
    
    const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
    
    if (Settings && Settings->FileComparisonMethod == EFileComparisonMethod::Hash)
    {
        
        FString AssetHash = GetAssetHash(AssetPath);
        if (AssetHash.IsEmpty())
        {
            return true;
        }
        
        return ShouldReexportByHash(AssetPath, AssetHash);
    }
    else
    {
        
        const FDateTime* CachedTime = ExportedFilesCache.Find(AssetPath);
        
        if (!CachedTime)
        {
            return true;
        }
        
        
        bool bShouldReexport = AssetModifyTime > *CachedTime;
        if (bShouldReexport)
        {
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[REEXPORT] %s: Asset time=%s, Cache time=%s, Should reexport=%s"), 
              *AssetPath, 
              *AssetModifyTime.ToString(), 
              *CachedTime->ToString(), 
              bShouldReexport ? TEXT("YES") : TEXT("NO"));  
        }
        return bShouldReexport;
    }
}

bool ULuaExportManager::ShouldReexport(const FString& AssetPath, const FString& AssetFilePath) const
{
    
    const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
    
    if (Settings && Settings->FileComparisonMethod == EFileComparisonMethod::Hash)
    {
        
        FString AssetHash = CalculateFileHash(AssetFilePath);
        if (AssetHash.IsEmpty())
        {
            UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[REEXPORT] Failed to get hash for file %s (asset: %s), will export"), *AssetFilePath, *AssetPath);
            return true;
        }
        
        return ShouldReexportByHash(AssetPath, AssetHash);
    }
    else
    {
        
        FDateTime AssetModifyTime = IFileManager::Get().GetTimeStamp(*AssetFilePath);
        if (AssetModifyTime == FDateTime::MinValue())
        {
            return true;
        }
        
        const FDateTime* CachedTime = ExportedFilesCache.Find(AssetPath);
        
        if (!CachedTime)
        {
            return true;
        }
        
        
        bool bShouldReexport = AssetModifyTime > *CachedTime;
        if (bShouldReexport)
        {
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[REEXPORT] %s: Asset time=%s, Cache time=%s, Should reexport=%s"), 
                *AssetPath, 
                *AssetModifyTime.ToString(), 
                *CachedTime->ToString(), 
                bShouldReexport ? TEXT("YES") : TEXT("NO"));
        }
        return bShouldReexport;
    }
}

void ULuaExportManager::UpdateExportCache(const FString& AssetPath, const FDateTime& ExportTime)
{
    
    if (ShouldExcludeFromExport(AssetPath))
    {
        return;
    }
    
    
    const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
    
    if (Settings && Settings->FileComparisonMethod == EFileComparisonMethod::Hash)
    {
        
        FString AssetHash = GetAssetHash(AssetPath);
        if (!AssetHash.IsEmpty())
        {
            UpdateExportCacheByHash(AssetPath, AssetHash);
        }
    }
    else
    {
        
        ExportedFilesCache.Add(AssetPath, ExportTime);
    }
}

bool ULuaExportManager::IsValidFieldForExport(const UField* Field, FString& OutFieldName) const
{
    if (!Field)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("IsValidFieldForExport: Field is null"));
        return false;
    }

    
    if (!IsValid(Field))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("IsValidFieldForExport: Field is not valid: %s"), Field ? *Field->GetName() : TEXT("null"));
        return false;
    }
    
    
    try
    {
        OutFieldName = Field->GetName();
    }
    catch (...)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("IsValidFieldForExport: Exception getting field name"));
        return false;
    }
    
    if (OutFieldName.IsEmpty() || OutFieldName == TEXT("None") || OutFieldName == TEXT("NULL"))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("IsValidFieldForExport: Skipping field with invalid name: %s"), *OutFieldName);
        return false;
    }

    
    if (OutFieldName.StartsWith(TEXT(".")) || OutFieldName.EndsWith(TEXT(".")) ||
        OutFieldName.StartsWith(TEXT(" ")) || OutFieldName.EndsWith(TEXT(" ")))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("IsValidFieldForExport: Skipping field with invalid name format: %s"), *OutFieldName);
        return false;
    }
    
    
    if (OutFieldName.Len() > 256)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("IsValidFieldForExport: Skipping field with excessively long name (length: %d)"), OutFieldName.Len());
        return false;
    }

    return true;
}

FDateTime ULuaExportManager::GetAssetModifyTime(const FString& AssetPath) const
{
    
    
    FString NormalizedAssetPath = AssetPath;
    int32 LastDotIndex;
    if (NormalizedAssetPath.FindLastChar('.', LastDotIndex))
    {
        FString ClassName = NormalizedAssetPath.Mid(LastDotIndex + 1);
        FString PathWithoutClass = NormalizedAssetPath.Left(LastDotIndex);
        
        
        if (PathWithoutClass.EndsWith("/" + ClassName))
        {
            NormalizedAssetPath = PathWithoutClass;
            UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[TIMESTAMP] Normalized blueprint path: %s -> %s"), *AssetPath, *NormalizedAssetPath);
        }
    }
    
    
    if (NormalizedAssetPath.StartsWith(TEXT("/Game/")))
    {
        FString PackageName = NormalizedAssetPath.RightChop(6); 
        FString AssetFilePath = FPaths::Combine(FPaths::ProjectContentDir(), PackageName + TEXT(".uasset"));
        
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[TIMESTAMP] Checking Game Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
        
        if (FPaths::FileExists(AssetFilePath))
        {
            FDateTime ModifyTime = IFileManager::Get().GetTimeStamp(*AssetFilePath);
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[TIMESTAMP] Blueprint modify time: %s"), *ModifyTime.ToString());
            return ModifyTime;
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[TIMESTAMP] Game Blueprint file not found: %s"), *AssetFilePath);
        }
    }
    
    else if (NormalizedAssetPath.StartsWith(TEXT("/")) && NormalizedAssetPath.Contains(TEXT("/")))
    {
        
        FString PackageName = NormalizedAssetPath;
        FString AssetFilePath;
        
        
        if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, AssetFilePath, TEXT(".uasset")))
        {
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[TIMESTAMP] Checking Plugin Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
            
            if (FPaths::FileExists(AssetFilePath))
            {
                FDateTime ModifyTime = IFileManager::Get().GetTimeStamp(*AssetFilePath);
                UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[TIMESTAMP] Plugin Blueprint modify time: %s"), *ModifyTime.ToString());
                return ModifyTime;
            }
            else
            {
                UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[TIMESTAMP] Plugin Blueprint file not found: %s"), *AssetFilePath);
            }
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[TIMESTAMP] Failed to convert package name to file path: %s"), *AssetPath);
        }
    }
    
    else if (AssetPath.StartsWith(TEXT("/Native/")))
    {
        FString ModuleFilename = FModuleManager::Get().GetModuleFilename("EmmyLuaIntelliSense");
        if (FPaths::FileExists(ModuleFilename))
        {
            return FDateTime::FromUnixTimestamp(FPlatformFileManager::Get().GetPlatformFile().GetStatData(*ModuleFilename).ModificationTime.ToUnixTimestamp());
        }
    }
    
    
    return FDateTime::Now();
}

void ULuaExportManager::LoadExcludedPathsFromFile(TArray<FString>& OutExcludedPaths) const
{
    
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("EmmyLuaIntelliSense"));
    FString ConfigFilePath;
    
    if (Plugin.IsValid())
    {
        FString PluginDir = Plugin->GetBaseDir();
        ConfigFilePath = FPaths::Combine(PluginDir, TEXT("Resources"), TEXT("ExcludedPaths.json"));
    }
    else
    {
        
        ConfigFilePath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("EmmyLuaIntelliSense"), TEXT("ExcludedPaths.json"));
    }

    if (!FPaths::FileExists(ConfigFilePath))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[CONFIG] Excluded paths config file not found, using default exclusions: %s"), *ConfigFilePath);
        return;
    }
    
    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *ConfigFilePath))
    {
        return;
    }

    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return;
    }
    
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
        
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[CONFIG] Loaded %d excluded paths from config file"), OutExcludedPaths.Num());
        
        
        for (int32 i = 0; i < FMath::Min(3, OutExcludedPaths.Num()); i++)
        {
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[CONFIG] Sample path [%d]: %s"), i, *OutExcludedPaths[i]);
        }
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
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[HASH] Failed to load file for hashing: %s"), *FilePath);
        return FString();
    }
    
    
    FSHA1 Sha1;
    Sha1.Update(FileData.GetData(), FileData.Num());
    Sha1.Final();
    
    
    FString HashString;
    for (int32 i = 0; i < 20; ++i)
    {
        HashString += FString::Printf(TEXT("%02x"), Sha1.m_digest[i]);
    }
    
    return HashString;
}

FString ULuaExportManager::GetAssetHash(const FString& AssetPath) const
{
    
    
    FString NormalizedAssetPath = AssetPath;
    int32 LastDotIndex;
    if (NormalizedAssetPath.FindLastChar('.', LastDotIndex))
    {
        FString ClassName = NormalizedAssetPath.Mid(LastDotIndex + 1);
        FString PathWithoutClass = NormalizedAssetPath.Left(LastDotIndex);
        
        
        if (PathWithoutClass.EndsWith("/" + ClassName))
        {
            NormalizedAssetPath = PathWithoutClass;
            UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[HASH] Normalized blueprint path: %s -> %s"), *AssetPath, *NormalizedAssetPath);
        }
    }
    
    
    if (NormalizedAssetPath.StartsWith(TEXT("/Game/")))
    {
        FString PackageName = NormalizedAssetPath.RightChop(6); 
        FString AssetFilePath = FPaths::Combine(FPaths::ProjectContentDir(), PackageName + TEXT(".uasset"));
        
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[HASH] Calculating hash for Game Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
        
        FString Hash = CalculateFileHash(AssetFilePath);
        if (!Hash.IsEmpty())
        {
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[HASH] Game Blueprint hash: %s"), *Hash);
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[HASH] Failed to calculate hash for Game Blueprint: %s"), *AssetFilePath);
        }
        
        return Hash;
    }
    
    else if (NormalizedAssetPath.StartsWith(TEXT("/")) && NormalizedAssetPath.Contains(TEXT("/")))
    {
        
        FString PackageName = NormalizedAssetPath;
        FString AssetFilePath;
        
        
        if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, AssetFilePath, TEXT(".uasset")))
        {
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[HASH] Calculating hash for Plugin Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
            
            FString Hash = CalculateFileHash(AssetFilePath);
            if (!Hash.IsEmpty())
            {
                UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[HASH] Plugin Blueprint hash: %s"), *Hash);
            }
            else
            {
                UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[HASH] Failed to calculate hash for Plugin Blueprint: %s"), *AssetFilePath);
            }
            
            return Hash;
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[HASH] Failed to convert package name to file path: %s"), *AssetPath);
        }
    }
    
    
    FString TypeInfo = AssetPath + TEXT("_") + FDateTime::Now().ToString();
    return FMD5::HashAnsiString(*TypeInfo);
}

bool ULuaExportManager::ShouldReexportByHash(const FString& AssetPath, const FString& AssetHash) const
{
    const FString* CachedHash = ExportedFilesHashCache.Find(AssetPath);
    
    if (!CachedHash)
    {
        
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[REEXPORT_HASH] No hash cache found for %s, will export"), *AssetPath);
        return true;
    }
    
    
    bool bShouldReexport = AssetHash != *CachedHash;
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[REEXPORT_HASH] %s: Asset hash=%s, Cache hash=%s, Should reexport=%s"), 
        *AssetPath, 
        *AssetHash, 
        **CachedHash, 
        bShouldReexport ? TEXT("YES") : TEXT("NO"));
    
    return bShouldReexport;
}

void ULuaExportManager::UpdateExportCacheByHash(const FString& AssetPath, const FString& AssetHash)
{
    ExportedFilesHashCache.Add(AssetPath, AssetHash);
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[CACHE_HASH] Updated hash cache for %s: %s"), *AssetPath, *AssetHash);
}

bool ULuaExportManager::ShouldExcludeFromExport(const FString& AssetPath) const
{
    
    static TSet<FString> ExcludedPaths;
    static bool bPathsLoaded = false;
    
    if (!bPathsLoaded)
    {
        TArray<FString> TempExcludedPaths;
        LoadExcludedPathsFromFile(TempExcludedPaths);
        ExcludedPaths = TSet<FString>(TempExcludedPaths);
        bPathsLoaded = true;
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[EXCLUDE] Loaded %d excluded paths for filtering"), ExcludedPaths.Num());
        
        
        int32 Count = 0;
        for (const FString& Path : ExcludedPaths)
        {
            if (Count >= 5) break;
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[EXCLUDE] Sample excluded path [%d]: %s"), Count, *Path);
            Count++;
        }
    }
    
    
    
    FString CurrentPath = AssetPath;
    
    
    if (ExcludedPaths.Contains(CurrentPath))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("[EXCLUDE] Path excluded (exact match): %s"), *AssetPath);
        return true;
    }
    
    
    int32 LastSlashIndex;
    while (CurrentPath.FindLastChar('/', LastSlashIndex) && LastSlashIndex > 0)
    {
        CurrentPath = CurrentPath.Left(LastSlashIndex);
        if (ExcludedPaths.Contains(CurrentPath))
        {
            UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("[EXCLUDE] Path excluded (parent match): %s (matched: %s)"), *AssetPath, *CurrentPath);
            return true;
        }
    }
    
    UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[EXCLUDE] Path allowed: %s"), *AssetPath);
    return false;
}

void ULuaExportManager::CopyUELibFolder() const
{
    
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("EmmyLuaIntelliSense"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogEmmyLuaIntelliSense, Error, TEXT("[COPY_UELIB] Failed to find EmmyLuaIntelliSense plugin"));
        return;
    }
    
    FString PluginDir = Plugin->GetBaseDir();
    FString SourceUELibDir = FPaths::Combine(PluginDir, TEXT("Resources"), TEXT("UELib"));
    FString TargetUELibDir = FPaths::Combine(OutputDir, TEXT("UELib"));
    
    
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*SourceUELibDir))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("[COPY_UELIB] Source UELib directory does not exist: %s"), *SourceUELibDir);
        return;
    }
    
    
    if (!PlatformFile.DirectoryExists(*TargetUELibDir))
    {
        if (!PlatformFile.CreateDirectoryTree(*TargetUELibDir))
        {
            UE_LOG(LogEmmyLuaIntelliSense, Error, TEXT("[COPY_UELIB] Failed to create target directory: %s"), *TargetUELibDir);
            return;
        }
    }
    
    
    class FUELibCopyVisitor : public IPlatformFile::FDirectoryVisitor
    {
    public:
        FString SourceDir;
        FString TargetDir;
        IPlatformFile* PlatformFile;
        int32 CopiedFiles;
        
        FUELibCopyVisitor(const FString& InSourceDir, const FString& InTargetDir, IPlatformFile* InPlatformFile)
            : SourceDir(InSourceDir), TargetDir(InTargetDir), PlatformFile(InPlatformFile), CopiedFiles(0)
        {
        }
        
        virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
        {
            FString FullPath(FilenameOrDirectory);
            FString RelativePath = FullPath.RightChop(SourceDir.Len() + 1); 
            FString TargetPath = FPaths::Combine(TargetDir, RelativePath);
            
            if (bIsDirectory)
            {
                
                if (!PlatformFile->DirectoryExists(*TargetPath))
                {
                    if (!PlatformFile->CreateDirectoryTree(*TargetPath))
                    {
                        UE_LOG(LogEmmyLuaIntelliSense, Error, TEXT("[COPY_UELIB] Failed to create directory: %s"), *TargetPath);
                        return false;
                    }
                }
            }
            else
            {
                
                if (PlatformFile->CopyFile(*TargetPath, *FullPath))
                {
                    CopiedFiles++;
                    UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("[COPY_UELIB] Copied file: %s -> %s"), *FullPath, *TargetPath);
                }
                else
                {
                    UE_LOG(LogEmmyLuaIntelliSense, Error, TEXT("[COPY_UELIB] Failed to copy file: %s -> %s"), *FullPath, *TargetPath);
                }
            }
            
            return true;
        }
    };
    
    FUELibCopyVisitor CopyVisitor(SourceUELibDir, TargetUELibDir, &PlatformFile);
    PlatformFile.IterateDirectoryRecursively(*SourceUELibDir, CopyVisitor);
    
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[COPY_UELIB] Successfully copied UELib folder from %s to %s (%d files)"), 
        *SourceUELibDir, *TargetUELibDir, CopyVisitor.CopiedFiles);
}