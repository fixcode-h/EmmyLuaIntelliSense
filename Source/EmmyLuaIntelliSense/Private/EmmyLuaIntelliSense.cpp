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

// 定义插件专属的日志类别
DEFINE_LOG_CATEGORY(LogEmmyLuaIntelliSense);

void FEmmyLuaIntelliSenseModule::StartupModule()
{
	// 注册插件设置
	RegisterSettings();
	
	// 注册引擎初始化完成后的回调
	FCoreDelegates::OnPostEngineInit.AddRaw(this, &FEmmyLuaIntelliSenseModule::OnPostEngineInit);
}

void FEmmyLuaIntelliSenseModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("EmmyLuaIntelliSense module shutting down..."));
	
	// 注销插件设置
	UnregisterSettings();
	
	// EditorSubsystem会自动清理，无需手动处理
	
	// 取消注册回调
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);
	
	// 取消注册AssetRegistry回调
	if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		AssetRegistry.OnFilesLoaded().RemoveAll(this);
	}
}

void FEmmyLuaIntelliSenseModule::OnAssetRegistryFilesLoaded()
{
	// 资产注册表已加载完成，可以安全地初始化导出管理器
	InitializeLuaExportManager();
}

void FEmmyLuaIntelliSenseModule::OnPostEngineInit()
{
	// 确保在编辑器环境中运行
	if (!GIsEditor)
	{
		return;
	}
	
	// 防止重复初始化
	if (bIsInitialized)
	{
		UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Already initialized, skipping"));
		return;
	}
	
	// 使用AssetRegistry的OnFilesLoaded回调，确保所有资产都已加载
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 检查资产注册表是否已经加载完成
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
	// 设置初始化标志
	bIsInitialized = true;
	
	// EditorSubsystem会自动初始化，无需手动调用Initialize
	ULuaExportManager* ExportManager = ULuaExportManager::Get();
	if (!ExportManager)
	{
		UE_LOG(LogEmmyLuaIntelliSense, Error, TEXT("Failed to get ULuaExportManager instance"));
		return;
	}
	
	// 扫描现有的蓝图和类型，添加到待导出列表
	ExportManager->ScanExistingAssets();
	
	ShowExportDialogIfNeeded();
}

void FEmmyLuaIntelliSenseModule::ShowExportDialogIfNeeded()
{
	// 检查是否有待导出的变化
	ULuaExportManager* ExportManager = ULuaExportManager::Get();
	if (ExportManager && ExportManager->HasPendingChanges())
	{
		int32 BlueprintCount = ExportManager->GetPendingBlueprintsCount();
		int32 NativeTypeCount = ExportManager->GetPendingNativeTypesCount();
		int32 CoreFileCount = ExportManager->GetPendingCoreFilesCount();
		int32 TotalCount = BlueprintCount + NativeTypeCount + CoreFileCount;
		
		UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("Showing Lua export dialog due to pending changes (%d blueprints, %d native types, %d core files, %d total files)..."), BlueprintCount, NativeTypeCount, CoreFileCount, TotalCount);
		
		// 显示简化的导出确认对话框，分别显示蓝图、原生类型和核心文件数量
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