// Copyright Epic Games, Inc. All Rights Reserved.

#include "EmmyLuaIntelliSense.h"
#include "LuaExportManager.h"
#include "LuaExportDialog.h"
#include "EmmyLuaIntelliSenseSettings.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/CoreDelegates.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ISettingsModule.h"

#define LOCTEXT_NAMESPACE "FEmmyLuaIntelliSenseModule"

DEFINE_LOG_CATEGORY(LogEmmyLuaIntelliSense);

void FEmmyLuaIntelliSenseModule::StartupModule()
{
	RegisterSettings();
	
	FCoreDelegates::OnPostEngineInit.AddRaw(this, &FEmmyLuaIntelliSenseModule::OnPostEngineInit);
}

void FEmmyLuaIntelliSenseModule::ShutdownModule()
{
	UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("EmmyLuaIntelliSense module shutting down..."));
	
	// 清理通知管理器
	FLuaExportNotificationManager::Cleanup();
	
	UnregisterSettings();
	
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);
	
	if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		AssetRegistry.OnFilesLoaded().RemoveAll(this);
	}
}

void FEmmyLuaIntelliSenseModule::OnAssetRegistryFilesLoaded()
{
	InitializeLuaExportManager();
}

void FEmmyLuaIntelliSenseModule::OnPostEngineInit()
{
	if (!GIsEditor)
	{
		return;
	}
	
	if (bIsInitialized)
	{
		UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Already initialized, skipping"));
		return;
	}
	
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	if (AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.OnFilesLoaded().AddRaw(this, &FEmmyLuaIntelliSenseModule::OnAssetRegistryFilesLoaded);
	}
	else
	{
		OnAssetRegistryFilesLoaded();
	}
}

void FEmmyLuaIntelliSenseModule::InitializeLuaExportManager()
{
	bIsInitialized = true;
	
	ULuaExportManager* ExportManager = ULuaExportManager::Get();
	if (!ExportManager)
	{
		UE_LOG(LogEmmyLuaIntelliSense, Error, TEXT("Failed to get ULuaExportManager instance"));
		return;
	}

#if !PLATFORM_WINDOWS
	return;
#endif
	
	if (IsRunningCommandlet() || !GIsEditor)
	{
		return ;
	}

	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	const UEmmyLuaIntelliSenseSettings* Settings = UEmmyLuaIntelliSenseSettings::Get();
	if (Settings && Settings->bAutoStartScanOnStartup)
	{
		ExportManager->ScanExistingAssetsAsync();
	}
	else
	{
		FLuaExportDialog::ShowScanConfirmation();
	}
}

void FEmmyLuaIntelliSenseModule::ShowExportDialogIfNeeded()
{
	ULuaExportManager* ExportManager = ULuaExportManager::Get();
	if (ExportManager && ExportManager->HasPendingChanges())
	{
		int32 BlueprintCount = ExportManager->GetPendingBlueprintsCount();
		int32 NativeTypeCount = ExportManager->GetPendingNativeTypesCount();
		int32 CoreFileCount = ExportManager->GetPendingCoreFilesCount();
		int32 TotalCount = BlueprintCount + NativeTypeCount + CoreFileCount;
		
		UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Showing Lua export dialog due to pending changes (%d blueprints, %d native types, %d core files, %d total files)..."), BlueprintCount, NativeTypeCount, CoreFileCount, TotalCount);
		
		FLuaExportDialog::ShowExportConfirmation(BlueprintCount, NativeTypeCount, CoreFileCount);
	}
	else
	{
		UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("No pending changes, skipping export dialog."));
	}
}

void FEmmyLuaIntelliSenseModule::RegisterSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "EmmyLuaIntelliSense",
			NSLOCTEXT("EmmyLuaIntelliSenseSettings", "SettingsName", "Emmy Lua IntelliSense"),
			NSLOCTEXT("EmmyLuaIntelliSenseSettings", "SettingsDescription", "Configure Emmy Lua IntelliSense export settings"),
			UEmmyLuaIntelliSenseSettings::GetMutable()
		);
	}
}

void FEmmyLuaIntelliSenseModule::UnregisterSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->UnregisterSettings("Project", "Plugins", "EmmyLuaIntelliSense");
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FEmmyLuaIntelliSenseModule, EmmyLuaIntelliSense)