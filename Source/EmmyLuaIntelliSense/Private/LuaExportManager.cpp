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
#include "EmmyLuaIntelliSense.h"
#include "Modules/ModuleManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/ScopedSlowTask.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Async/AsyncWork.h"
#include "Async/Async.h"
#include "TimerManager.h"
#include "Editor.h"

ULuaExportManager::ULuaExportManager()
    : bInitialized(false)
    , bIsAsyncScanningInProgress(false)
    , bScanCancelled(false)
    , bIsFramedProcessingInProgress(false)
    , CurrentBlueprintIndex(0)
    , CurrentNativeTypeIndex(0)
{
    FieldHashCache.Empty();
    FieldHashCacheTimestamp.Empty();
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
    SaveExportCache();
    bInitialized = false;
    PendingBlueprints.Empty();
    PendingNativeTypes.Empty();
    ExportedFilesHashCache.Empty();
    FieldHashCache.Empty();
    FieldHashCacheTimestamp.Empty();
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
    int32 ExportedCount = 0; // 添加导出计数器
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
                    ExportedCount++; // 增加导出计数
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
            ExportedCount++; // 增加导出计数
        }
        SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("正在导出UE核心类型...")));
        ExportUETypes(NativeTypes);
        ExportedCount++; // UE核心类型也算一项
        FString Message = FString::Printf(TEXT("Lua IntelliSense文件导出完成，共导出 %d 项！"), ExportedCount);
        FLuaExportNotificationManager::ShowExportSuccess(Message);
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Full Lua export completed. Exported %d items."), ExportedCount);
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
		FString BlueprintHash = GetAssetHash(BlueprintPath);
		UpdateExportCacheByHash(BlueprintPath, BlueprintHash);
		UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[EXPORT] Updated cache for Blueprint: %s with hash: %s"), *BlueprintPath, *BlueprintHash);
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
    FString NativeTypePath = Field->GetPathName();
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
            FString FieldHash = GetCachedFieldHash(Field);
            UpdateExportCacheByHash(NativeTypePath, FieldHash);
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[EXPORT] Updated cache for Native Type: %s with hash: %s"), *NativeTypePath, *FieldHash);
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
void ULuaExportManager::ScanExistingAssets()
{
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[SCAN] Starting optimized async asset scanning..."));
    ScanExistingAssetsAsync();
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
    ExportedFilesHashCache.Empty();
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
    const TSharedPtr<FJsonObject>* HashCachePtr = nullptr;
    if (JsonObject->TryGetObjectField(TEXT("HashCache"), HashCachePtr))
    {
        if (HashCachePtr && HashCachePtr->IsValid())
        {
            for (const auto& Pair : (*HashCachePtr)->Values)
            {
                if (ShouldExcludeFromExport(Pair.Key))
                {
                    FilteredCount++;
                    continue;
                }
                FString HashString = Pair.Value->AsString();
                if (!HashString.IsEmpty())
                {
                    ExportedFilesHashCache.Add(Pair.Key, HashString);
                }
            }
        }
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Loaded hash cache: %d hash entries"), 
            ExportedFilesHashCache.Num());
    }
    else
    {
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Old format cache detected, starting fresh with hash-based caching"));
    }
    double ProcessEndTime = FPlatformTime::Seconds();
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Cache processing took: %.3f ms, filtered %d excluded paths"), (ProcessEndTime - ProcessStartTime) * 1000.0, FilteredCount);
    double TotalTime = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("LoadExportCache completed: %d hash entries loaded in %.3f ms"), 
        ExportedFilesHashCache.Num(), TotalTime * 1000.0);
}
void ULuaExportManager::SaveExportCache()
{
    double StartTime = FPlatformTime::Seconds();
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Saving export cache with %d hash entries to: %s"), 
        ExportedFilesHashCache.Num(), *ExportCacheFilePath);
    FString CacheDir = FPaths::GetPath(ExportCacheFilePath);
    if (!FPaths::DirectoryExists(CacheDir))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Creating cache directory: %s"), *CacheDir);
        IFileManager::Get().MakeDirectory(*CacheDir, true);
    }
    double SerializeStartTime = FPlatformTime::Seconds();
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
    TSharedPtr<FJsonObject> HashCache = MakeShareable(new FJsonObject);
    for (const auto& Pair : ExportedFilesHashCache)
    {
        HashCache->SetStringField(Pair.Key, Pair.Value);
    }
    JsonObject->SetObjectField(TEXT("HashCache"), HashCache);
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
bool ULuaExportManager::ShouldReexport(const FString& AssetPath, const FString& AssetFilePath) const
{
    FString AssetHash = CalculateFileHash(AssetFilePath);
    if (AssetHash.IsEmpty())
    {
        UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[REEXPORT] Failed to get hash for file %s (asset: %s), will export"), *AssetFilePath, *AssetPath);
        return true;
    }
    return ShouldReexportByHash(AssetPath, AssetHash);
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
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[HASH] Failed to load file for hashing: %s"), *FilePath);
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
FString ULuaExportManager::CalculateClassStructureHash(const UClass* Class) const
{
    if (!Class || !IsValid(Class) || Class->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
    {
        return FString();
    }
    FString SignatureString;
    SignatureString += FString::Printf(TEXT("ClassName:%s;"), *Class->GetName());
    if (const UClass* SuperClass = Class->GetSuperClass())
    {
        SignatureString += FString::Printf(TEXT("ParentClass:%s;"), *SuperClass->GetName());
    }
    else
    {
        SignatureString += TEXT("ParentClass:None;");
    }
    SignatureString += TEXT("Properties:[");
    for (TFieldIterator<FProperty> PropertyIt(Class, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt)
    {
        FProperty* Property = *PropertyIt;
        if (!Property)
        {
            continue;
        }
        SignatureString += FString::Printf(TEXT("Name:%s,"), *Property->GetName());
        SignatureString += FString::Printf(TEXT("Type:%s,"), *Property->GetClass()->GetName());
        SignatureString += TEXT(";");
    }
    SignatureString += TEXT("];");
    SignatureString += TEXT("Functions:[");
    for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
    {
        UFunction* Function = *FunctionIt;
        if (!Function)
        {
            continue;
        }
        SignatureString += FString::Printf(TEXT("Name:%s,"), *Function->GetName());
        if (FProperty* ReturnProperty = Function->GetReturnProperty())
        {
            SignatureString += FString::Printf(TEXT("ReturnType:%s,"), *ReturnProperty->GetClass()->GetName());
        }
        else
        {
            SignatureString += TEXT("ReturnType:void,");
        }
        SignatureString += TEXT("Params:[");
        for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
        {
            FProperty* Param = *ParamIt;
            if (!Param || Param->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                continue;
            }
            SignatureString += FString::Printf(TEXT("ParamName:%s,ParamType:%s;"), *Param->GetName(), *Param->GetClass()->GetName());
        }
        SignatureString += TEXT("],");
        SignatureString += TEXT(";");
    }
    SignatureString += TEXT("];");
    FSHA1 Sha1;
    FTCHARToUTF8 UTF8String(*SignatureString);
    Sha1.Update((const uint8*)UTF8String.Get(), UTF8String.Length());
    Sha1.Final();
    FString HashString;
    for (int32 i = 0; i < 20; ++i)
    {
        HashString += FString::Printf(TEXT("%02x"), Sha1.m_digest[i]);
    }
    UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[STRUCTURE_HASH] Class %s signature: %s"), *Class->GetName(), *SignatureString);
    UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[STRUCTURE_HASH] Class %s hash: %s"), *Class->GetName(), *HashString);
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
        UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[HASH] Calculating hash for Game Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
        FString Hash = CalculateFileHash(AssetFilePath);
        if (!Hash.IsEmpty())
        {
            UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[HASH] Game Blueprint hash: %s"), *Hash);
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[HASH] Failed to calculate hash for Game Blueprint: %s"), *AssetFilePath);
        }
        return Hash;
    }
    else if (NormalizedAssetPath.StartsWith(TEXT("/")) && NormalizedAssetPath.Contains(TEXT("/")))
    {
        FString PackageName = NormalizedAssetPath;
        FString AssetFilePath;
        if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, AssetFilePath, TEXT(".uasset")))
        {
            UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[HASH] Calculating hash for Plugin Blueprint file: %s -> %s"), *AssetPath, *AssetFilePath);
            FString Hash = CalculateFileHash(AssetFilePath);
            if (!Hash.IsEmpty())
            {
                UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[HASH] Plugin Blueprint hash: %s"), *Hash);
            }
            else
            {
                UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[HASH] Failed to calculate hash for Plugin Blueprint: %s"), *AssetFilePath);
            }
            return Hash;
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[HASH] Failed to convert package name to file path: %s"), *AssetPath);
        }
    }
    FString TypeInfo = AssetPath + TEXT("_") + FDateTime::Now().ToString();
    return FMD5::HashAnsiString(*TypeInfo);
}
FString ULuaExportManager::GetAssetHash(const UField* Field) const
{
    if (!Field || !IsValid(Field) || Field->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
    {
        return FString();
    }
    if (const UClass* Class = Cast<UClass>(Field))
    {
        FString StructureHash = CalculateClassStructureHash(Class);
        UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[HASH] Native Class structure hash for %s: %s"), *Class->GetName(), *StructureHash);
        return StructureHash;
    }
    FString SignatureString = FString::Printf(TEXT("FieldType:%s;FieldName:%s;"), *Field->GetClass()->GetName(), *Field->GetName());
    if (const UStruct* Struct = Cast<UStruct>(Field))
    {
        SignatureString += TEXT("Properties:[");
        for (TFieldIterator<FProperty> PropertyIt(Struct, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt)
        {
            FProperty* Property = *PropertyIt;
            if (Property)
            {
                SignatureString += FString::Printf(TEXT("Name:%s,Type:%s;"), *Property->GetName(), *Property->GetClass()->GetName());
            }
        }
        SignatureString += TEXT("];");
    }
    if (const UEnum* Enum = Cast<UEnum>(Field))
    {
        SignatureString += TEXT("EnumValues:[");
        for (int32 i = 0; i < Enum->NumEnums(); ++i)
        {
            SignatureString += FString::Printf(TEXT("Name:%s,Value:%lld;"), *Enum->GetNameStringByIndex(i), Enum->GetValueByIndex(i));
        }
        SignatureString += TEXT("];");
    }
    FSHA1 Sha1;
    FTCHARToUTF8 UTF8String(*SignatureString);
    Sha1.Update((const uint8*)UTF8String.Get(), UTF8String.Length());
    Sha1.Final();
    FString HashString;
    for (int32 i = 0; i < 20; ++i)
    {
        HashString += FString::Printf(TEXT("%02x"), Sha1.m_digest[i]);
    }
    UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[HASH] Native Field structure hash for %s (%s): %s"), *Field->GetName(), *Field->GetClass()->GetName(), *HashString);
    return HashString;
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
    if (bShouldReexport)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("[REEXPORT_HASH] %s: Asset hash=%s, Cache hash=%s, Should reexport=YES"), 
            *AssetPath, 
            *AssetHash, 
            **CachedHash);
    }
    else
    {
        UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[REEXPORT_HASH] %s: Asset hash=%s, Cache hash=%s, Should reexport=NO"), 
            *AssetPath, 
            *AssetHash, 
            **CachedHash);
    }
    return bShouldReexport;
}
void ULuaExportManager::UpdateExportCacheByHash(const FString& AssetPath, const FString& AssetHash)
{
    ExportedFilesHashCache.Add(AssetPath, AssetHash);
    UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("[CACHE_HASH] Updated hash cache for %s: %s"), *AssetPath, *AssetHash);
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
void ULuaExportManager::ScanExistingAssetsAsync()
{
    if (bIsAsyncScanningInProgress)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Async scanning already in progress, skipping..."));
        return;
    }
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Starting async asset scanning..."));
    bIsAsyncScanningInProgress = true;
    bScanCancelled = false;
    ScanProgressNotification = FLuaExportNotificationManager::ShowScanProgress(TEXT("正在扫描资源..."));
    
    // 使用异步任务执行扫描
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
    {
        TArray<FAssetData> BlueprintAssets;
        TArray<const UField*> NativeTypes;
        
        // 在后台线程执行扫描
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
        AssetRegistryModule.Get().GetAssets(Filter, BlueprintAssets);
        CollectNativeTypes(NativeTypes);
        
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Asset scanning completed. Found %d blueprints, %d native types"), 
               BlueprintAssets.Num(), NativeTypes.Num());
        
        // 回到主线程处理结果
        AsyncTask(ENamedThreads::GameThread, [this, BlueprintAssets, NativeTypes]()
        {
            OnAsyncScanCompleted(BlueprintAssets, NativeTypes);
        });
    });
}
void ULuaExportManager::OnAsyncScanCompleted(const TArray<FAssetData>& BlueprintAssets, const TArray<const UField*>& NativeTypes)
{
    if (!IsValid(this))
    {
        return;
    }
    if (bScanCancelled)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Asset scanning was cancelled by user"));
        FLuaExportNotificationManager::CompleteScanProgressNotification(ScanProgressNotification, TEXT("扫描已取消"), false);
        bIsAsyncScanningInProgress = false;
        bScanCancelled = false;
        ScanProgressNotification.Reset();
        return;
    }
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Async scan completed. Found %d blueprints, %d native types"), BlueprintAssets.Num(), NativeTypes.Num());
    FLuaExportNotificationManager::UpdateScanProgressNotification(ScanProgressNotification, TEXT("正在分析需要导出的资源..."), 0.8f);
    PendingBlueprints.Empty();
    PendingNativeTypes.Empty();
    for (const FAssetData& AssetData : BlueprintAssets)
    {
        if (AddToPendingBlueprints(AssetData))
        {
            UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("Added blueprint to pending list: %s"), *AssetData.AssetName.ToString());
        }
        else
        {
            FString AssetHash = GetAssetHash(AssetData.ObjectPath.ToString());
            if (!AssetHash.IsEmpty())
            {
                UpdateExportCacheByHash(AssetData.ObjectPath.ToString(), AssetHash);
                UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("Updated cache for blueprint (no export needed): %s"), *AssetData.AssetName.ToString());
            }
        }
    }
    for (const UField* Field : NativeTypes)
    {
        if (Field)
        {
            FString FieldName;
            if (IsValidFieldForExport(Field, FieldName))
            {
                FString FieldHash = GetCachedFieldHash(Field);
                FString NativeTypePath = Field->GetPathName();
                if (ShouldReexportByHash(NativeTypePath, FieldHash))
                {
                    PendingNativeTypes.Add(Field);
                    UE_LOG(LogEmmyLuaIntelliSense, Verbose, TEXT("Added native type to pending list: %s"), *Field->GetName());
                }
                else
                {
                    UpdateExportCacheByHash(NativeTypePath, FieldHash);
                    UE_LOG(LogEmmyLuaIntelliSense, VeryVerbose, TEXT("Updated cache for native type (no export needed): %s"), *Field->GetName());
                }
            }
        }
    }
    bIsAsyncScanningInProgress = false;
    FLuaExportNotificationManager::CompleteScanProgressNotification(ScanProgressNotification, TEXT("扫描完成"), true);
    ScanProgressNotification.Reset();
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Scan analysis completed. Pending exports: %d blueprints, %d native types"), 
           PendingBlueprints.Num(), PendingNativeTypes.Num());
    SaveExportCache();
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Export cache updated after scan"));
    
    // 检查是否有待导出的文件
    if (HasPendingChanges())
    {
        // 获取设置以确定后续行为
        const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
        if (Settings && Settings->bAutoStartScanOnStartup)
        {
            // 自动扫描模式：显示导出确认对话框让用户选择
            if (FModuleManager::Get().IsModuleLoaded("EmmyLuaIntelliSense"))
            {
                FEmmyLuaIntelliSenseModule& Module = FModuleManager::GetModuleChecked<FEmmyLuaIntelliSenseModule>("EmmyLuaIntelliSense");
                Module.ShowExportDialogIfNeeded();
            }
        }
        else
        {
            // 手动扫描模式：用户已经确认要扫描，直接进行导出
            UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Manual scan completed, starting automatic export..."));
            ExportIncremental();
        }
    }
    else
    {
        UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("No pending changes after scan, no export needed."));
    }
}

void ULuaExportManager::CancelAsyncScan()
{
    if (!bIsAsyncScanningInProgress)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("No async scanning in progress to cancel"));
        return;
    }
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Cancelling async asset scanning..."));
    bScanCancelled = true;
}
void ULuaExportManager::StartFramedProcessing()
{
    if (bIsFramedProcessingInProgress)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("Framed processing is already in progress"));
        return;
    }
    bIsFramedProcessingInProgress = true;
    CurrentBlueprintIndex = 0;
    CurrentNativeTypeIndex = 0;
    if (ScanProgressNotification.IsValid())
    {
        ScanProgressNotification->SetText(FText::FromString(TEXT("开始处理扫描结果...")));
        ScanProgressNotification->SetCompletionState(SNotificationItem::CS_Pending);
    }
    if (UWorld* World = GEngine->GetCurrentPlayWorld())
    {
        World->GetTimerManager().SetTimer(
            FramedProcessingTimerHandle,
            this,
            &ULuaExportManager::ProcessFramedStepWrapper,
            0.016f,
            true
        );
    }
    else
    {
        if (GEditor && GEditor->GetEditorWorldContext().World())
        {
            GEditor->GetEditorWorldContext().World()->GetTimerManager().SetTimer(
                FramedProcessingTimerHandle,
                this,
                &ULuaExportManager::ProcessFramedStepWrapper,
                0.016f,
                true
            );
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("No world available for timer, processing all at once"));
            while (ProcessFramedStep())
            {
            }
        }
    }
}
bool ULuaExportManager::ProcessFramedStep()
{
    if (!bIsFramedProcessingInProgress)
    {
        return false;
    }
    const int32 ItemsPerFrame = 5;
    int32 ProcessedThisFrame = 0;
    while (CurrentBlueprintIndex < ScannedBlueprintAssets.Num() && ProcessedThisFrame < ItemsPerFrame)
    {
        const FAssetData& AssetData = ScannedBlueprintAssets[CurrentBlueprintIndex];
        if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset()))
        {
            ExportBlueprint(Blueprint);
        }
        CurrentBlueprintIndex++;
        ProcessedThisFrame++;
        if (ScanProgressNotification.IsValid())
        {
            int32 TotalItems = ScannedBlueprintAssets.Num() + ScannedNativeTypes.Num();
            int32 ProcessedItems = CurrentBlueprintIndex + CurrentNativeTypeIndex;
            float Progress = TotalItems > 0 ? (float)ProcessedItems / TotalItems : 1.0f;
            FString ProgressText = FString::Printf(
                TEXT("处理中... (%d/%d) - 蓝图: %d/%d, 原生类型: %d/%d"),
                ProcessedItems,
                TotalItems,
                CurrentBlueprintIndex,
                ScannedBlueprintAssets.Num(),
                CurrentNativeTypeIndex,
                ScannedNativeTypes.Num()
            );
            ScanProgressNotification->SetText(FText::FromString(ProgressText));
        }
    }
    while (CurrentNativeTypeIndex < ScannedNativeTypes.Num() && ProcessedThisFrame < ItemsPerFrame)
    {
        const UField* Field = ScannedNativeTypes[CurrentNativeTypeIndex];
        ExportNativeType(Field);
        CurrentNativeTypeIndex++;
        ProcessedThisFrame++;
        if (ScanProgressNotification.IsValid())
        {
            int32 TotalItems = ScannedBlueprintAssets.Num() + ScannedNativeTypes.Num();
            int32 ProcessedItems = CurrentBlueprintIndex + CurrentNativeTypeIndex;
            float Progress = TotalItems > 0 ? (float)ProcessedItems / TotalItems : 1.0f;
            FString ProgressText = FString::Printf(
                TEXT("处理中... (%d/%d) - 蓝图: %d/%d, 原生类型: %d/%d"),
                ProcessedItems,
                TotalItems,
                CurrentBlueprintIndex,
                ScannedBlueprintAssets.Num(),
                CurrentNativeTypeIndex,
                ScannedNativeTypes.Num()
            );
            ScanProgressNotification->SetText(FText::FromString(ProgressText));
        }
    }
    if (CurrentBlueprintIndex >= ScannedBlueprintAssets.Num() && 
        CurrentNativeTypeIndex >= ScannedNativeTypes.Num())
    {
        return false;
    }
    return true;
}
void ULuaExportManager::ProcessFramedStepWrapper()
{
    if (!ProcessFramedStep())
    {
        CompleteFramedProcessing();
    }
}
void ULuaExportManager::CompleteFramedProcessing()
{
    bIsFramedProcessingInProgress = false;
    if (UWorld* World = GEngine->GetCurrentPlayWorld())
    {
        World->GetTimerManager().ClearTimer(FramedProcessingTimerHandle);
    }
    else if (GEditor && GEditor->GetEditorWorldContext().World())
    {
        GEditor->GetEditorWorldContext().World()->GetTimerManager().ClearTimer(FramedProcessingTimerHandle);
    }
    ScannedBlueprintAssets.Empty();
    ScannedNativeTypes.Empty();
    CurrentBlueprintIndex = 0;
    CurrentNativeTypeIndex = 0;
    SaveExportCache();
    if (ScanProgressNotification.IsValid())
    {
        ScanProgressNotification->SetText(FText::FromString(TEXT("导出完成！")));
        ScanProgressNotification->SetCompletionState(SNotificationItem::CS_Success);
        ScanProgressNotification->ExpireAndFadeout();
        ScanProgressNotification.Reset();
    }
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Framed processing completed successfully"));
}
FString ULuaExportManager::GetCachedFieldHash(const UField* Field) const
{
    if (!Field)
    {
        return FString();
    }
    CleanupExpiredHashCache();
    if (FieldHashCache.Contains(Field))
    {
        double* CachedTime = FieldHashCacheTimestamp.Find(Field);
        if (CachedTime && (FPlatformTime::Seconds() - *CachedTime) < HASH_CACHE_EXPIRE_TIME)
        {
            return FieldHashCache[Field];
        }
    }
    FString Hash = GetAssetHash(Field);
    FieldHashCache.Add(Field, Hash);
    FieldHashCacheTimestamp.Add(Field, FPlatformTime::Seconds());
    return Hash;
}
void ULuaExportManager::CleanupExpiredHashCache() const
{
    double CurrentTime = FPlatformTime::Seconds();
    TArray<const UField*> ExpiredKeys;
    for (const auto& Pair : FieldHashCacheTimestamp)
    {
        if (CurrentTime - Pair.Value > HASH_CACHE_EXPIRE_TIME)
        {
            ExpiredKeys.Add(Pair.Key);
        }
    }
    for (const UField* Key : ExpiredKeys)
    {
        FieldHashCache.Remove(Key);
        FieldHashCacheTimestamp.Remove(Key);
    }
}